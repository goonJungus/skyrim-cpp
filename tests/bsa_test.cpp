// bsa_test.cpp - verifies the BSA reader against synthetic archives whose
// contents we generated ourselves (see make_bsa_fixture.py). Because we know
// exactly what each entry should contain, this checks the reader against
// ground truth rather than against itself.
//
// Checks: header parse, folder/body interleaving, absolute payload offsets,
// case-insensitive lookup on the full path, stored and zlib block decompression,
// the u32 decompressed-size prefix, zero-byte files, sector padding, and all
// three on-disk versions (v103 = 2011 Steam, v104, v105 x64 folder records).

#include "skyrim/bsa_archive.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;

void expect(bool cond, const std::string& what) {
    if (!cond) { std::printf("  FAIL: %s\n", what.c_str()); failures++; }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: bsa_test <fixture-dir>\n");
        return 2;
    }
    const std::string dir = argv[1];

    // ---- v104: mixed stored / compressed, several folders ----
    {
        skyrim::BsaArchive bsa;
        expect(bsa.open(dir + "/fixture104.bsa"), "open v104");
        expect(bsa.version() == 104, "version == 104");
        expect(bsa.fileCount() == 7,
               "7 files in v104 (got " + std::to_string(bsa.fileCount()) + ")");
        expect(bsa.folderCount() == 4,
               "4 folders in v104 (got " + std::to_string(bsa.folderCount()) + ")");
        if (!bsa.isOpen()) { std::printf("cannot continue v104\n"); return 1; }

        // stored
        {
            const auto d = bsa.read("meshes\\a\\plain.nif");
            expect(d.size() == 64,
                   "plain.nif size 64 (got " + std::to_string(d.size()) + ")");
            expect(d.size() >= 4 && std::memcmp(d.data(), "MESH", 4) == 0,
                   "plain.nif starts with MESH");
        }

        // case-insensitive lookup, both slash styles
        expect(bsa.has("MESHES\\A\\PLAIN.NIF"), "case-insensitive lookup");
        expect(bsa.has("meshes/a/plain.nif"), "forward-slash lookup");

        // compressed, byte-exact round-trip
        {
            const auto d = bsa.read("textures\\a\\d.dds");
            expect(d.size() == 5000,
                   "d.dds decompresses to 5000 (got " + std::to_string(d.size()) + ")");
            bool contentOk = (d.size() == 5000);
            for (size_t i = 0; contentOk && i < d.size(); i++) {
                const unsigned want =
                    static_cast<unsigned>((i * 37 + (i >> 3) * 11 + (i >> 6)) & 0xFF);
                if (d[i] != static_cast<uint8_t>(want)) contentOk = false;
            }
            expect(contentOk, "d.dds byte-exact round-trip");
        }

        // large compressed
        {
            const auto d = bsa.read("textures\\a\\big.dds");
            expect(d.size() == 40000,
                   "big.dds -> 40000 (got " + std::to_string(d.size()) + ")");
            expect(d.size() == 40000 && d[0] == 'X' && d[39999] == 'X',
                   "big.dds content");
        }

        // stored large file: payload is sector-padded on disk, so this checks
        // that sizeBytes wins over the padded length
        {
            const auto d = bsa.read("strings\\strings0.strings");
            expect(d.size() == 9000,
                   "strings0 -> 9000 (got " + std::to_string(d.size()) + ")");
        }

        // zero-byte file
        expect(bsa.read("meshes\\empty.nif").empty(), "empty.nif reads as empty");

        // nested folder path -- proves folder name + leaf are joined correctly
        {
            const auto d =
                bsa.read("meshes\\architecture\\whiterun\\deep.nif");
            expect(d.size() == 1234,
                   "deep.nif -> 1234 (got " + std::to_string(d.size()) + ")");
            expect(d.size() == 1234 && d[0] == 'D' && d[1233] == 'D',
                   "deep.nif content");
        }

        // leaf-only lookup fallback
        expect(bsa.has("deep.nif"), "leaf-only lookup");

        // extension listing returns full paths
        {
            const auto nifs = bsa.listByExtension("nif");
            expect(nifs.size() == 4,
                   "4 .nif entries (got " + std::to_string(nifs.size()) + ")");
            const auto dds = bsa.listByExtension(".dds");
            expect(dds.size() == 2, "2 .dds entries");
        }

        // metadata
        {
            const skyrim::BsaFileEntry* e = bsa.find("textures\\a\\d.dds");
            expect(e != nullptr, "entry pointer for d.dds");
            if (e) {
                expect(e->compressed, "d.dds flagged compressed");
                expect(e->folderIndex == 1,
                       "d.dds folderIndex == 1 (got " +
                           std::to_string(e->folderIndex) + ")");
                expect(e->fullPath == "textures\\d.dds",
                       "d.dds fullPath joined correctly (got " + e->fullPath + ")");
            }
        }

        expect(bsa.read("does\\not\\exist.nif").empty(), "missing file -> empty");
    }

    // ---- v105: x64 folder records (24-byte stride) ----
    {
        skyrim::BsaArchive bsa;
        expect(bsa.open(dir + "/fixture105.bsa"), "open v105");
        expect(bsa.version() == 105, "v105 version == 105");
        expect(bsa.fileCount() == 3,
               "3 files in v105 (got " + std::to_string(bsa.fileCount()) + ")");
        if (bsa.isOpen()) {
            expect(bsa.read("meshes\\s\\plain.nif").size() == 64, "v105 stored -> 64");
            const auto z = bsa.read("meshes\\s\\zipped.nif");
            expect(z.size() == 2048,
                   "v105 zlib -> 2048 (got " + std::to_string(z.size()) + ")");
            expect(z.size() == 2048 && z[0] == 0 && z[2047] == 255,
                   "v105 zlib content");
            expect(bsa.read("textures\\s\\big.dds").size() == 30000,
                   "v105 large stored -> 30000");
        }
    }

    // ---- v103: the 2011 Steam layout ----
    {
        skyrim::BsaArchive bsa;
        expect(bsa.open(dir + "/fixture103.bsa"), "open v103");
        expect(bsa.version() == 103, "v103 version == 103");
        if (bsa.isOpen()) {
            const auto n = bsa.read("meshes\\v103.nif");
            expect(n.size() == 2048,
                   "v103.nif -> 2048 (got " + std::to_string(n.size()) + ")");
            expect(n.size() == 2048 && n[0] == 0 && n[2047] == 255,
                   "v103.nif content");
            expect(bsa.read("textures\\t.dds").size() == 5000, "v103 t.dds -> 5000");
        }
    }

    // ---- v104 with embedded file names (archiveFlags 0x100) ----
    // Each payload is prefixed with its own full virtual path as a bstring,
    // which is how Skyrim SE writes its texture archives.
    {
        skyrim::BsaArchive bsa;
        expect(bsa.open(dir + "/fixture104_embed.bsa"), "open embedded-names v104");
        expect((bsa.archiveFlags() & 0x100u) != 0, "embedded-names flag set");
        if (bsa.isOpen()) {
            {
                const auto d = bsa.read("textures\\actors\\dog\\dog.dds");
                expect(d.size() == 5000,
                       "embed dog.dds -> 5000 (got " + std::to_string(d.size()) + ")");
                bool ok = (d.size() == 5000);
                for (size_t i = 0; ok && i < d.size(); i++) {
                    const unsigned want =
                        static_cast<unsigned>((i * 37 + (i >> 3) * 11 + (i >> 6)) & 0xFF);
                    if (d[i] != static_cast<uint8_t>(want)) ok = false;
                }
                expect(ok, "embed dog.dds byte-exact");
            }
            {
                const auto d = bsa.read("textures\\clutter\\food\\bread.dds");
                expect(d.size() == 7000,
                       "embed bread.dds -> 7000 (got " + std::to_string(d.size()) + ")");
                expect(d.size() == 7000 && d[0] == 'B' && d[6999] == 'B',
                       "embed bread.dds content");
            }
            // stored (not compressed) with an embedded name prefix
            {
                const auto d = bsa.read("meshes\\a\\plain.nif");
                expect(d.size() == 64,
                       "embed plain.nif -> 64 (got " + std::to_string(d.size()) + ")");
                expect(d.size() == 64 && std::memcmp(d.data(), "MESH", 4) == 0,
                       "embed plain.nif content");
            }
        }
    }

    // ---- robustness ----
    {
        skyrim::BsaArchive bsa;
        expect(!bsa.open(dir + "/does_not_exist.bsa"), "missing archive rejected");
        expect(!bsa.isOpen(), "state clean after failed open");
    }
    {
        // a file that is not a BSA at all
        skyrim::BsaArchive bsa;
        expect(!bsa.open(dir + "/expected.json"), "non-BSA file rejected");
        expect(!bsa.isOpen(), "state clean after bad magic");
    }

    if (failures == 0) {
        std::printf("ALL BSA TESTS PASSED\n");
        return 0;
    }
    std::printf("%d BSA CHECK(S) FAILED\n", failures);
    return 1;
}
