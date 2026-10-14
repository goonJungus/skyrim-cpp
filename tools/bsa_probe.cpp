// bsa_probe.cpp - command-line harness for the BSA reader.
//
// Prints the directory of one or more archives and, optionally, extracts a
// single entry. Used to verify the reader against a real install:
//
//     bsa_probe --archive "<Data>\Skyrim - Misc.bsa" [--list] [--ext nif]
//               [--count] [--extract "meshes\\foo.nif" --out foo.nif]
//
// No install paths are compiled in; everything is a runtime argument.

#include "skyrim/bsa_archive.hpp"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

struct Args {
    std::vector<std::string> archives;
    bool list = false;
    bool count = false;
    bool verify = false;
    std::string ext;
    std::string extract;
    std::string out;
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        const std::string k = argv[i];
        if (k == "--archive" && i + 1 < argc)      a.archives.emplace_back(argv[++i]);
        else if (k == "--list")                    a.list = true;
        else if (k == "--count")                   a.count = true;
        else if (k == "--verify")                  a.verify = true;
        else if (k == "--ext" && i + 1 < argc)     a.ext = argv[++i];
        else if (k == "--extract" && i + 1 < argc) a.extract = argv[++i];
        else if (k == "--out" && i + 1 < argc)     a.out = argv[++i];
        else if (k == "--help" || k == "-h") {
            std::printf(
                "usage: bsa_probe --archive <file.bsa> [options]\n"
                "  --list                 list every entry\n"
                "  --count                show a few counts only\n"
                "  --verify               read every entry, report failures\n"
                "  --ext <ext>            list entries with this extension\n"
                "  --extract <name>       read one entry\n"
                "  --out <file>           write the extracted entry here\n");
            std::exit(0);
        }
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse(argc, argv);
    if (args.archives.empty()) {
        std::printf("usage: bsa_probe --archive <file.bsa> [options]  (--help)\n");
        return 2;
    }

    int rc = 0;
    for (const std::string& path : args.archives) {
        skyrim::BsaArchive bsa;
        if (!bsa.open(path)) {
            std::printf("FAIL open: %s\n", path.c_str());
            rc = 1;
            continue;
        }
        std::printf("%s\n", path.c_str());
        std::printf("  version=%u folders=%zu files=%zu uncompressed=%.1f MB\n",
                    bsa.version(), bsa.folderCount(), bsa.fileCount(),
                    static_cast<double>(bsa.totalUncompressedBytes()) /
                        (1024.0 * 1024.0));

        if (args.list) {
            const auto names = bsa.listFiles();
            for (const auto& n : names) std::printf("    %s\n", n.c_str());
        }
        if (!args.ext.empty()) {
            const auto names = bsa.listByExtension(args.ext);
            std::printf("  --ext %s: %zu entries\n", args.ext.c_str(), names.size());
            for (size_t i = 0; i < names.size() && i < 40; i++) {
                std::printf("    %s\n", names[i].c_str());
            }
        }
        if (!args.extract.empty()) {
            const auto* e = bsa.find(args.extract);
            if (!e) {
                std::printf("  --extract: '%s' NOT FOUND\n", args.extract.c_str());
                rc = 1;
            } else {
                const auto data = bsa.read(args.extract);
                std::printf("  --extract '%s': stored=%u bytes, read %zu bytes%s\n",
                            args.extract.c_str(), e->sizeBytes, data.size(),
                            e->compressed ? " (compressed)" : "");
                // A compressed entry expands, so stored size is the floor, not
                // the expected result. An empty read from a non-empty entry is
                // the failure signal.
                if (data.empty() && e->sizeBytes > 0) {
                    std::printf("    DECOMPRESSION FAILED\n");
                    rc = 1;
                } else if (data.size() > 0 && data.size() < e->sizeBytes) {
                    std::printf("    WARNING: read fewer bytes than stored\n");
                    rc = 1;
                }
                if (!args.out.empty() && !data.empty()) {
                    std::ofstream f(args.out, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(data.data()),
                            static_cast<std::streamsize>(data.size()));
                    std::printf("    wrote %s\n", args.out.c_str());
                }
            }
        }

        // Read every entry and report how many decompressed cleanly. This is
        // the real correctness check against a live install: a directory can
        // parse perfectly while payloads are still wrong.
        if (args.verify) {
            size_t ok = 0, failed = 0, empty = 0;
            uint64_t bytes = 0;
            for (const auto& name : bsa.listFiles()) {
                const auto* e = bsa.find(name);
                if (!e) continue;
                if (e->sizeBytes == 0) { empty++; continue; }
                const auto d = bsa.read(name);
                if (d.empty()) {
                    failed++;
                    if (failed <= 3) std::printf("    FAILED: %s\n", name.c_str());
                } else {
                    ok++;
                    bytes += d.size();
                }
            }
            std::printf("  --verify: ok=%zu failed=%zu empty=%zu total=%.1f MB\n",
                        ok, failed, empty,
                        static_cast<double>(bytes) / (1024.0 * 1024.0));
            if (failed) rc = 1;
        }
    }
    return rc;
}
