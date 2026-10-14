// texture.hpp - DDS texture decoding.
//
// Skyrim ships its art in *.bsa archives as DDS (DirectDraw Surface)
// textures. Nothing renders until they decode to plain RGBA, so this turns the
// raw bytes handed over by the archive reader into an 8-bit-per-channel image.
//
// Design notes:
//   * Decoded from a memory buffer, not a path. Textures normally arrive from
//     a .bsa entry; the same function is used for loose files by reading them
//     into a vector first.
//   * Only the top mip level is decoded. Distant mips matter for a shipping
//     renderer, but not for bringing the world up, and decoding them would
//     dominate load time.
//   * Cross-platform, standard library only. No Direct3D types leak out; the
//     host uploads the RGBA buffer to whatever API it owns.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace skyrim {

// A decoded image: tightly packed, top-left origin, RGBA8.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // width * height * 4 bytes

    bool valid() const {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    }
};

// Texture storage formats we recognise.
enum class TextureFormat {
    Unknown = 0,
    BC1,      // DXT1  - RGB + 1-bit alpha
    BC2,      // DXT3  - RGB + 4-bit explicit alpha
    BC3,      // DXT5  - RGB + interpolated alpha
    BC4,      // ATI1  - single channel (recognised, not yet decoded)
    BC5,      // ATI2  - two channels   (recognised, not yet decoded)
    BC7,      // DXGI  - modern block compression (recognised, not yet decoded)
    Bgra8,    // uncompressed 32-bit
    Bgrx8,    // uncompressed 32-bit, alpha ignored
};

const char* toString(TextureFormat f);

// Header-only description of a DDS image. No pixel work.
struct TextureInfo {
    int width = 0;
    int height = 0;
    int mipCount = 1;
    TextureFormat format = TextureFormat::Unknown;
    bool compressed = false;   // true for the BC* block formats
    int blockBytes = 0;        // bytes per 4x4 block when compressed, else 0
};

// Parse the DDS header. Returns false on a non-DDS buffer or a malformed
// header; `error` (when non-null) receives a human-readable reason.
bool inspectDDS(const uint8_t* data, size_t size, TextureInfo& out,
                std::string* error = nullptr);

// Decode the top mip level of a DDS image into RGBA8. Returns false on a
// malformed or unsupported buffer; `error` receives the reason.
bool decodeDDS(const uint8_t* data, size_t size, Image& out,
               std::string* error = nullptr);

}  // namespace skyrim
