// bsa_archive.hpp - Bethesda BSA archive reader (TES4 family: v103/v104/v105).
//
// This is where the world actually lives. Skyrim's meshes, textures and (on a
// stock 2011 install) its string tables ship inside *.bsa archives, not as
// loose files, so nothing renders until this works.
//
// Layout, verified byte-for-byte against real Skyrim archives (see
// src/world/bsa_archive.cpp for the full field map):
//
//   header            36 bytes, always
//   folder records    folderCount x 16 bytes (v103/v104) or x 24 (v105)
//   body              for each folder, in folder order:
//                       u8   nameLen        (length includes the NUL)
//                       ...  folderName     (nameLen bytes, NUL-terminated)
//                       file records for this folder, 16 bytes each
//   file name table   fileCount NUL-terminated leaf names, file-record order
//   payload data      file records point into here by absolute byte offset
//
// The body is *interleaved*: each folder's name is immediately followed by that
// folder's own file records. Treating the folder records, file records and name
// tables as three separate sequential tables desynchronises the parse, which is
// the single most common way to get this format wrong.
//
// Design notes:
//   * Archives are opened by path and read on demand. A 1.4 GB Meshes.bsa is
//     never loaded into memory; only the directory is parsed up front and file
//     bodies are seeked to and read per request.
//   * Per-file payloads are block-compressed (zlib per block, or one block).
//     Only the blocks needed are inflated.
//   * Lookup is case-insensitive on the full archive-relative path, e.g.
//     "meshes\\architecture\\whiterun\\keep\\keepwall01.nif".

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace skyrim {

// One entry in a BSA file-record table.
struct BsaFileEntry {
    // Start of the stored payload, an absolute byte offset from the file start.
    // (This is not a 2048-byte sector index; the TES4 record stores bytes.)
    uint64_t offset = 0;
    // Bytes stored on disk (flag bits masked off). Includes any embedded-name
    // prefix and the u32 decompressed-size prefix; both are stripped on read.
    uint32_t sizeBytes = 0;
    uint32_t folderIndex = 0;
    bool     compressed = false;  // payload is DEFLATE block-framed
    std::string name;        // leaf name, lowercase, e.g. "keepwall01.nif"
    std::string fullPath;    // lowercase, folder + "\\" + name
};

// A folder (archive sub-directory).
struct BsaFolderEntry {
    uint32_t count = 0;     // number of file records owned by this folder
    uint64_t firstFileIndex = 0;  // index of this folder's first record
    std::string name;       // lowercase path, e.g. "meshes\\architecture\\whiterun"
};

class BsaArchive {
public:
    BsaArchive() = default;
    ~BsaArchive();
    BsaArchive(const BsaArchive&) = delete;
    BsaArchive& operator=(const BsaArchive&) = delete;

    // Parse the directory of a BSA. Returns false on a missing file, an
    // unsupported version, or a structurally invalid directory.
    bool open(const std::string& path);

    void close();
    bool isOpen() const { return m_open; }

    const std::string& path() const { return m_path; }
    uint32_t version() const { return m_version; }
    uint32_t archiveFlags() const { return m_archiveFlags; }
    size_t fileCount() const { return m_files.size(); }
    size_t folderCount() const { return m_folders.size(); }

    // Look a file up by full archive-relative path, case-insensitive. Accepts
    // either slash style. A leaf-only name is also tried, so
    // read("keepwall01.nif") works in an archive with many matches (first wins).
    // Returns nullptr when absent.
    const BsaFileEntry* find(const std::string& name) const;
    bool has(const std::string& name) const { return find(name) != nullptr; }

    // Read and decompress one file. Returns an empty vector when absent or on
    // any read/decompress failure. Never throws.
    std::vector<uint8_t> read(const std::string& name) const;

    // Read one file without decompressing (raw, still block-framed). Useful for
    // tools that want to inspect or stream it.
    std::vector<uint8_t> readRaw(const BsaFileEntry& entry) const;

    // Every full path in the archive (lowercase).
    std::vector<std::string> listFiles() const;

    // Names whose extension matches (case-insensitive, with or without dot).
    std::vector<std::string> listByExtension(const std::string& ext) const;

    // Total uncompressed bytes across the directory.
    uint64_t totalUncompressedBytes() const { return m_totalBytes; }

private:
    bool parseDirectory();
    std::vector<uint8_t> decompress(const BsaFileEntry& e) const;

    std::string              m_path;
    std::FILE*               m_file = nullptr;
    bool                     m_open = false;
    uint32_t                 m_version = 0;
    uint32_t                 m_archiveFlags = 0;
    bool                     m_archiveCompressed = false;
    std::vector<BsaFileEntry>  m_files;
    std::vector<BsaFolderEntry> m_folders;
    std::unordered_map<std::string, uint32_t> m_index;  // full path -> index
    std::unordered_map<std::string, uint32_t> m_leafIndex;  // leaf -> index
    uint64_t                 m_totalBytes = 0;
};

}  // namespace skyrim
