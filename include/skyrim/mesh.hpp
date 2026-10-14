// mesh.hpp - Mesh data structures for runtime NIF loading.
//
// Represents geometry extracted from NIF files (Skyrim SE/LE NIF 20.2.0.7).
// The host uploads this to GPU; the library remains renderer-free.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace skyrim {

struct Vertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    float u = 0.0f, v = 0.0f;
};

struct Submesh {
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
    std::string texturePath;
    uint32_t alphaTest = 0;
    float fallbackR = 1.0f, fallbackG = 1.0f, fallbackB = 1.0f;
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Submesh> submeshes;
    bool valid() const { return !vertices.empty() && !indices.empty(); }
};

bool loadNif(const uint8_t* data, size_t size, Mesh& out,
              std::string* error = nullptr);

bool loadNifFromFile(const std::string& path, Mesh& out,
                     std::string* error = nullptr);

}  // namespace skyrim
