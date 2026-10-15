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
    // Per-vertex colour. Models carry the NIF's own vertex colour here (white
    // when the mesh has none or the shader does not ask for it); terrain carries
    // the land-texture layer opacity in `a` so a layer pass knows how strongly it
    // shows at each vertex.
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

struct Submesh {
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
    std::string texturePath;
    uint32_t alphaTest = 0;     // 1 = cutout, from NiAlphaProperty's test bit
    uint32_t alphaBlend = 0;    // 1 = blended, from NiAlphaProperty's blend bit
    uint32_t vertexColors = 0;  // 1 = r/g/b above is a real NIF vertex colour
    uint32_t colorsPresent = 0; // 1 = the NIF had vertex colours, applied or not
    uint32_t shaderFlags1 = 0;  // BSLightingShaderProperty Shader Flags 1
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
