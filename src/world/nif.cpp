// nif.cpp - Runtime NIF (NetImmerse) mesh parser for The Elder Scrolls V: Skyrim.
//
// Reads the user's own .nif meshes at runtime and extracts renderable geometry:
// positions, normals, UVs, triangle indices, per-shape texture assignment and
// alpha-test flags. No pre-conversion step, no game assets in the repo.
//
// Format notes (NIF version 20.2.0.7, the Skyrim generation):
//
//   header        version, a type-name table, then per-block type indices and
//                 absolute block sizes, then a string table, then block data.
//   blocks        walked in order; sizes come from the header so the data
//                 section is addressable without interpreting each block.
//   geometry      NiTriShape / NiTriStrips reference an NiTriShapeData /
//                 NiTriStripsData block holding the vertex stream.
//   material      the shape references a BSLightingShaderProperty (or
//                 BSEffectShaderProperty) which references a
//                 BSShaderTextureSet holding the diffuse path.
//
// The parser is deliberately tolerant: a malformed shape is skipped, not
// fatal, and shape->data references are recovered by scanning when the
// canonical offset is unusable. This mirrors the well-tested behaviour of the
// author's earlier conversion tool, but runs entirely in-process.

#include "skyrim/mesh.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace skyrim {
namespace {

// ---------------------------------------------------------------------------
// Little-endian readers (buffers may be unaligned)
// ---------------------------------------------------------------------------

uint16_t u16(const uint8_t* d, size_t p) {
    return static_cast<uint16_t>(d[p] | (d[p + 1] << 8));
}

uint32_t u32(const uint8_t* d, size_t p) {
    return static_cast<uint32_t>(d[p]) |
           (static_cast<uint32_t>(d[p + 1]) << 8) |
           (static_cast<uint32_t>(d[p + 2]) << 16) |
           (static_cast<uint32_t>(d[p + 3]) << 24);
}

float f32(const uint8_t* d, size_t p) {
    const uint32_t v = u32(d, p);
    float f = 0.0f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

// A NIF opens with a printable banner, not a magic word: the fixed header
// fields sit at offsets that only the banner's length pins down. The numeric
// version follows the banner's newline. Accept only the Skyrim generation
// (20.2.0.7) that this parser understands.
bool bannerSupported(const uint8_t* d, size_t size) {
    static const char kGamebryo[] = "Gamebryo File Format";
    static const char kNetImmerse[] = "NetImmerse File Format";
    const size_t gb = sizeof(kGamebryo) - 1;
    const size_t ni = sizeof(kNetImmerse) - 1;
    const bool gamebryo = size >= gb && std::memcmp(d, kGamebryo, gb) == 0;
    const bool netimmerse = size >= ni && std::memcmp(d, kNetImmerse, ni) == 0;
    if (!gamebryo && !netimmerse) return false;
    for (size_t i = 0; i + 4 <= size && i < 64; i++) {
        if (d[i] == '\n') return u32(d, i + 1) == 0x14020007u;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

struct Nif {
    const uint8_t* data = nullptr;
    size_t size = 0;
    std::vector<std::string> types;
    std::vector<uint16_t> typeIdx;
    std::vector<uint32_t> blockSizes;
    std::vector<size_t> offsets;
    std::vector<std::string> strings;
    size_t blockStart = 0;

    size_t blockCount() const { return blockSizes.size(); }

    std::string typeName(size_t bi) const {
        if (bi >= typeIdx.size()) return "INVALID";
        const uint16_t ti = typeIdx[bi];
        if (ti >= types.size()) return "INVALID";
        return types[ti];
    }

    std::string blockName(size_t bi) const {
        if (bi >= offsets.size() || offsets[bi] + 4 > size) return "?";
        const uint32_t si = u32(data, offsets[bi]);
        if (si < strings.size()) return strings[si];
        return "?";
    }
};

// Locate the type-name table. Its absolute position varies because the header
// has grown across NIF generations, so it is found by validating a candidate
// list rather than assuming a fixed offset.
bool findTypeList(const uint8_t* d, size_t size, size_t& outStart, uint16_t& outCount) {
    for (size_t start = 48; start < 160; start++) {
        if (start + 2 > size) break;
        const uint16_t nt = u16(d, start);
        if (nt < 5 || nt > 200) continue;
        size_t p = start + 2;
        bool ok = true;
        const uint16_t probe = std::min<uint16_t>(nt, 10);
        for (uint16_t i = 0; i < probe; i++) {
            if (p + 4 > size) { ok = false; break; }
            const uint32_t sl = u32(d, p);
            p += 4;
            if (sl < 2 || sl > 80 || p + sl > size) { ok = false; break; }
            const std::string s(reinterpret_cast<const char*>(d + p), sl);
            if (!(s.rfind("Ni", 0) == 0 || s.rfind("BS", 0) == 0 ||
                  s.rfind("bhk", 0) == 0 || s.rfind("hk", 0) == 0)) {
                ok = false;
                break;
            }
            p += sl;
        }
        if (ok) { outStart = start; outCount = nt; return true; }
    }
    return false;
}

bool parseHeader(const uint8_t* d, size_t size, Nif& nif) {
    if (size < 0x34) return false;
    const uint32_t numBlocks = u32(d, 0x30);
    if (numBlocks == 0 || numBlocks > 1000000) return false;

    size_t tstart = 0;
    uint16_t numTypes = 0;
    if (!findTypeList(d, size, tstart, numTypes)) return false;

    size_t pos = tstart + 2;
    nif.types.reserve(numTypes);
    for (uint16_t i = 0; i < numTypes; i++) {
        if (pos + 4 > size) return false;
        const uint32_t sl = u32(d, pos);
        pos += 4;
        if (sl > 80 || pos + sl > size) return false;
        nif.types.emplace_back(reinterpret_cast<const char*>(d + pos), sl);
        pos += sl;
    }

    if (pos + static_cast<size_t>(numBlocks) * 2 > size) return false;
    nif.typeIdx.resize(numBlocks);
    for (uint32_t i = 0; i < numBlocks; i++) {
        nif.typeIdx[i] = u16(d, pos);
        pos += 2;
    }

    if (pos + static_cast<size_t>(numBlocks) * 4 > size) return false;
    nif.blockSizes.resize(numBlocks);
    for (uint32_t i = 0; i < numBlocks; i++) {
        nif.blockSizes[i] = u32(d, pos);
        pos += 4;
    }

    // string table
    if (pos + 8 > size) return false;
    const uint32_t numStrings = u32(d, pos);
    pos += 4;
    pos += 4;  // unknown
    nif.strings.reserve(numStrings);
    for (uint32_t i = 0; i < numStrings; i++) {
        if (pos + 4 > size) return false;
        const uint32_t sl = u32(d, pos);
        pos += 4;
        if (pos + sl > size) return false;
        nif.strings.emplace_back(reinterpret_cast<const char*>(d + pos), sl);
        pos += sl;
    }

    // groups
    if (pos + 4 > size) return false;
    const uint32_t numGroups = u32(d, pos);
    pos += 4;
    if (pos + static_cast<size_t>(numGroups) * 4 > size) return false;
    pos += static_cast<size_t>(numGroups) * 4;

    nif.blockStart = pos;
    nif.data = d;
    nif.size = size;

    // absolute block offsets
    nif.offsets.resize(numBlocks);
    size_t off = pos;
    for (uint32_t i = 0; i < numBlocks; i++) {
        nif.offsets[i] = off;
        off += nif.blockSizes[i];
    }
    return true;
}

// ---------------------------------------------------------------------------
// Geometry data (NiTriShapeData / NiTriStripsData)
// ---------------------------------------------------------------------------

struct GeomData {
    std::vector<Vertex> verts;
    std::vector<uint32_t> tris;   // flat, 3 per triangle
    bool hasColors = false;
    float avgR = 0.0f, avgG = 0.0f, avgB = 0.0f;
};

bool parseGeomData(const Nif& nif, size_t bi, GeomData& out) {
    const uint8_t* d = nif.data;
    const size_t size = nif.size;
    const size_t boff = nif.offsets[bi];
    const size_t bsz = nif.blockSizes[bi];
    const size_t bend = boff + bsz;
    if (bend > size) return false;

    size_t p = boff + 4;  // skip groupID
    if (p + 2 > bend) return false;
    const uint16_t nv = u16(d, p);
    p += 2;
    if (nv < 1 || nv > 65535) return false;
    p += 3;  // keep, compress, hasVertices

    const size_t vpos = p;
    if (vpos + static_cast<size_t>(nv) * 12 > bend) return false;
    p += static_cast<size_t>(nv) * 12;

    if (p + 2 > bend) return false;
    const uint16_t df = u16(d, p);
    p += 2;
    p += 4;  // materialCRC
    if (p + 1 > bend) return false;
    const uint8_t hasNormals = d[p];
    p += 1;
    const size_t npos = p;
    if (hasNormals) p += static_cast<size_t>(nv) * 12;
    if (df & 4096) p += static_cast<size_t>(nv) * 24;  // tangents + bitangents
    p += 16;  // centre + radius
    if (p + 1 > bend) return false;
    const uint8_t hasColors = d[p];
    p += 1;
    size_t cpos = p;
    if (hasColors) p += static_cast<size_t>(nv) * 16;

    const int nuv = df & 63;
    const size_t uv0 = p;
    if (nuv >= 1) p += static_cast<size_t>(nv) * 8 * static_cast<size_t>(nuv);
    if (p > bend) return false;

    // Vertices.
    out.verts.clear();
    out.verts.reserve(nv);
    for (uint16_t i = 0; i < nv; i++) {
        Vertex v;
        const size_t o = vpos + static_cast<size_t>(i) * 12;
        v.x = f32(d, o);
        v.y = f32(d, o + 4);
        v.z = f32(d, o + 8);
        for (int c = 0; c < 3; c++) {
            const float coord = (c == 0) ? v.x : (c == 1 ? v.y : v.z);
            if (!(std::fabs(coord) < 100000.0f)) return false;
        }
        if (hasNormals) {
            const size_t no = npos + static_cast<size_t>(i) * 12;
            v.nx = f32(d, no);
            v.ny = f32(d, no + 4);
            v.nz = f32(d, no + 8);
        } else {
            v.nx = 0.0f; v.ny = 0.0f; v.nz = 1.0f;
        }
        if (nuv >= 1) {
            const size_t uo = uv0 + static_cast<size_t>(i) * 8;
            v.u = f32(d, uo);
            v.v = f32(d, uo + 4);
        } else {
            v.u = 0.0f; v.v = 0.0f;
        }
        out.verts.push_back(v);
    }

    // Vertex colours (averaged for a flat fallback tint).
    if (hasColors) {
        out.hasColors = true;
        double sr = 0.0, sg = 0.0, sb = 0.0;
        for (uint16_t i = 0; i < nv; i++) {
            const size_t o = cpos + static_cast<size_t>(i) * 16;
            sr += f32(d, o);
            sg += f32(d, o + 4);
            sb += f32(d, o + 8);
        }
        out.avgR = static_cast<float>(sr / nv);
        out.avgG = static_cast<float>(sg / nv);
        out.avgB = static_cast<float>(sb / nv);
    } else {
        cpos = 0;
        (void)cpos;
    }

    // Triangle / strip sections.
    const std::string t = nif.typeName(bi);
    if (t == "NiTriShapeData") {
        if (p + 2 > bend) return false;
        p += 2;        // consistency flags
        p += 4;        // two 0xFFFF pads
        if (p + 2 > bend) return false;
        const uint16_t nt = u16(d, p);
        p += 2;
        if (p + 4 > bend) return false;
        p += 4;        // num triangles (u32)
        if (p + 1 > bend) return false;
        const uint8_t ht = d[p];
        p += 1;
        const size_t tpos = p;
        if (ht) p += static_cast<size_t>(nt) * 6;
        if (p > bend) return false;
        out.tris.reserve(static_cast<size_t>(nt) * 3);
        if (ht) {
            for (uint16_t i = 0; i < nt; i++) {
                const size_t o = tpos + static_cast<size_t>(i) * 6;
                const uint16_t a = u16(d, o), b = u16(d, o + 2), c = u16(d, o + 4);
                if (a < nv && b < nv && c < nv && a != b && b != c && a != c) {
                    out.tris.push_back(a);
                    out.tris.push_back(b);
                    out.tris.push_back(c);
                }
            }
        }
    } else if (t == "NiTriStripsData") {
        if (p + 2 > bend) return false;
        p += 2;
        p += 4;
        if (p + 2 > bend) return false;
        const uint16_t ns = u16(d, p);
        p += 2;
        if (ns > 4096) return false;
        for (uint16_t s = 0; s < ns; s++) {
            if (p + 2 > bend) return false;
            const uint16_t slen = u16(d, p);
            p += 2;
            if (static_cast<size_t>(slen) * 2 > bend - p) return false;
            std::vector<uint16_t> strip(slen);
            for (uint16_t i = 0; i < slen; i++) strip[i] = u16(d, p + static_cast<size_t>(i) * 2);
            p += static_cast<size_t>(slen) * 2;
            for (uint16_t i = 0; i + 2 < slen; i++) {
                uint16_t a = strip[i], b = strip[i + 1], c = strip[i + 2];
                if (i % 2 == 1) std::swap(b, c);
                if (a < nv && b < nv && c < nv && a != b && b != c && a != c) {
                    out.tris.push_back(a);
                    out.tris.push_back(b);
                    out.tris.push_back(c);
                }
            }
        }
    } else {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Material resolution
// ---------------------------------------------------------------------------

std::string resolveTexture(const Nif& nif, size_t shaderBi) {
    if (shaderBi >= nif.blockCount()) return {};
    const std::string st = nif.typeName(shaderBi);
    const size_t so = nif.offsets[shaderBi];
    const size_t ss = nif.blockSizes[shaderBi];

    if (st == "BSLightingShaderProperty" && ss >= 44) {
        const uint32_t tset = u32(nif.data, so + 40);
        if (tset < nif.blockCount() && nif.typeName(tset) == "BSShaderTextureSet") {
            const size_t to = nif.offsets[tset];
            const size_t ts = nif.blockSizes[tset];
            if (to + 4 > nif.size) return {};
            const uint32_t ntex = u32(nif.data, to);
            size_t p = to + 4;
            for (uint32_t i = 0; i < ntex && i < 9; i++) {
                if (p + 4 > to + ts) break;
                const uint32_t sl = u32(nif.data, p);
                p += 4;
                if (p + sl > to + ts) break;
                const std::string s(reinterpret_cast<const char*>(nif.data + p), sl);
                p += sl;
                std::string lower = s;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".dds") == 0 &&
                    (s.find('\\') != std::string::npos || s.find('/') != std::string::npos)) {
                    return s;
                }
            }
        }
    } else if (st == "BSEffectShaderProperty") {
        // No structured texture set; recover the first plausible .dds path.
        for (size_t i = so; i + 4 < so + ss && i + 4 < nif.size; i++) {
            if (!(std::isprint(nif.data[i]) != 0)) continue;
            size_t j = i;
            while (j < so + ss && j < nif.size &&
                   std::isprint(nif.data[j]) != 0) {
                j++;
            }
            const std::string s(reinterpret_cast<const char*>(nif.data + i), j - i);
            const size_t dot = s.find(".dds");
            if (dot != std::string::npos && (s.find('\\') != std::string::npos ||
                                             s.find('/') != std::string::npos)) {
                return s.substr(0, dot + 4);
            }
            i = j;
        }
    }
    return {};
}

bool resolveAlphaTest(const Nif& nif, size_t shapeBi) {
    const size_t boff = nif.offsets[shapeBi];
    const size_t bsz = nif.blockSizes[shapeBi];
    if (bsz < 74) return false;
    const uint32_t nprop = u32(nif.data, boff + 66);
    if (nprop > 16) return false;
    for (uint32_t k = 0; k < nprop; k++) {
        const size_t o = boff + 70 + static_cast<size_t>(k) * 4;
        if (o + 4 > nif.size) break;
        const uint32_t pr = u32(nif.data, o);
        if (pr >= nif.blockCount() || nif.typeName(pr) != "NiAlphaProperty") continue;
        const size_t poff = nif.offsets[pr];
        if (nif.blockSizes[pr] < 14 || poff + 14 > nif.size) continue;
        const uint16_t flags = u16(nif.data, poff + 12);
        if ((flags & 0x200) && !(flags & 0x001)) return true;
    }
    return false;
}

// Recover a shape -> geometry-data reference. Prefer the canonical offset, then
// fall back to scanning the block for any reference that names a data block.
size_t resolveDataRef(const Nif& nif, size_t shapeBi) {
    const size_t boff = nif.offsets[shapeBi];
    const size_t bsz = nif.blockSizes[shapeBi];
    auto isData = [&](size_t bi) {
        const std::string t = nif.typeName(bi);
        return t == "NiTriShapeData" || t == "NiTriStripsData";
    };
    if (bsz >= 76 && boff + 76 <= nif.size) {
        const uint32_t dref = u32(nif.data, boff + 72);
        if (dref < nif.blockCount() && isData(dref)) return dref;
    }
    size_t best = nif.blockCount();
    size_t bestDist = 1000000;
    for (size_t o = 4; o + 4 <= bsz; o += 4) {
        if (boff + o + 4 > nif.size) break;
        const uint32_t cand = u32(nif.data, boff + o);
        if (cand < nif.blockCount() && isData(cand)) {
            const size_t dist = (o > 72) ? (o - 72) : (72 - o);
            if (dist < bestDist) { best = cand; bestDist = dist; }
        }
    }
    return best;
}

struct ShapePlan {
    size_t shapeBi = 0;
    size_t dataBi = 0;
    size_t shaderBi = 0;
    bool isLod = false;
    bool include = false;
    std::string name;
    GeomData geom;
    // Axis-aligned bounds of the parsed vertices, used to prove an LOD shape
    // duplicates a full-detail one.
    bool hasBbox = false;
    float bmin[3] = {0.0f, 0.0f, 0.0f};
    float bmax[3] = {0.0f, 0.0f, 0.0f};
};

void computeBbox(ShapePlan& s) {
    if (s.geom.verts.empty()) return;
    s.hasBbox = true;
    s.bmin[0] = s.bmin[1] = s.bmin[2] = 1e30f;
    s.bmax[0] = s.bmax[1] = s.bmax[2] = -1e30f;
    for (const Vertex& v : s.geom.verts) {
        const float p[3] = {v.x, v.y, v.z};
        for (int i = 0; i < 3; i++) {
            s.bmin[i] = std::min(s.bmin[i], p[i]);
            s.bmax[i] = std::max(s.bmax[i], p[i]);
        }
    }
}

bool bboxContains(const ShapePlan& outer, const ShapePlan& inner, float eps) {
    return inner.bmin[0] >= outer.bmin[0] - eps &&
           inner.bmin[1] >= outer.bmin[1] - eps &&
           inner.bmin[2] >= outer.bmin[2] - eps &&
           inner.bmax[0] <= outer.bmax[0] + eps &&
           inner.bmax[1] <= outer.bmax[1] + eps &&
           inner.bmax[2] <= outer.bmax[2] + eps;
}

bool isShapeType(const std::string& t) {
    return t == "NiTriShape" || t == "NiTriStrips" || t == "BSLODTriShape";
}

}  // namespace

bool loadNifFromFile(const std::string& path, Mesh& out, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        if (error) *error = "cannot open: " + path;
        return false;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    return loadNif(buf.data(), buf.size(), out, error);
}

bool loadNif(const uint8_t* data, size_t size, Mesh& out, std::string* error) {
    out = Mesh{};
    auto fail = [&](const std::string& reason) {
        if (error) *error = reason;
        return false;
    };

    if (!data || size < 0x40) return fail("NIF too small");
    if (!bannerSupported(data, size)) {
        return fail("unsupported or unrecognised NIF header");
    }

    Nif nif;
    if (!parseHeader(data, size, nif)) return fail("malformed NIF header");

    // Plan every shape block.
    std::vector<ShapePlan> plan;
    for (size_t bi = 0; bi < nif.blockCount(); bi++) {
        const std::string t = nif.typeName(bi);
        if (!isShapeType(t)) continue;
        ShapePlan sp;
        sp.shapeBi = bi;
        sp.isLod = (t == "BSLODTriShape");
        sp.name = nif.blockName(bi);
        sp.dataBi = resolveDataRef(nif, bi);
        if (nif.blockSizes[bi] >= 93 && nif.offsets[bi] + 93 <= size) {
            sp.shaderBi = u32(data, nif.offsets[bi] + 89);
        }
        if (sp.dataBi < nif.blockCount() && parseGeomData(nif, sp.dataBi, sp.geom)) {
            sp.include = true;
            computeBbox(sp);
        }
        plan.push_back(std::move(sp));
    }

    // LOD duplicate suppression. A BSLODTriShape is dropped only when it is a
    // proven duplicate of an included full-detail shape -- by name reference
    // ("X:N - L1_X:N" points at "X:N") or by bounding-box containment. Anything
    // else is unique geometry and is kept: a kept duplicate can only z-fight,
    // but a dropped unique shape is a hole.
    {
        std::vector<const ShapePlan*> full;
        for (const ShapePlan& s : plan)
            if (s.include && !s.isLod && !s.geom.tris.empty()) full.push_back(&s);
        for (ShapePlan& s : plan) {
            if (!s.include || !s.isLod) continue;
            bool dup = false;
            std::string base = s.name;
            const size_t dash = base.find(" - ");
            if (dash != std::string::npos) base = base.substr(0, dash);
            while (!base.empty() && (base.back() == ' ' || base.back() == '\t')) base.pop_back();
            for (const ShapePlan* f : full) {
                if (!base.empty() && base == f->name) { dup = true; break; }
                if (s.hasBbox && f->hasBbox) {
                    const float dx = f->bmax[0] - f->bmin[0];
                    const float dy = f->bmax[1] - f->bmin[1];
                    const float dz = f->bmax[2] - f->bmin[2];
                    const float diag = std::sqrt(dx * dx + dy * dy + dz * dz);
                    const float eps = std::max(1.0f, 0.02f * diag);
                    if (bboxContains(*f, s, eps)) { dup = true; break; }
                }
            }
            if (dup) s.include = false;
        }
    }

    // Assemble the mesh.
    for (const ShapePlan& s : plan) {
        if (!s.include || s.geom.verts.empty()) continue;
        const uint32_t vbase = static_cast<uint32_t>(out.vertices.size());
        const uint32_t istart = static_cast<uint32_t>(out.indices.size());

        out.vertices.insert(out.vertices.end(), s.geom.verts.begin(), s.geom.verts.end());
        for (uint32_t idx : s.geom.tris) out.indices.push_back(vbase + idx);

        Submesh sm;
        sm.indexStart = istart;
        sm.indexCount = static_cast<uint32_t>(out.indices.size()) - istart;
        sm.texturePath = resolveTexture(nif, s.shaderBi);
        sm.alphaTest = resolveAlphaTest(nif, s.shapeBi) ? 1u : 0u;
        if (s.geom.hasColors) {
            sm.fallbackR = std::clamp(s.geom.avgR, 0.0f, 1.0f);
            sm.fallbackG = std::clamp(s.geom.avgG, 0.0f, 1.0f);
            sm.fallbackB = std::clamp(s.geom.avgB, 0.0f, 1.0f);
        } else {
            sm.fallbackR = 0.72f;
            sm.fallbackG = 0.68f;
            sm.fallbackB = 0.62f;
        }
        out.submeshes.push_back(std::move(sm));
    }

    if (out.vertices.empty()) return fail("no renderable geometry in NIF");
    return true;
}

}  // namespace skyrim
