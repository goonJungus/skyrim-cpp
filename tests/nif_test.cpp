// nif_test.cpp - verifies the NIF parser against a synthetic mesh whose bytes
// and expected geometry we built by hand (see make_nif_fixture.py).
//
// The fixture is a single triangle with known vertices, UVs and texture path,
// so a parsing error shows up as a concrete mismatch rather than silence.

#include "skyrim/mesh.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool cond, const std::string& what) {
    if (!cond) { std::printf("  FAIL: %s\n", what.c_str()); failures++; }
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: nif_test <fixture-dir>\n");
        return 2;
    }
    const std::string dir = argv[1];

    // ---- happy path ----
    {
        const auto bytes = readFile(dir + "/triangle.nif");
        expect(!bytes.empty(), "triangle.nif readable");
        skyrim::Mesh mesh;
        std::string err;
        const bool ok = skyrim::loadNif(bytes.data(), bytes.size(), mesh, &err);
        expect(ok, "parse triangle.nif: " + err);
        if (ok) {
            expect(mesh.vertices.size() == 3, "3 vertices");
            expect(mesh.indices.size() == 3, "3 indices (1 triangle)");
            expect(mesh.submeshes.size() == 1, "1 submesh");
            if (mesh.vertices.size() == 3) {
                expect(near(mesh.vertices[0].x, 0.0f) && near(mesh.vertices[0].y, 0.0f) &&
                       near(mesh.vertices[0].z, 0.0f), "v0 = (0,0,0)");
                expect(near(mesh.vertices[1].x, 1.0f) && near(mesh.vertices[1].y, 0.0f),
                       "v1 = (1,0,0)");
                expect(near(mesh.vertices[2].x, 0.0f) && near(mesh.vertices[2].y, 1.0f),
                       "v2 = (0,1,0)");
                expect(near(mesh.vertices[2].u, 0.0f) && near(mesh.vertices[2].v, 1.0f),
                       "v2 uv = (0,1)");
            }
            if (mesh.indices.size() == 3) {
                expect(mesh.indices[0] == 0 && mesh.indices[1] == 1 && mesh.indices[2] == 2,
                       "indices 0,1,2");
            }
            if (mesh.submeshes.size() == 1) {
                expect(mesh.submeshes[0].indexStart == 0 && mesh.submeshes[0].indexCount == 3,
                       "submesh covers all indices");
                expect(mesh.submeshes[0].texturePath == "textures\\test.dds",
                       "diffuse texture path ('" + mesh.submeshes[0].texturePath + "')");
            }
        }
    }

    // ---- robustness ----
    {
        const auto bytes = readFile(dir + "/bad_magic.nif");
        skyrim::Mesh mesh;
        std::string err;
        expect(!skyrim::loadNif(bytes.data(), bytes.size(), mesh, &err),
               "bad magic rejected");
    }
    {
        const auto bytes = readFile(dir + "/truncated.nif");
        skyrim::Mesh mesh;
        std::string err;
        expect(!skyrim::loadNif(bytes.data(), bytes.size(), mesh, &err),
               "truncated rejected");
    }
    {
        skyrim::Mesh mesh;
        std::string err;
        expect(!skyrim::loadNif(nullptr, 0, mesh, &err), "null buffer rejected");
    }

    if (failures == 0) {
        std::printf("ALL NIF TESTS PASSED\n");
        return 0;
    }
    std::printf("%d NIF CHECK(S) FAILED\n", failures);
    return 1;
}
