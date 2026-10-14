// mesh_probe.cpp - load a NIF with the runtime parser and print what it got.
//
// Validates the mesh loader against a real install (nothing is bundled):
//
//     mesh_probe --archive "<Data>\Skyrim - Meshes.bsa" "meshes\...\foo.nif"
//     mesh_probe C:\path\to\loose.nif
//
// The NIF can come from a BSA archive or from disk. Output is deliberately
// compact so it can be diffed against the author's reference conversion tool.

#include "skyrim/bsa_archive.hpp"
#include "skyrim/mesh.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readLoose(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

void usage() {
    std::printf(
        "usage: mesh_probe [--archive <file.bsa>] <nif-path>\n"
        "  With --archive, <nif-path> is looked up inside the archive.\n"
        "  Without it, <nif-path> is read from disk.\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string archive;
    std::string nif;
    for (int i = 1; i < argc; i++) {
        const std::string k = argv[i];
        if ((k == "--archive" || k == "-a") && i + 1 < argc) {
            archive = argv[++i];
        } else if (k == "--help" || k == "-h") {
            usage();
            return 0;
        } else {
            nif = k;
        }
    }
    if (nif.empty()) {
        usage();
        return 2;
    }

    std::vector<uint8_t> data;
    if (!archive.empty()) {
        skyrim::BsaArchive bsa;
        if (!bsa.open(archive)) {
            std::printf("FAIL open archive: %s\n", archive.c_str());
            return 1;
        }
        data = bsa.read(nif);
        if (data.empty()) {
            std::printf("NOT FOUND in archive: %s\n", nif.c_str());
            return 1;
        }
    } else {
        data = readLoose(nif);
        if (data.empty()) {
            std::printf("FAIL read: %s\n", nif.c_str());
            return 1;
        }
    }

    skyrim::Mesh mesh;
    std::string err;
    const bool ok = skyrim::loadNif(data.data(), data.size(), mesh, &err);

    std::printf("%s  (%zu bytes)\n", nif.c_str(), data.size());
    if (!ok) {
        std::printf("  PARSE FAILED: %s\n", err.c_str());
        return 1;
    }
    std::printf("  %zu verts, %zu indices (%zu tris), %zu groups\n",
                mesh.vertices.size(), mesh.indices.size(),
                mesh.indices.size() / 3, mesh.submeshes.size());
    for (size_t g = 0; g < mesh.submeshes.size(); g++) {
        const skyrim::Submesh& s = mesh.submeshes[g];
        std::printf("  group %zu: idx[%u..%u] tex=%s alpha=%u rgb=(%.2f,%.2f,%.2f)\n",
                    g, s.indexStart, s.indexStart + s.indexCount,
                    s.texturePath.empty() ? "(none)" : s.texturePath.c_str(),
                    s.alphaTest, s.fallbackR, s.fallbackG, s.fallbackB);
    }
    return 0;
}
