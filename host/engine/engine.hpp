// engine.hpp - the windowed playable host, presented to main() behind a stable
// interface so the CLI stays portable even though the renderer is Windows-only.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skyrim::host {

struct EngineOptions {
    std::string dataDir;                 // user's own Data folder
    std::vector<std::string> plugins;    // extra plugins
    std::vector<std::string> archives;   // explicit .bsa list; empty = scan dataDir
    uint32_t worldspace = 0x0000003C;    // Tamriel
    int gridX = 0;
    int gridY = 0;
    int radius = 1;
    int width = 1280;
    int height = 720;
    int frames = 0;                      // >0 renders that many frames then exits
};

// True when this build has a real renderer (Windows).
bool engineAvailable();

// Loads the world, opens a window and runs until it is closed. Returns a
// process exit code. On a platform with no renderer it reports so and fails.
int runEngine(const EngineOptions& options);

}  // namespace skyrim::host
