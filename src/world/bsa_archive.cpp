// bsa_archive.cpp - Bethesda BSA reader (TES4 family). See bsa_archive.hpp.
//
// Field map, established by reading a reference implementation and then
// verified byte-for-byte against real Skyrim archives.
//
// Header (36 bytes, little-endian):
//     char  magic[4]        "BSA\0"
//     u32   version         103 | 104 | 105
//     u32   headerSize      always 36
//     u32   archiveFlags
//     u32   folderCount
//     u32   fileCount
//     u32   folderNamesLen  <- LENGTH, not an offset
//     u32   fileNamesLen    <- LENGTH, not an offset
//     u16   archiveTypes
//     u16   padding
//
// The two u32s that older documentation calls folderRecordOffset /
// fileRecordOffset are name table *lengths*. There are no table offsets in the
// header at all: every position is derived sequentially. Crucially the lengths
// exclude each name's u8 length prefix, so the folder-name region occupies
// folderNamesLen + folderCount bytes and the file-name region occupies
// fileNamesLen + fileCount.
//
// archiveFlags bits:
//     0x001  directory strings present   (folder names in the body)
//     0x002  file strings present        (leaf-name table after the body)
//     0x004  archive is compressed       (per-file bit 0x40000000 overrides)
//     0x008  retain folder names at runtime
//     0x010  retain file names at runtime
//     0x020  retain file name offsets
//     0x040  xbox (big-endian CRC) archive
//     0x080  retain strings during startup
//     0x100  embedded file names (full virtual path written beside each payload)
//     0x200  xmem compression
//
// Folder record (16 bytes on v103/v104, 24 on v105):
//     u64   hash       Bethesda name hash of the folder path
//     u32   count      number of file records owned by this folder
//     u32   offset     first file index (authoritative in archives written by
//                      archive.exe; some producers corrupt it, so it is only
//                      used as a cross-check and the running position is used)
//     u32   padding    v105 only
//
// File record (16 bytes, every version):
//     u64   hash       Bethesda name hash of the file
//     u32   size       stored size, with 0x40000000 = compression override and
//                      0x80000000 = "checked"; both must be masked off
//     u32   offset     absolute byte offset of the payload; bit 31 marks a
//                      secondary archive and must be masked off
//
// Body, interleaved per folder in folder order:
//     u8    nameLen    includes the NUL terminator
//     ...   folderName
//     file records for that folder, 16 bytes each
//
// then the leaf-name table: fileCount NUL-terminated names in file-record order.
//
// Payload: when compressed, a u32 decompressed size precedes the zlib stream,
// which is a sequence of u16 block headers (size-1 in the low 15 bits, 0x8000 =
// that block is zlib) followed by the block bytes.

#include "skyrim/bsa_archive.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "world/inflate.hpp"

namespace skyrim {
namespace {

// Size of the on-disk name hash: { last, last2, length, first, crc32 }.
constexpr uint32_t kHashBytes = 8;
constexpr uint32_t kFileRecordSize = 16;

// The two high bits of the size field are flags, not size.
constexpr uint32_t kSizeCompressionBit = 0x40000000u;
constexpr uint32_t kSizeCheckedBit = 0x80000000u;
constexpr uint32_t kSizeMask = 0x3FFFFFFFu;

// The high bit of the offset field marks a secondary (external) archive.
constexpr uint32_t kOffsetSecondaryBit = 0x80000000u;

// archiveFlags bit 0x100: each payload is prefixed with its full virtual path
// as a bstring (u8 length including the NUL, then that many bytes).
constexpr uint32_t kFlagEmbeddedFileNames = 0x100u;

// Defensive ceilings so a corrupt archive cannot allocate unbounded memory.
constexpr uint64_t kMaxFiles = 4'000'000;
constexpr uint64_t kMaxFolders = 1'000'000;
constexpr uint64_t kMaxFileSize = 4ull * 1024 * 1024 * 1024;
constexpr uint64_t kMaxNameLen = 1024;

std::string toLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Normalise separators and case so "Meshes/A/Thing.nif" and
// "meshes\\a\\thing.nif" are the same key.
std::string normalizePath(const std::string& s) {
    std::string out = toLower(s);
    for (char& c : out) {
        if (c == '/') c = '\\';
    }
    return out;
}

std::string extensionOf(const std::string& lowerName) {
    const size_t dot = lowerName.find_last_of('.');
    if (dot == std::string::npos) return {};
    return lowerName.substr(dot + 1);
}

std::string leafOf(const std::string& lowerPath) {
    const size_t slash = lowerPath.find_last_of('\\');
    return (slash == std::string::npos) ? lowerPath : lowerPath.substr(slash + 1);
}

bool readExact(std::FILE* f, void* dst, size_t n) {
    return n == 0 || std::fread(dst, 1, n, f) == n;
}

bool seekTo(std::FILE* f, uint64_t offset) {
    return std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0;
}

bool readU32At(std::FILE* f, uint64_t offset, uint32_t& out) {
    if (!seekTo(f, offset)) return false;
    uint8_t b[4];
    if (!readExact(f, b, 4)) return false;
    out = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
          (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

bool readU64At(std::FILE* f, uint64_t offset, uint64_t& out) {
    uint32_t lo = 0, hi = 0;
    if (!readU32At(f, offset, lo)) return false;
    if (!readU32At(f, offset + 4, hi)) return false;
    out = (static_cast<uint64_t>(hi) << 32) | lo;
    return true;
}

}  // namespace

BsaArchive::~BsaArchive() { close(); }

bool BsaArchive::open(const std::string& path) {
    close();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    m_file = f;
    m_path = path;

    if (!parseDirectory()) {
        close();
        return false;
    }
    m_open = true;
    return true;
}

void BsaArchive::close() {
    if (m_file) { std::fclose(m_file); m_file = nullptr; }
    m_files.clear();
    m_folders.clear();
    m_index.clear();
    m_leafIndex.clear();
    m_path.clear();
    m_open = false;
    m_totalBytes = 0;
    m_version = 0;
    m_archiveFlags = 0;
    m_archiveCompressed = false;
}

bool BsaArchive::parseDirectory() {
    // ---- header ----
    char magic[4] = {0, 0, 0, 0};
    if (!seekTo(m_file, 0) || !readExact(m_file, magic, 4)) return false;
    if (std::memcmp(magic, "BSA\0", 4) != 0) return false;

    uint32_t headerSize = 0, archiveFlags = 0;
    uint32_t folderCount = 0, fileCount = 0;
    uint32_t folderNamesLen = 0, fileNamesLen = 0;
    if (!readU32At(m_file, 4, m_version)) return false;
    if (!readU32At(m_file, 8, headerSize) || !readU32At(m_file, 12, archiveFlags) ||
        !readU32At(m_file, 16, folderCount) || !readU32At(m_file, 20, fileCount) ||
        !readU32At(m_file, 24, folderNamesLen) || !readU32At(m_file, 28, fileNamesLen)) {
        return false;
    }

    if (m_version < 103 || m_version > 105) return false;  // unknown layout
    if (headerSize != 36) return false;
    if (folderCount > kMaxFolders || fileCount > kMaxFiles) return false;
    if (fileCount == 0) return false;

    m_archiveFlags = archiveFlags;
    m_archiveCompressed = (archiveFlags & 0x004u) != 0;
    const bool haveFolderNames = (archiveFlags & 0x001u) != 0;
    const bool haveFileNames = (archiveFlags & 0x002u) != 0;

    // Folder records are 16 bytes on x86, 24 on x64 (v105).
    const uint64_t folderStride = (m_version >= 105) ? 24u : 16u;

    // ---- folder record table ----
    // Only the hash and count are needed; the name lives in the body.
    m_folders.resize(folderCount);
    for (uint32_t i = 0; i < folderCount; i++) {
        const uint64_t at = headerSize + static_cast<uint64_t>(i) * folderStride;
        // The record opens with a u64 name hash, which is skipped: lookup is by
        // path. Only the file count matters here.
        if (!readU32At(m_file, at + kHashBytes, m_folders[i].count)) return false;
    }

    // ---- interleaved body: name, then that folder's file records ----
    // The name length prefix is not counted in folderNamesLen, so the body
    // spans folderNamesLen + folderCount bytes past the folder record table.
    uint64_t cursor = headerSize + folderStride * folderCount;
    m_files.reserve(fileCount);
    uint64_t totalStored = 0;

    for (uint32_t i = 0; i < folderCount; i++) {
        BsaFolderEntry& folder = m_folders[i];
        folder.firstFileIndex = m_files.size();  // running position is authoritative

        if (haveFolderNames) {
            uint8_t nameLen = 0;
            if (!seekTo(m_file, cursor) || !readExact(m_file, &nameLen, 1)) return false;
            if (nameLen == 0 || nameLen > kMaxNameLen) return false;
            std::string raw(static_cast<size_t>(nameLen), '\0');
            if (!readExact(m_file, raw.data(), raw.size())) return false;
            const size_t z = raw.find('\0');
            folder.name = normalizePath(
                raw.substr(0, z == std::string::npos ? raw.size() : z));
            cursor += 1 + nameLen;
        }

        for (uint32_t k = 0; k < folder.count; k++) {
            BsaFileEntry e;
            e.folderIndex = i;
            uint32_t sizeField = 0, offsetField = 0;
            // The record opens with a u64 name hash. Lookup is by path, not by
            // hash, so it is skipped rather than stored.
            if (!readU32At(m_file, cursor + kHashBytes, sizeField)) return false;
            if (!readU32At(m_file, cursor + kHashBytes + 4, offsetField)) return false;

            e.sizeBytes = sizeField & kSizeMask;
            // A set compression bit means "differs from the archive default".
            const bool perFileCompressed = (sizeField & kSizeCompressionBit) != 0;
            e.compressed = perFileCompressed ? !m_archiveCompressed : m_archiveCompressed;
            e.offset = offsetField & ~kOffsetSecondaryBit;

            if (e.sizeBytes > kMaxFileSize) return false;
            // sizeBytes is the whole stored payload, including any embedded-name
            // prefix and the u32 decompressed-size prefix. Both are stripped in
            // decompress(), so nothing is seeked to here -- that keeps opening a
            // 23,000-file archive a single sequential pass.
            totalStored += e.sizeBytes;
            m_files.push_back(std::move(e));
            cursor += kFileRecordSize;
        }
    }

    if (m_files.size() != fileCount) return false;  // body did not match the count

    // ---- leaf-name table ----
    // Sits directly after the body; names are NUL-terminated, in record order.
    if (haveFileNames) {
        const uint64_t tableSize = fileNamesLen + fileCount;  // + one NUL per name
        if (tableSize > kMaxFileSize) return false;
        std::string table(static_cast<size_t>(tableSize), '\0');
        if (!seekTo(m_file, cursor) || !readExact(m_file, table.data(), table.size())) {
            return false;
        }
        size_t pos = 0;
        for (auto& e : m_files) {
            if (pos >= table.size()) return false;
            const size_t end = table.find('\0', pos);
            if (end == std::string::npos) return false;
            e.name = normalizePath(table.substr(pos, end - pos));
            pos = end + 1;
        }
    }

    // ---- join folder + leaf, and index ----
    for (auto& e : m_files) {
        const std::string& folder = m_folders[e.folderIndex].name;
        e.fullPath = folder.empty() ? e.name : folder + "\\" + e.name;
    }
    // Without decompressing every entry there is no honest uncompressed total,
    // so report the stored size, which is what the archive actually occupies.
    m_totalBytes = totalStored;

    m_index.reserve(m_files.size() * 2);
    m_leafIndex.reserve(m_files.size() * 2);
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_files.size()); i++) {
        if (!m_files[i].fullPath.empty()) m_index.emplace(m_files[i].fullPath, i);
        if (!m_files[i].name.empty()) m_leafIndex.emplace(m_files[i].name, i);
    }
    return true;
}

const BsaFileEntry* BsaArchive::find(const std::string& name) const {
    const std::string want = normalizePath(name);
    const auto it = m_index.find(want);
    if (it != m_index.end()) return &m_files[it->second];
    // Allow a bare leaf name; first match wins, which matches how the engine
    // resolves ambiguous names.
    const auto lit = m_leafIndex.find(leafOf(want));
    if (lit != m_leafIndex.end()) return &m_files[lit->second];
    return nullptr;
}

std::vector<uint8_t> BsaArchive::readRaw(const BsaFileEntry& e) const {
    std::vector<uint8_t> out;
    if (!m_file || !m_open) return out;
    if (e.sizeBytes == 0) return out;
    if (e.sizeBytes > kMaxFileSize) return out;

    out.resize(static_cast<size_t>(e.sizeBytes));
    if (!seekTo(m_file, e.offset) || !readExact(m_file, out.data(), out.size())) {
        out.clear();
    }
    return out;
}

std::vector<uint8_t> BsaArchive::decompress(const BsaFileEntry& e) const {
    std::vector<uint8_t> raw = readRaw(e);
    if (raw.empty()) return {};

    size_t pos = 0;

    // archiveFlags 0x100: the payload opens with its own virtual path as a
    // bstring -- a u8 length (including the NUL) followed by that many bytes.
    if (m_archiveFlags & kFlagEmbeddedFileNames) {
        if (pos >= raw.size()) return {};
        const size_t nameLen = raw[pos];
        if (nameLen == 0 || nameLen > kMaxNameLen) return {};
        pos += 1 + nameLen;
        if (pos > raw.size()) return {};
    }

    if (!e.compressed) {
        // Stored payloads may be sector-padded on disk; the remaining bytes are
        // the file, so just hand back what is left.
        return std::vector<uint8_t>(raw.begin() + static_cast<long>(pos), raw.end());
    }

    // Compressed payloads are a single zlib stream (RFC 1950), framed by a u32
    // decompressed size. Stock Skyrim archives confirmed this layout. There is
    // no per-block framing on this path.
    if (pos + 4 > raw.size()) return {};
    const uint32_t decompressedSize = static_cast<uint32_t>(raw[pos]) |
                                      (static_cast<uint32_t>(raw[pos + 1]) << 8) |
                                      (static_cast<uint32_t>(raw[pos + 2]) << 16) |
                                      (static_cast<uint32_t>(raw[pos + 3]) << 24);
    if (decompressedSize > kMaxFileSize) return {};
    pos += 4;

    std::vector<uint8_t> out;
    out.reserve(decompressedSize);
    if (!zip::zlibInflate(raw.data() + pos, raw.size() - pos, out,
                          decompressedSize)) {
        return {};
    }
    if (out.size() != decompressedSize) return {};
    return out;
}

std::vector<uint8_t> BsaArchive::read(const std::string& name) const {
    const BsaFileEntry* e = find(name);
    return e ? decompress(*e) : std::vector<uint8_t>{};
}

std::vector<std::string> BsaArchive::listFiles() const {
    std::vector<std::string> out;
    out.reserve(m_files.size());
    for (const auto& e : m_files) {
        if (!e.fullPath.empty()) out.push_back(e.fullPath);
    }
    return out;
}

std::vector<std::string> BsaArchive::listByExtension(const std::string& ext) const {
    std::string want = toLower(ext);
    if (!want.empty() && want[0] != '.') want.insert(want.begin(), '.');
    const std::string bare = (want.size() > 1) ? want.substr(1) : std::string();
    std::vector<std::string> out;
    for (const auto& e : m_files) {
        if (e.name.empty()) continue;
        if (extensionOf(e.name) == bare) out.push_back(e.fullPath);
    }
    return out;
}

}  // namespace skyrim
