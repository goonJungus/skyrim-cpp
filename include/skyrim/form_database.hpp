// form_database.hpp - TESForm database loaded from the user's own ESM/ESP.
//
// This is the foundation every other subsystem reads from: cells and actors
// resolve record types, perks/skills resolve progression records, inventory
// resolves item records, magic resolves spells and effects, Papyrus resolves
// script records.
//
// Nothing here embeds game content. Forms are read at runtime from the user's
// installation (see SkyrimConfig::dataDirectory).

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "skyrim/skyrim_module.hpp"

namespace skyrim {

// ---------------------------------------------------------------------------
// FormID
// ---------------------------------------------------------------------------

// A 32-bit TES form reference. The high byte is the index of the source
// plugin in the load order; the low 24 bits are the form's index within it.
// This is the layout Bethesda uses for override resolution, so it is
// reproduced exactly rather than abstracted away.
using FormID = uint32_t;

constexpr uint8_t kMasterIndexNone = 0xFF;

constexpr FormID  kFormIDNull    = 0;
constexpr uint32_t kFormIndexMask = 0x00FFFFFF;

constexpr bool   isNull(FormID id)          { return id == kFormIDNull; }
constexpr uint8_t masterIndexOf(FormID id)  { return static_cast<uint8_t>(id >> 24); }
constexpr uint32_t formIndexOf(FormID id)   { return id & kFormIndexMask; }
constexpr FormID  makeFormID(uint8_t master, uint32_t index) {
    return (static_cast<FormID>(master) << 24) | (index & kFormIndexMask);
}

// Four-character record codes, packed big-endian so they read as 'CELL'.
using RecordType = uint32_t;
constexpr RecordType makeRecordType(char a, char b, char c, char d) {
    return (static_cast<RecordType>(static_cast<uint8_t>(a)) << 24) |
           (static_cast<RecordType>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<RecordType>(static_cast<uint8_t>(c)) <<  8) |
           (static_cast<RecordType>(static_cast<uint8_t>(d)));
}
// Character accessors for a packed record code ('C','E','L','L' -> CELL).
constexpr char typeChar0(RecordType t) { return static_cast<char>((t >> 24) & 0xFF); }
constexpr char typeChar1(RecordType t) { return static_cast<char>((t >> 16) & 0xFF); }
constexpr char typeChar2(RecordType t) { return static_cast<char>((t >>  8) & 0xFF); }
constexpr char typeChar3(RecordType t) { return static_cast<char>( t        & 0xFF); }
std::string toString(RecordType t);

// Common record codes used across the module.
namespace rec {
constexpr RecordType kTES4 = makeRecordType('T','E','S','4');
constexpr RecordType kGRUP = makeRecordType('G','R','U','P');
constexpr RecordType kCELL = makeRecordType('C','E','L','L');
constexpr RecordType kWRLD = makeRecordType('W','R','L','D');
constexpr RecordType kREFR = makeRecordType('R','E','F','R');
constexpr RecordType kACHR = makeRecordType('A','C','H','R');
constexpr RecordType kSTAT = makeRecordType('S','T','A','T');
constexpr RecordType kFACT = makeRecordType('F','A','C','T');
constexpr RecordType kNPC_ = makeRecordType('N','P','C','_');
constexpr RecordType kPERK = makeRecordType('P','E','R','K');
constexpr RecordType kSKIL = makeRecordType('S','K','I','L');
constexpr RecordType kSPEL = makeRecordType('S','P','E','L');
constexpr RecordType kSCPT = makeRecordType('S','C','P','T');
constexpr RecordType kMGEF = makeRecordType('M','G','E','F');
constexpr RecordType kALCH = makeRecordType('A','L','C','H');
constexpr RecordType kAMMO = makeRecordType('A','M','M','O');
constexpr RecordType kARMO = makeRecordType('A','R','M','O');
constexpr RecordType kWEAP = makeRecordType('W','E','A','P');
constexpr RecordType kKEYN = makeRecordType('K','E','Y','N');
constexpr RecordType kENCH = makeRecordType('E','N','C','H');
}  // namespace rec

// ---------------------------------------------------------------------------
// Subrecords
// ---------------------------------------------------------------------------

// A view into a record's decompressed payload. Non-owning; valid only while
// the owning TESForm's FormDatabase is alive and unmodified.
struct Subrecord {
    RecordType   type;
    const uint8_t* data;
    uint32_t     size;
};

namespace sub {
constexpr RecordType kEDID = makeRecordType('E','D','I','D');  // editor id (zstring)
constexpr RecordType kFULL = makeRecordType('F','U','L','L');  // display name
constexpr RecordType kNAME = makeRecordType('N','A','M','E');  // often an id ref
constexpr RecordType kDATA = makeRecordType('D','A','T','A');
constexpr RecordType kDESC = makeRecordType('D','E','S','C');
constexpr RecordType kSNAM = makeRecordType('S','N','A','M');  // master description
constexpr RecordType kCNAM = makeRecordType('C','N','A','M');  // master author
constexpr RecordType kHEDR = makeRecordType('H','E','D','R');  // header data
}  // namespace sub

// ---------------------------------------------------------------------------
// Localized strings (FULL / DESC)
// ---------------------------------------------------------------------------

// Skyrim SE (1.5.976+) does NOT store FULL/DESC as a NUL-terminated string.
// It stores a 4-byte reference into a string table that lives in the BSA
// archives (*.strings). Older records and some plugins still carry the text
// inline.
//
// Confirmed against a real master: all 21,606 FULL subrecords examined in one
// Skyrim.esm were exactly 4 bytes, i.e. every one was a reference. The two
// forms are therefore distinguished purely by subrecord length.
//
// Consequence: reading FULL as a plain string yields garbage. Use
// TESForm::displayNameRaw() and resolve the id through the BSA string table
// (see the not-yet-implemented archive reader) to get real text.
struct LocalizedString {
    bool        isReference = false;  // true => id into the BSA string table
    uint32_t    id   = 0;             // valid when isReference
    std::string text;                 // valid when !isReference
    bool empty() const { return !isReference && text.empty(); }
};

// Parse a FULL/DESC subrecord payload. Never throws.
LocalizedString parseLocalizedString(const uint8_t* data, uint32_t size);

// ---------------------------------------------------------------------------
// TESForm
// ---------------------------------------------------------------------------

// One record from a master or plugin. The payload is stored decompressed so
// subrecord access is uniform; see FormDatabase for the memory trade-off.
class TESForm {
public:
    FormID     id()     const { return m_id; }
    RecordType type()   const { return m_type; }
    uint32_t   flags()  const { return m_flags; }
    uint8_t    source() const { return m_source; }

    // True when the record was stored zlib-compressed and has been inflated.
    bool wasCompressed() const { return m_wasCompressed; }

    // Decompressed record payload.
    const uint8_t* payload()     const { return m_payload.data(); }
    uint32_t       payloadSize() const { return static_cast<uint32_t>(m_payload.size()); }

    // EDID / FULL as strings, or empty if absent. Never throws.
    //
    // editorId() is always a plain string. displayName() is only correct for
    // records whose FULL is inline; on Skyrim SE it is usually a 4-byte
    // reference and therefore returns empty. Prefer displayNameRaw().
    std::string editorId() const;
    std::string displayName() const;

    // FULL as a localized string: inline text, or a string-table id to be
    // resolved against the BSA string table.
    LocalizedString displayNameRaw() const;

    // DESC as a localized string (same encoding as FULL).
    LocalizedString descriptionRaw() const;

    // Walk subrecords in payload order. fn receives each Subrecord; returning
    // false from fn stops the walk early. Returns false if the payload is
    // structurally malformed.
    template <class Fn>
    bool forEachSubrecord(Fn&& fn) const {
        const uint8_t* p = m_payload.data();
        const uint8_t* end = p + m_payload.size();
        while (p + 6 <= end) {
            const RecordType t = (static_cast<RecordType>(p[0]) << 24) |
                                 (static_cast<RecordType>(p[1]) << 16) |
                                 (static_cast<RecordType>(p[2]) <<  8) |
                                  static_cast<RecordType>(p[3]);
            const uint32_t sz = static_cast<uint32_t>(p[4]) |
                                (static_cast<uint32_t>(p[5]) << 8);
            p += 6;
            if (static_cast<size_t>(end - p) < sz) return false;  // truncated
            if (!fn(Subrecord{t, p, sz})) return true;
            p += sz;
        }
        return p == end;
    }

private:
    friend class FormDatabase;

    FormID     m_id = kFormIDNull;
    RecordType m_type = 0;
    uint32_t   m_flags = 0;
    uint8_t    m_source = kMasterIndexNone;
    bool       m_wasCompressed = false;
    std::vector<uint8_t> m_payload;
};

// ---------------------------------------------------------------------------
// Source file tracking
// ---------------------------------------------------------------------------

// A loaded master or plugin, and its assigned index in load order.
struct SourceFile {
    std::string filename;      // e.g. "Skyrim.esm"
    uint8_t     index = kMasterIndexNone;
    bool        isMaster = false;
    uint64_t    recordCount = 0;
};

// ---------------------------------------------------------------------------
// FormDatabase
// ---------------------------------------------------------------------------

// FormID -> TESForm, populated from the user's masters and plugins with
// Bethesda's override rules applied (later plugins overwrite earlier forms
// that carry the same index).
class FormDatabase {
public:
    FormDatabase() = default;
    ~FormDatabase() = default;

    FormDatabase(const FormDatabase&) = delete;
    FormDatabase& operator=(const FormDatabase&) = delete;

    // Load masters then plugins. Returns Ok on success; on failure the
    // database is left empty rather than partially populated.
    Status load(const SkyrimConfig& config);

    void clear();

    bool loaded() const { return m_loaded; }
    size_t formCount() const { return m_forms.size(); }

    // Master index -> source file. Indexes 0..(sourceCount-1).
    const std::vector<SourceFile>& sources() const { return m_sources; }
    size_t sourceCount() const { return m_sources.size(); }

    // The last successfully opened master. Null when nothing is loaded.
    // Header fields describe the user's own installation.
    struct MasterHeader {
        float    version = 0.0f;      // HEDR version (0.94 / 1.7.x)
        uint32_t nextObjectId = 0;    // HEDR next available object id
        std::string description;      // SNAM
        bool valid = false;
    };
    const MasterHeader& masterHeader() const { return m_masterHeader; }

    // Look up by full FormID (source index + local index).
    const TESForm* find(FormID id) const;

    // Look up a form by local index within a specific source file. This is how
    // references inside a plugin resolve, since a NAME subrecord is a local
    // index relative to the referencing record's own source.
    const TESForm* findIn(uint8_t source, uint32_t localIndex) const;

    // All loaded forms of a given record type.
    std::vector<const TESForm*> findAll(RecordType type) const;

    // Count of loaded forms of a given record type.
    size_t countOf(RecordType type) const;

    // Record-type -> count, for diagnostics. Ordered map for stable output.
    std::vector<std::pair<RecordType, size_t>> typeHistogram() const;

private:
    // Recursive record walker over [p, end). Declared as a member so it can
    // populate TESForm's private fields (TESForm friends FormDatabase).
    static bool walkRecords(std::vector<TESForm>& out, const uint8_t* p,
                            const uint8_t* end, int depth, uint8_t sourceIndex,
                            uint64_t limit, bool& overflowed);

    Status loadFile(const std::string& path, uint8_t sourceIndex, bool isMaster,
                    std::vector<TESForm>& outForms);

    std::unordered_map<uint32_t, uint32_t> m_index;  // FormID -> position in m_forms
    std::vector<TESForm>      m_forms;
    std::vector<SourceFile>   m_sources;
    std::unordered_map<uint32_t, size_t> m_typeCounts;
    MasterHeader              m_masterHeader;
    bool                      m_loaded = false;
};

}  // namespace skyrim
