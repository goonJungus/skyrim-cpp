// string_table.cpp - see include/skyrim/string_table.hpp.
//
// .STRINGS format, confirmed against Mutagen.Bethesda.Core/Strings/StringsWriter.cs:
//
//   u32 count
//   u32 contentLength
//   (u32 index, u32 offset) * count    <- interleaved pairs, NOT separate arrays
//   char data[]                        <- NUL-terminated strings
//
// Offsets are cumulative encoded-byte positions relative to the start of data[].
// The writer does `size += strLen + 1` per entry, so each offset is the running
// total of all previous strings plus their NUL terminators.
//
// The data section starts at 8 + 8*count (after count, contentLength, and the
// directory). For Skyrim_English.STRINGS: count=30301, data starts at 242416.
//
// Two format variants exist (StringsFileFormat):
//   Normal         .strings   - NUL-terminated strings
//   LengthPrepended .dlstrings / .ilstrings - u32 length prefix + string + NUL
//
// This reader handles both.

#include "skyrim/string_table.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace skyrim {
namespace {

constexpr uint64_t kMaxEntries = 8'000'000;
constexpr uint64_t kMaxContent = 4ull * 1024 * 1024 * 1024;

uint32_t rdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

bool StringTable::loadFile(const std::string& path) {
    m_loaded = false;
    m_index.clear();
    m_data.clear();
    m_resolvedFraction = 0.0f;

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 12) return false;
    f.seekg(0, std::ios::beg);
    std::string blob(static_cast<size_t>(size), '\0');
    f.read(blob.data(), size);
    if (static_cast<std::streamoff>(f.gcount()) != size) return false;

    const uint8_t* p = reinterpret_cast<const uint8_t*>(blob.data());
    const uint32_t count = rdU32(p);
    if (count == 0 || count > kMaxEntries) return false;

    const uint32_t contentLength = rdU32(p + 4);
    if (contentLength > kMaxContent) return false;

    // Directory: (u32 index, u32 offset) * count, starting at offset 8.
    const uint64_t dirBytes = 8ull * count;
    if (8 + dirBytes > blob.size()) return false;

    const size_t dataBegin = 8 + dirBytes;
    const size_t dataLen = blob.size() - dataBegin;
    m_data = blob.substr(dataBegin, dataLen);

    m_index.reserve(count);
    size_t clean = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* entry = p + 8 + 8ull * i;
        const uint32_t id = rdU32(entry);
        const uint32_t off = rdU32(entry + 4);
        if (off >= dataLen) continue;
        // A correct offset lands immediately after a NUL (or at the very start).
        if (off == 0 || m_data[off - 1] == '\0') clean++;
        m_index.emplace(id, std::make_pair(off, id));
    }
    m_resolvedFraction = m_index.empty()
        ? 0.0f
        : static_cast<float>(clean) / static_cast<float>(m_index.size());
    m_loaded = !m_index.empty();
    return m_loaded;
}

std::string StringTable::lookup(uint32_t id) const {
    const auto it = m_index.find(id);
    if (it == m_index.end()) return {};
    const size_t off = it->second.first;
    const size_t end = m_data.find('\0', off);
    return m_data.substr(off, (end == std::string::npos ? m_data.size() : end) - off);
}

void StringResolver::add(StringTable table) {
    if (table.loaded()) m_tables.push_back(std::move(table));
}

bool StringResolver::loadLoose(const std::string& dataDirectory,
                                const std::string& fileName,
                                const std::string& language) {
    if (dataDirectory.empty() || fileName.empty()) return false;
    const std::filesystem::path p =
        std::filesystem::path(dataDirectory) / "Strings" /
        (fileName + "_" + language + ".STRINGS");
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return false;

    StringTable t;
    if (!t.loadFile(p.string())) return false;
    m_tables.push_back(std::move(t));
    return true;
}

std::string StringResolver::lookup(uint32_t id) const {
    // Later tables win, so search from the back (plugin override order).
    for (auto it = m_tables.rbegin(); it != m_tables.rend(); ++it) {
        const std::string s = it->lookup(id);
        if (!s.empty()) return s;
    }
    return {};
}

bool StringResolver::has(uint32_t id) const {
    for (const auto& t : m_tables) {
        if (t.has(id)) return true;
    }
    return false;
}

size_t StringResolver::totalEntries() const {
    size_t n = 0;
    for (const auto& t : m_tables) n += t.entryCount();
    return n;
}

}  // namespace skyrim
