// engine.cpp - Windows implementation of the playable host entry point.

#include "engine.hpp"

#include <cstdio>

#include "game.hpp"

namespace skyrim::host {

bool engineAvailable() { return true; }

int runEngine(const EngineOptions& options) {
    GameOptions game;
    game.dataDir = options.dataDir;
    game.plugins = options.plugins;
    game.archives = options.archives;
    game.worldspace = options.worldspace;
    game.gridX = options.gridX;
    game.gridY = options.gridY;
    game.radius = options.radius;
    game.width = options.width;
    game.height = options.height;
    game.maxFrames = options.frames;
    game.shotPath = options.shotPath;
    game.haveCam = options.haveCam;
    game.camX = options.camX;
    game.camY = options.camY;
    game.camZ = options.camZ;
    game.lookYawDeg = options.lookYawDeg;
    game.lookPitchDeg = options.lookPitchDeg;
    game.fovDeg = options.fovDeg;
    game.exposure = options.exposure;

    Game session;
    std::string err;
    if (!session.init(game, &err)) {
        std::printf("cannot start the engine: %s\n", err.c_str());
        return 1;
    }
    const int rc = session.run();
    session.shutdown();
    return rc;
}

}  // namespace skyrim::host
