// string_table.hpp - Skyrim SE localized string tables (*.strings).
//
// Public API of the Skyrim module.
//
// A Skyrim SE FULL/DESC subrecord is a 4-byte reference into one of these
// tables (see LocalizedString in form_database.hpp). This resolves those ids
// to text.
//
// Tables are found in two places:
//   <dataDirectory>/Strings/<File>_<Language>.STRINGS   (loose files)
//   <Archive>.bsa -> strings<N>.strings                  (inside BSA archives;
//                                                        the base game ships
//                                                        its strings this way,
//                                                        not as loose files)
//
// The loose-file reader is implemented here. BSA-backed tables follow with the
// archive reader.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace skyrim {

// One loaded localized string table (a whole language for a whole source file).
class StringTable {
public:
    StringTable() = default;

    // Load a .strings / .STRINGS file. Returns false if the file is missing or
    // the header is implausible.
    bool loadFile(const std::string& path);

    // True once a table has been loaded successfully.
    bool loaded() const { return m_loaded; }
    size_t entryCount() const { return m_index.size(); }

    // Resolve a string id. Returns an empty string when the id is absent.
    // Handles the id 0 sentinel used for "no name".
    std::string lookup(uint32_t id) const;

    bool has(uint32_t id) const { return m_index.find(id) != m_index.end(); }

    // Fraction of entries that resolve to a NUL-preceded, printable string.
    // Exposed because a mis-detected layout shows up here first; see the
    // README note on verifying this against a known install.
    float resolvedFraction() const { return m_resolvedFraction; }

private:
    // offset -> absolute position of the string, and the string id
    std::unordered_map<uint32_t, std::pair<size_t, uint32_t>> m_index;
    std::string m_data;        // the string blob (owned copy)
    bool        m_loaded = false;
    float       m_resolvedFraction = 0.0f;
};

// Resolves ids across several tables, later tables winning (plugin override
// order). This mirrors how the game layers base-game strings with DLC and
// plugin strings.
class StringResolver {
public:
    // Load <dataDirectory>/Strings/<fileName>_<language>.STRINGS.
    // Missing files are skipped silently (normal - not every master has one).
    bool loadLoose(const std::string& dataDirectory,
                   const std::string& fileName,
                   const std::string& language);

    // Register an already-loaded table (used by the BSA reader later).
    void add(StringTable table);

    std::string lookup(uint32_t id) const;
    bool has(uint32_t id) const;

    size_t tableCount() const { return m_tables.size(); }
    size_t totalEntries() const;

private:
    std::vector<StringTable> m_tables;
};

}  // namespace skyrim
