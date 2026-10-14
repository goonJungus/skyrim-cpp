// engine_stub.cpp - non-Windows stand-in for the playable host.
//
// The library is fully portable; only the Direct3D 9 renderer is Windows-only.
// On other platforms the CLI still builds and reports that --play is
// unavailable, rather than failing to link.

#include "engine.hpp"

#include <cstdio>

namespace skyrim::host {

bool engineAvailable() { return false; }

int runEngine(const EngineOptions& /*options*/) {
    std::printf(
        "This build has no windowed renderer.\n"
        "The playable host renders through Direct3D 9 and is Windows-only;\n"
        "the library it drives (parsing, cells, meshes) builds everywhere.\n");
    return 1;
}

}  // namespace skyrim::host
