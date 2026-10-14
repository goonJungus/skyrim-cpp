// form_database.cpp - ESM/ESP reader. See include/skyrim/form_database.hpp.
//
// File layout (TES4, Skyrim LE):
//     TES4 record   (file header: version, record count, master list)
//     GRUP record   (top-level group, e.g. label "CELL")
//       ...records, and nested GRUPs (CELL groups contain REFR/ACHR)...
//
// Record header, 24 bytes:
//     char[4] type, u32 dataSize, u32 flags, u32 formID,
//     u16 timestamp, u16 versionControl, u16 internalVersion, u16 unknown
//
// GRUP header, 24 bytes:
//     char[4] "GRUP", u32 groupSize (INCLUDES this header), char[4] label,
//     i32 groupType, u16 timestamp, u16 versionControl, u32 unknown
//
// Subrecord, inside a record payload:
//     char[4] type, u16 size, u8[size] data
//
// A record whose flags carry 0x00040000 stores its payload zlib-compressed;
// it is inflated on load so subrecord access is uniform.
//
// FormID assignment: a record's header formID is a LOCAL index within its own
// file. The high byte of a full FormID is the file's index in load order, so
// every form gets a unique, globally meaningful ID without any override
// bookkeeping -- which matches how Bethesda's loader resolves references.

#include "skyrim/form_database.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "world/inflate.hpp"

namespace skyrim {
namespace {

}  // namespace

namespace {

constexpr uint32_t kRecordHeaderSize = 24;
constexpr uint32_t kGroupHeaderSize  = 24;
constexpr uint32_t kSubrecordHeaderSize = 6;
constexpr uint32_t kFlagCompressed   = 0x00040000;

// Defensive ceilings so a corrupt file cannot spin or over-allocate.
constexpr int         kMaxGroupDepth  = 32;
constexpr uint64_t    kMaxFormsPerFile = 8'000'000;
constexpr uint64_t    kDefaultMaxRecords = 4'000'000;

uint32_t rdU16(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
}
uint32_t rdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
float rdF32(const uint8_t* p) {
    const uint32_t v = rdU32(p);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}
RecordType rdType(const uint8_t* p) {
    return (static_cast<RecordType>(p[0]) << 24) | (static_cast<RecordType>(p[1]) << 16) |
           (static_cast<RecordType>(p[2]) << 8) | static_cast<RecordType>(p[3]);
}

// NUL-terminated string inside a payload.
std::string rdZString(const uint8_t* p, uint32_t size) {
    if (!p || size == 0) return {};
    uint32_t n = 0;
    while (n < size && p[n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

// Locate a subrecord by type within a payload. Returns false if absent.
bool findSubrecord(const uint8_t* payload, uint32_t size, RecordType want,
                   const uint8_t** outData, uint32_t* outSize) {
    const uint8_t* p = payload;
    const uint8_t* end = payload + size;
    while (static_cast<size_t>(end - p) >= kSubrecordHeaderSize) {
        const RecordType t = rdType(p);
        const uint32_t sz = rdU16(p + 4);
        p += kSubrecordHeaderSize;
        if (static_cast<size_t>(end - p) < sz) return false;   // truncated
        if (t == want) {
            if (outData) *outData = p;
            if (outSize) *outSize = sz;
            return true;
        }
        p += sz;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// TESForm
// ---------------------------------------------------------------------------

std::string TESForm::editorId() const {
    const uint8_t* d = nullptr;
    uint32_t s = 0;
    if (!findSubrecord(m_payload.data(), payloadSize(), sub::kEDID, &d, &s)) return {};
    return rdZString(d, s);
}

LocalizedString parseLocalizedString(const uint8_t* data, uint32_t size) {
    LocalizedString out;
    if (!data || size == 0) return out;
    if (size == 4) {
        // Skyrim SE string-table reference.
        out.isReference = true;
        out.id = rdU32(data);
        return out;
    }
    // Older / plugin-authored inline string. EDID-style zstring.
    out.text = rdZString(data, size);
    return out;
}

LocalizedString TESForm::displayNameRaw() const {
    const uint8_t* d = nullptr;
    uint32_t s = 0;
    if (!findSubrecord(m_payload.data(), payloadSize(), sub::kFULL, &d, &s)) return {};
    return parseLocalizedString(d, s);
}

LocalizedString TESForm::descriptionRaw() const {
    const uint8_t* d = nullptr;
    uint32_t s = 0;
    if (!findSubrecord(m_payload.data(), payloadSize(), sub::kDESC, &d, &s)) return {};
    return parseLocalizedString(d, s);
}

std::string TESForm::displayName() const {
    const LocalizedString s = displayNameRaw();
    return s.isReference ? std::string{} : s.text;
}

// ---------------------------------------------------------------------------
// Record walking
// ---------------------------------------------------------------------------

// Recursive walker over [p, end). Collects every non-GRUP record as a form.
// Defined outside the anonymous namespace because it is a FormDatabase member
// and must have access to TESForm's private fields.
bool FormDatabase::walkRecords(std::vector<TESForm>& forms, const uint8_t* p,
                               const uint8_t* end, int depth, uint8_t sourceIndex,
                               uint64_t limit, bool& overflowed) {
    if (depth > kMaxGroupDepth) return false;

    while (static_cast<size_t>(end - p) >= kRecordHeaderSize) {
        const RecordType type = rdType(p);
        const uint32_t dataSize = rdU32(p + 4);
        const uint32_t flags    = rdU32(p + 8);
        const uint32_t localId  = rdU32(p + 12);

        if (type == rec::kGRUP) {
            // IMPORTANT: in TES4 a GRUP header *is* the 24-byte record header
            // reinterpreted -- it is not a second header stacked on top. The
            // fields at +8/+12 hold the group label and group type rather than
            // flags/formID. groupSize therefore covers the whole GRUP record
            // INCLUDING its header, and children begin right after it.
            if (dataSize < kGroupHeaderSize) return false;
            const uint8_t* children = p + kGroupHeaderSize;
            const uint8_t* groupEnd = p + dataSize;
            if (groupEnd > end) return false;
            if (!walkRecords(forms, children, groupEnd, depth + 1, sourceIndex,
                             limit, overflowed)) {
                return false;
            }
            // groupSize covers the WHOLE GRUP including its 24-byte header,
            // so the advance is exactly dataSize.
            p += dataSize;
        } else {
            const uint8_t* body = p + kRecordHeaderSize;
            if (static_cast<size_t>(end - body) < dataSize) return false;

            if (localId != 0) {                       // 0 = unused slot, skip
                if (forms.size() >= limit) { overflowed = true; return true; }

                TESForm form;
                form.m_id = makeFormID(sourceIndex, localId);
                form.m_type = type;
                form.m_flags = flags;
                form.m_source = sourceIndex;

                if (flags & kFlagCompressed) {
                    form.m_wasCompressed = true;
                    // Compressed payloads are inflated up front so subrecord
                    // access is uniform. The body is framed by a u32
                    // decompressed size followed by a zlib stream.
                    // Memory cost is the trade-off for simplicity; lazy
                    // inflation is a later optimisation.
                    if (!zip::zlibInflateRecord(body, dataSize, form.m_payload)) {
                        // Keep the record with an empty payload rather than
                        // dropping it: a form's absence would break
                        // references, an empty one degrades gracefully.
                        form.m_payload.clear();
                    }
                } else {
                    form.m_payload.assign(body, body + dataSize);
                }
                forms.push_back(std::move(form));
            }
            // For a plain record dataSize is the PAYLOAD size only, so the
            // record spans header + payload.
            p += kRecordHeaderSize + dataSize;
        }
    }
    return true;
}

// Parse the leading TES4 header record (before any GRUP).
namespace {

bool parseMasterHeader(const uint8_t* p, const uint8_t* end, uint8_t sourceIndex,
                       uint64_t limit, FormDatabase::MasterHeader& out) {
    if (static_cast<size_t>(end - p) < kRecordHeaderSize) return false;
    if (rdType(p) != rec::kTES4) return false;

    const uint32_t dataSize = rdU32(p + 4);
    const uint32_t flags    = rdU32(p + 8);
    const uint8_t* body     = p + kRecordHeaderSize;
    if (static_cast<size_t>(end - body) < dataSize) return false;

    std::vector<uint8_t> payload;
    if (flags & kFlagCompressed) {
        if (!zip::zlibInflateRecord(body, dataSize, payload)) return false;
    } else {
        payload.assign(body, body + dataSize);
    }

    // HEDR: float version, int32 recordCount, uint32 nextObjectId
    const uint8_t* d = nullptr;
    uint32_t s = 0;
    if (findSubrecord(payload.data(), static_cast<uint32_t>(payload.size()),
                      makeRecordType('H','E','D','R'), &d, &s) && s >= 12) {
        out.version = rdF32(d);
        out.nextObjectId = rdU32(d + 8);
    }
    if (findSubrecord(payload.data(), static_cast<uint32_t>(payload.size()),
                      sub::kSNAM, &d, &s)) {
        out.description = rdZString(d, s);
    }
    out.valid = true;
    (void)sourceIndex;
    (void)limit;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// FormDatabase
// ---------------------------------------------------------------------------

Status FormDatabase::load(const SkyrimConfig& config) {
    clear();

    if (config.dataDirectory.empty()) return Status::NotConfigured;

    std::error_code ec;
    if (!std::filesystem::is_directory(config.dataDirectory, ec)) {
        return Status::NotConfigured;
    }
    if (!config.logSink) {
        // no-op: caller supplied no sink
    }

    const std::filesystem::path dir(config.dataDirectory);
    const uint64_t limit =
        config.maxRecords ? config.maxRecords : kDefaultMaxRecords;

    // Masters first, in the canonical order. Skyrim.esm is index 0; every
    // plugin's FormID high byte follows the same ordering.
    const std::vector<std::string> masters = {
        "Skyrim.esm",
        "Update.esm",
        "Dawnguard.esm",
        "Hearthfires.esm",
        "Dragonborn.esm",
    };

    std::vector<TESForm> loaded;
    uint8_t sourceIndex = 0;

    // Read the whole file once. ESM/ESP files are large but sequential, and a
    // single buffer keeps subrecord lifetime simple (records hold copies).
    auto readAll = [&](const std::filesystem::path& path, std::vector<uint8_t>& buf) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        f.seekg(0, std::ios::end);
        const std::streamoff size = f.tellg();
        if (size <= 0) return false;
        f.seekg(0, std::ios::beg);
        buf.resize(static_cast<size_t>(size));
        f.read(reinterpret_cast<char*>(buf.data()), size);
        return static_cast<size_t>(f.gcount()) == buf.size();
    };

    // --- masters ---
    bool sawMaster = false;
    for (const std::string& name : masters) {
        const std::filesystem::path path = dir / name;
        if (!std::filesystem::exists(path, ec)) continue;

        std::vector<uint8_t> buf;
        if (!readAll(path, buf)) { clear(); return Status::ReadFailed; }

        const uint8_t* p = buf.data();
        const uint8_t* end = buf.data() + buf.size();

        if (!sawMaster) {
            if (!parseMasterHeader(p, end, sourceIndex, limit, m_masterHeader)) {
                clear();
                return Status::ParseFailed;
            }
            sawMaster = true;
            p += kRecordHeaderSize + rdU32(p + 4);
        }

        const size_t before = loaded.size();
        bool overflowed = false;
        if (!walkRecords(loaded, p, end, 0, sourceIndex, limit, overflowed)) {
            clear();
            return Status::ParseFailed;
        }
        if (overflowed) { clear(); return Status::TooManyRecords; }

        m_sources.push_back(SourceFile{name, sourceIndex, true,
                                       static_cast<uint64_t>(loaded.size() - before)});
        ++sourceIndex;
    }
    if (!sawMaster) { clear(); return Status::MasterMissing; }

    // --- plugins, in caller-supplied load order ---
    for (const std::string& name : config.plugins) {
        if (sourceIndex == kMasterIndexNone) break;   // 0xFF reserved
        const std::filesystem::path path = dir / name;
        if (!std::filesystem::exists(path, ec)) continue;   // absent = skip, not fatal

        std::vector<uint8_t> buf;
        if (!readAll(path, buf)) { clear(); return Status::ReadFailed; }

        const size_t before = loaded.size();
        bool overflowed = false;
        // Plugins carry their own TES4 header too; skip it.
        const uint8_t* p = buf.data();
        const uint8_t* end = buf.data() + buf.size();
        if (static_cast<size_t>(end - p) >= kRecordHeaderSize &&
            rdType(p) == rec::kTES4) {
            p += kRecordHeaderSize + rdU32(p + 4);
        }
        if (!walkRecords(loaded, p, end, 0, sourceIndex, limit, overflowed)) {
            clear();
            return Status::ParseFailed;
        }
        if (overflowed) { clear(); return Status::TooManyRecords; }

        m_sources.push_back(SourceFile{name, sourceIndex, false,
                                       static_cast<uint64_t>(loaded.size() - before)});
        ++sourceIndex;
    }

    // --- index ---
    m_forms = std::move(loaded);
    m_index.reserve(m_forms.size() * 2);
    for (uint32_t i = 0; i < m_forms.size(); ++i) {
        m_index[m_forms[i].m_id] = i;
        m_typeCounts[m_forms[i].m_type]++;
    }
    m_loaded = true;
    return Status::Ok;
}

void FormDatabase::clear() {
    m_index.clear();
    m_forms.clear();
    m_sources.clear();
    m_typeCounts.clear();
    m_masterHeader = MasterHeader{};
    m_loaded = false;
}

const TESForm* FormDatabase::find(FormID id) const {
    const auto it = m_index.find(id);
    if (it == m_index.end()) return nullptr;
    return &m_forms[it->second];
}

const TESForm* FormDatabase::findIn(uint8_t source, uint32_t localIndex) const {
    return find(makeFormID(source, localIndex));
}

std::vector<const TESForm*> FormDatabase::findAll(RecordType type) const {
    std::vector<const TESForm*> out;
    for (const TESForm& f : m_forms) {
        if (f.m_type == type) out.push_back(&f);
    }
    return out;
}

size_t FormDatabase::countOf(RecordType type) const {
    const auto it = m_typeCounts.find(type);
    return it == m_typeCounts.end() ? 0 : it->second;
}

std::vector<std::pair<RecordType, size_t>> FormDatabase::typeHistogram() const {
    std::vector<std::pair<RecordType, size_t>> out(m_typeCounts.begin(), m_typeCounts.end());
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

std::string toString(RecordType t) {
    return std::string{typeChar0(t), typeChar1(t), typeChar2(t), typeChar3(t)};
}

const char* toString(Status s) {
    switch (s) {
        case Status::Ok:            return "Ok";
        case Status::NotConfigured: return "NotConfigured";
        case Status::MasterMissing: return "MasterMissing";
        case Status::ReadFailed:    return "ReadFailed";
        case Status::ParseFailed:   return "ParseFailed";
        case Status::TooManyRecords:return "TooManyRecords";
        case Status::ArchiveFailed: return "ArchiveFailed";
    }
    return "Unknown";
}

}  // namespace skyrim
