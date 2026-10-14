// nif.cpp
#include "skyrim/mesh.hpp"
#include <cstring>
#include <fstream>
#include <vector>
#include <string>

namespace skyrim {
namespace {
inline uint32_t rdU32(const uint8_t* p){
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
inline uint16_t rdU16(const uint8_t* p){
    return (uint16_t)(p[0] | (p[1]<<8));
}
} // namespace

bool loadNifFromFile(const std::string& path, Mesh& out, std::string* error){
    std::ifstream f(path, std::ios::binary);
    if(!f.is_open()){
        if(error) *error = "cannot open: " + path;
        return false;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return loadNif(buf.data(), buf.size(), out, error);
}

bool loadNif(const uint8_t* data, size_t size, Mesh& out, std::string* error){
    out = Mesh{};
    auto fail = [&](const char* r){ if(error) *error = r; return false; };
    if(!data || size < 256) return fail("NIF too small");
    if(error) *error = "NIF parser not fully implemented yet";
    return false;
}
} // namespace skyrim
