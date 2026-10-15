// cell_system.cpp - see include/skyrim/cell_system.hpp.
//
// Builds the cell hierarchy by walking the ESM files with parent tracking.
// The form database stores records flat, so CELL -> REFR parentage must be
// recovered here by re-walking the file and noting which CELL each reference
// belongs to, and which worldspace each exterior cell belongs to.
//
// ESM structure relied upon:
//   TES4 header
//   GRUP "CELL" (top level)
//     GRUP gtype 2 -> GRUP gtype 3 -> CELL (interior) -> child GRUP -> REFR
//   GRUP "WRLD" (top level)
//     WRLD record
//     GRUP gtype 1 (label = WRLD FormID; its "world children")
//       CELL (the worldspace persistent pseudo-cell, XCLC 0,0)
//       GRUP gtype 4 -> GRUP gtype 5 -> CELL (exterior) -> child GRUP -> REFR
//
// A GRUP header is 24 bytes and groupSize INCLUDES that header. A plain
// record's dataSize is payload-only, so it spans header + payload.

#include "skyrim/cell_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "skyrim/string_table.hpp"
#include "world/inflate.hpp"

namespace skyrim {
namespace {

constexpr uint32_t kRecordHeaderSize = 24;
constexpr uint32_t kGroupHeaderSize = 24;
constexpr uint32_t kSubHeaderSize = 6;
constexpr uint64_t kMaxRecords = 8'000'000;
constexpr int kMaxGroupDepth = 8;
constexpr size_t kNoCell = static_cast<size_t>(-1);

constexpr uint32_t kFlagCompressed = 0x00040000u;

RecordType rdType(const uint8_t* p) {
    return (static_cast<RecordType>(p[0]) << 24) |
           (static_cast<RecordType>(p[1]) << 16) |
           (static_cast<RecordType>(p[2]) <<  8) |
            static_cast<RecordType>(p[3]);
}

uint32_t rdU16(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
}

uint32_t rdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

float rdF32(const uint8_t* p) {
    uint32_t bits = rdU32(p);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Find a subrecord by type within [p, p+size). Returns pointer and size.
// A subrecord header is: char[4] type, u16 size, u8[size] data.
bool findSub(const uint8_t* p, uint32_t size, RecordType type,
             const uint8_t** out, uint32_t* outSize) {
    while (size >= kSubHeaderSize) {
        const RecordType t = rdType(p);
        const uint32_t s = rdU16(p + 4);
        p += kSubHeaderSize;
        size -= kSubHeaderSize;
        if (size < s) return false;
        if (t == type) { *out = p; *outSize = s; return true; }
        p += s;
        size -= s;
    }
    return false;
}

// Context for the recursive walk. Uses an index rather than a Cell* so it
// stays valid when the cell vector reallocates.
struct WalkState {
    size_t   currentCell = kNoCell;         // kNoCell when not inside a CELL
    FormID   currentWorldspace = kFormIDNull;  // set inside a gtype 1 GRUP
    // Depth at which cells are direct children of the current worldspace's
    // children group; a cell at that depth is the persistent pseudo-cell.
    int      worldChildCellDepth = -1;
    uint64_t recordCount = 0;
};

// Forward declaration: recursive walker over [p, end).
bool walk(const uint8_t* p, const uint8_t* end, int depth, WalkState& st,
          std::vector<Cell>& cells);

// Recursive record walker. Collects CELL records and their child references.
bool walk(const uint8_t* p, const uint8_t* end, int depth, WalkState& st,
          std::vector<Cell>& cells) {
    if (depth > kMaxGroupDepth) return false;

    while (static_cast<size_t>(end - p) >= kRecordHeaderSize) {
        const RecordType type = rdType(p);
        const uint32_t dataSize = rdU32(p + 4);
        const uint32_t flags = rdU32(p + 8);
        const uint32_t localId = rdU32(p + 12);

        if (type == rec::kGRUP) {
            if (dataSize < kGroupHeaderSize) return false;
            const uint8_t* groupEnd = p + dataSize;
            if (groupEnd > end) return false;

            // The group label and type occupy the same offsets a plain record
            // uses for flags/formID. A "world children" group (gtype 1) names
            // its worldspace in the label, which is the WRLD record's FormID.
            const uint32_t label = rdU32(p + 8);
            const int32_t groupType = static_cast<int32_t>(rdU32(p + 12));
            const FormID savedWs = st.currentWorldspace;
            const int savedDepth = st.worldChildCellDepth;
            if (groupType == kGroupTypeWorldChildren) {
                st.currentWorldspace = makeFormID(0, label);  // source patched later
                st.worldChildCellDepth = depth + 1;
            }

            const uint8_t* children = p + kGroupHeaderSize;
            if (!walk(children, groupEnd, depth + 1, st, cells)) return false;

            st.currentWorldspace = savedWs;
            st.worldChildCellDepth = savedDepth;
            p = groupEnd;
            continue;
        }

        // Plain record: header + payload.
        const uint8_t* body = p + kRecordHeaderSize;
        if (static_cast<size_t>(end - body) < dataSize) return false;

        if (localId != 0 && st.recordCount < kMaxRecords) {
            ++st.recordCount;

            // Decompress if needed. A compressed record body is framed by a u32
            // decompressed size followed by a zlib stream; zlibInflateRecord
            // handles that framing.
            std::vector<uint8_t> payload;
            const uint8_t* payloadData = body;
            if (flags & kFlagCompressed) {
                if (!zip::zlibInflateRecord(body, dataSize, payload)) {
                    payload.clear();
                }
                payloadData = payload.data();
            } else {
                payload.assign(body, body + dataSize);
                payloadData = payload.data();
            }
            const uint32_t payloadSize = static_cast<uint32_t>(payload.size());

            if (type == rec::kCELL) {
                // Start a new cell context.
                Cell cell;
                cell.formId = makeFormID(0, localId);       // source patched later
                cell.worldspace = st.currentWorldspace;
                cell.isInterior = true;

                const uint8_t* d = nullptr;
                uint32_t s = 0;
                if (findSub(payloadData, payloadSize, sub::kEDID, &d, &s)) {
                    cell.editorId.assign(reinterpret_cast<const char*>(d),
                                         strnlen(reinterpret_cast<const char*>(d), s));
                }
                if (findSub(payloadData, payloadSize, sub::kFULL, &d, &s)) {
                    cell.name = parseLocalizedString(d, s);
                }
                // XCLC: int32 gridX, int32 gridY (exterior cells only).
                if (findSub(payloadData, payloadSize,
                            makeRecordType('X','C','L','C'), &d, &s) && s >= 8) {
                    cell.gridX = static_cast<int32_t>(rdU32(d));
                    cell.gridY = static_cast<int32_t>(rdU32(d + 4));
                    cell.isInterior = false;
                    // A cell directly under the worldspace's children group is
                    // the persistent pseudo-cell, not a placeable cell.
                    cell.isPersistent = (depth == st.worldChildCellDepth);
                    cell.worldMinX = static_cast<float>(cell.gridX) * kCellSize;
                    cell.worldMinY = static_cast<float>(cell.gridY) * kCellSize;
                    cell.worldMaxX = cell.worldMinX + kCellSize;
                    cell.worldMaxY = cell.worldMinY + kCellSize;
                    cell.centerX = cell.worldMinX + kCellSize * 0.5f;
                    cell.centerY = cell.worldMinY + kCellSize * 0.5f;
                }

                cells.push_back(std::move(cell));
                st.currentCell = cells.size() - 1;
            } else if ((type == rec::kREFR || type == rec::kACHR) &&
                       st.currentCell != kNoCell) {
                CellObject obj;
                obj.formId = makeFormID(0, localId);
                obj.isActor = (type == rec::kACHR);

                const uint8_t* d = nullptr;
                uint32_t s = 0;
                if (findSub(payloadData, payloadSize, sub::kEDID, &d, &s)) {
                    obj.editorId.assign(reinterpret_cast<const char*>(d),
                                        strnlen(reinterpret_cast<const char*>(d), s));
                }
                if (findSub(payloadData, payloadSize, sub::kNAME, &d, &s) && s >= 4) {
                    obj.baseObject = rdU32(d);
                }
                // DATA: 24 bytes = float x,y,z + float rotX,rotY,rotZ.
                if (findSub(payloadData, payloadSize, sub::kDATA, &d, &s) && s >= 24) {
                    obj.x = rdF32(d);
                    obj.y = rdF32(d + 4);
                    obj.z = rdF32(d + 8);
                    obj.rotX = rdF32(d + 12);
                    obj.rotY = rdF32(d + 16);
                    obj.rotZ = rdF32(d + 20);
                }

                cells[st.currentCell].objects.push_back(std::move(obj));
            } else if (type == rec::kLAND && st.currentCell != kNoCell) {
                // Terrain for the current exterior cell (interiors have none).
                decodeLand(payloadData, payloadSize, cells[st.currentCell].land);
            }
        }

        p += kRecordHeaderSize + dataSize;
    }
    return true;
}

// Read a whole file into a buffer.
bool readAll(const std::filesystem::path& path, std::vector<uint8_t>& buf) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0) return false;
    f.seekg(0, std::ios::beg);
    buf.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(buf.data()), size);
    return static_cast<size_t>(f.gcount()) == buf.size();
}

// Patch the source index into a range of cells, their worldspace and objects.
// Each file's records carry a local formID; the high byte is that file's
// load-order index.
void patchSourceIndex(std::vector<Cell>& cells, size_t begin, uint8_t sourceIndex) {
    for (size_t i = begin; i < cells.size(); ++i) {
        Cell& cell = cells[i];
        cell.formId = makeFormID(sourceIndex, formIndexOf(cell.formId));
        if (!isNull(cell.worldspace)) {
            cell.worldspace = makeFormID(sourceIndex, formIndexOf(cell.worldspace));
        }
        for (auto& obj : cell.objects) {
            obj.formId = makeFormID(sourceIndex, formIndexOf(obj.formId));
        }
    }
}

}  // namespace

size_t CellSystem::GridKeyHash::operator()(const GridKey& k) const noexcept {
    // Mix the three fields into a 64-bit value, then fold with a Fibonacci
    // constant and an xorshift so adjacent grid coordinates scatter well.
    uint64_t h = static_cast<uint64_t>(k.worldspace) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(k.gridX)) << 32;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(k.gridY));
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return static_cast<size_t>(h);
}

Status CellSystem::load(const SkyrimConfig& config) {
    clear();
    if (config.dataDirectory.empty()) return Status::NotConfigured;

    std::error_code ec;
    if (!std::filesystem::is_directory(config.dataDirectory, ec)) {
        return Status::NotConfigured;
    }

    const std::filesystem::path dir(config.dataDirectory);
    const std::vector<std::string> masters = {
        "Skyrim.esm", "Update.esm", "Dawnguard.esm", "Hearthfires.esm", "Dragonborn.esm",
    };

    // Walk each master/plugin in load order, collecting cells and their
    // references. Later files override earlier cells with the same FormID.
    std::vector<Cell> collected;
    uint8_t sourceIndex = 0;
    for (const std::string& name : masters) {
        const std::filesystem::path path = dir / name;
        if (!std::filesystem::exists(path, ec)) continue;

        std::vector<uint8_t> buf;
        if (!readAll(path, buf)) { clear(); return Status::ReadFailed; }

        const uint8_t* p = buf.data();
        const uint8_t* end = buf.data() + buf.size();

        // Skip the TES4 header record: 24-byte header + its payload.
        if (static_cast<size_t>(end - p) < kRecordHeaderSize) {
            clear();
            return Status::ParseFailed;
        }
        p += kRecordHeaderSize + rdU32(p + 4);
        if (p > end) { clear(); return Status::ParseFailed; }

        WalkState st;
        const size_t begin = collected.size();
        if (!walk(p, end, 0, st, collected)) {
            clear();
            return Status::ParseFailed;
        }

        patchSourceIndex(collected, begin, sourceIndex);
        ++sourceIndex;
    }

    if (collected.empty()) return Status::ParseFailed;

    // Apply overrides: when a FormID reappears in a later file, its most recent
    // copy replaces the earlier one in place.
    std::unordered_map<FormID, size_t> position;
    position.reserve(collected.size() * 2);
    for (Cell& cell : collected) {
        const auto it = position.find(cell.formId);
        if (it != position.end()) {
            m_cells[it->second] = std::move(cell);
        } else {
            position.emplace(cell.formId, m_cells.size());
            m_cells.push_back(std::move(cell));
        }
    }

    // Build the lookup indexes now that the deque is stable.
    m_persistentCellCount = 0;
    m_byFormId.reserve(m_cells.size() * 2);
    for (const Cell& cell : m_cells) {
        m_byFormId[cell.formId] = &cell;
        if (cell.isPersistent) {
            ++m_persistentCellCount;
        } else if (!cell.isInterior) {
            const GridKey key{cell.worldspace, cell.gridX, cell.gridY};
            m_gridIndex[key] = &cell;
            m_worldspaceCells[cell.worldspace]++;
        }
    }

    m_loaded = true;
    return Status::Ok;
}

void CellSystem::clear() {
    m_cells.clear();
    m_gridIndex.clear();
    m_byFormId.clear();
    m_worldspaceCells.clear();
    m_persistentCellCount = 0;
    m_loaded = false;
}

size_t CellSystem::totalObjectCount() const {
    size_t n = 0;
    for (const auto& cell : m_cells) n += cell.objects.size();
    return n;
}

const Cell* CellSystem::findCell(FormID worldspace, int32_t gridX, int32_t gridY) const {
    const GridKey key{worldspace, gridX, gridY};
    const auto it = m_gridIndex.find(key);
    return (it != m_gridIndex.end()) ? it->second : nullptr;
}

const Cell* CellSystem::findCellById(FormID id) const {
    const auto it = m_byFormId.find(id);
    return (it != m_byFormId.end()) ? it->second : nullptr;
}

std::vector<const Cell*> CellSystem::queryBounds(FormID worldspace, float minX, float minY,
                                                 float maxX, float maxY) const {
    std::vector<const Cell*> out;
    const int32_t minGX = static_cast<int32_t>(std::floor(minX / kCellSize));
    const int32_t minGY = static_cast<int32_t>(std::floor(minY / kCellSize));
    const int32_t maxGX = static_cast<int32_t>(std::floor(maxX / kCellSize));
    const int32_t maxGY = static_cast<int32_t>(std::floor(maxY / kCellSize));
    for (int32_t gy = minGY; gy <= maxGY; ++gy) {
        for (int32_t gx = minGX; gx <= maxGX; ++gx) {
            const Cell* c = findCell(worldspace, gx, gy);
            if (c) out.push_back(c);
        }
    }
    return out;
}

std::vector<std::pair<FormID, size_t>> CellSystem::worldspaceHistogram() const {
    std::vector<std::pair<FormID, size_t>> out(m_worldspaceCells.begin(),
                                               m_worldspaceCells.end());
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

void CellSystem::resolveNames(const class StringResolver& resolver) {
    for (auto& cell : m_cells) {
        if (cell.name.isReference) {
            cell.name.text = resolver.lookup(cell.name.id);
        }
    }
}

}  // namespace skyrim
