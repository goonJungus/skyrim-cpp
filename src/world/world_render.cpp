// world_render.cpp - cell -> placed models -> cached meshes.

#include "skyrim/world_render.hpp"

#include "skyrim/bsa_archive.hpp"

#include <cctype>
#include <utility>

namespace skyrim {
namespace {

constexpr RecordType kMODL = makeRecordType('M', 'O', 'D', 'L');

bool hasPrefixIgnoreCase(const std::string& s, const char* prefix) {
    size_t n = 0;
    while (prefix[n] != '\0') n++;
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string ModelResolver::modelPath(FormID base) const {
    if (m_db == nullptr || base == kFormIDNull) return {};
    const TESForm* form = m_db->find(base);
    if (form == nullptr) return {};

    std::string path;
    form->forEachSubrecord([&](const Subrecord& sr) {
        if (sr.type != kMODL) return true;
        // MODL is a NUL-terminated path in the record's own encoding.
        const char* p = reinterpret_cast<const char*>(sr.data);
        size_t n = 0;
        while (n < sr.size && p[n] != '\0') n++;
        path.assign(p, n);
        return false;  // stop at the first MODL
    });
    return path;
}

std::vector<PlacedModel> instancesForCell(const Cell& cell,
                                          const ModelResolver& resolver) {
    std::vector<PlacedModel> out;
    out.reserve(cell.objects.size());
    for (const CellObject& o : cell.objects) {
        std::string path = resolver.modelPath(o.baseObject);
        if (path.empty()) continue;
        PlacedModel m;
        m.modelPath = std::move(path);
        m.x = o.x;
        m.y = o.y;
        m.z = o.z;
        m.rotX = o.rotX;
        m.rotY = o.rotY;
        m.rotZ = o.rotZ;
        m.baseObject = o.baseObject;
        out.push_back(std::move(m));
    }
    return out;
}

void MeshCache::clear() {
    m_meshes.clear();
    m_failed.clear();
    m_hits = 0;
    m_misses = 0;
}

const Mesh* MeshCache::get(const std::string& modelPath) {
    if (modelPath.empty()) return nullptr;

    // Archives key on lowercase, backslash-separated paths.
    std::string key;
    key.reserve(modelPath.size());
    for (char c : modelPath) {
        if (c == '/') {
            key.push_back('\\');
        } else {
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }

    auto it = m_meshes.find(key);
    if (it != m_meshes.end()) {
        m_hits++;
        return &it->second;
    }
    if (m_failed.find(key) != m_failed.end()) {
        m_misses++;
        return nullptr;
    }

    std::vector<std::string> candidates;
    candidates.push_back(key);
    // MODL paths omit the "meshes\\" root that archives store.
    if (!hasPrefixIgnoreCase(key, "meshes\\")) candidates.push_back("meshes\\" + key);

    for (const BsaArchive* bsa : m_archives) {
        if (bsa == nullptr) continue;
        for (const std::string& cand : candidates) {
            if (!bsa->has(cand)) continue;
            const std::vector<uint8_t> data = bsa->read(cand);
            if (data.empty()) continue;
            Mesh mesh;
            if (loadNif(data.data(), data.size(), mesh, nullptr) && mesh.valid()) {
                auto res = m_meshes.emplace(key, std::move(mesh));
                m_hits++;
                return &res.first->second;
            }
        }
    }

    m_failed[key] = true;
    m_misses++;
    return nullptr;
}

}  // namespace skyrim
