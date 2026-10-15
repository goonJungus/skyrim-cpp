// main.cpp - skyrim_standalone: the playable program.
//
// Deliberately thin. It resolves where the player's game files live, then either
// prints what the library loaded or hands control to the windowed engine. The
// simulation, rendering and collision all live in the library and host/engine/.

#include "skyrim/skyrim_module.hpp"
#include "skyrim/form_database.hpp"
#include "skyrim/string_table.hpp"
#include "engine/engine.hpp"
#include "engine/install.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void logLine(const char* msg) {
    std::printf("  %s\n", msg);
}

// Minimal arg parsing: --data <dir>  --plugin <file>  --types  --limit <n>
// plus the playable mode: --play [--ws hex] [--grid X Y] [--radius N]
//                             [--meshes <bsa>]... [--width N] [--height N]
struct Args {
    std::string dataDir;
    std::vector<std::string> plugins;
    std::vector<std::string> archives;
    bool listTypes = false;
    bool play = false;
    uint64_t limit = 0;
    uint32_t worldspace = 0x0000003C;
    int gridX = 0;
    int gridY = 0;
    int radius = 4;
    int farRadius = 32;
    int width = 1280;
    int height = 720;
    int frames = 0;
    std::string shot;
    bool haveCam = false;
    float camX = 0.0f, camY = 0.0f, camZ = 0.0f;
    float lookYaw = 0.0f, lookPitch = 0.0f;
    float fov = 68.0f;
    float exposure = 1.8f;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        const std::string k = argv[i];
        if ((k == "--data" || k == "-d" || k == "--game" || k == "-g") && i + 1 < argc) {
            a.dataDir = argv[++i];
        } else if (k == "--plugin" && i + 1 < argc) {
            a.plugins.emplace_back(argv[++i]);
        } else if (k == "--play") {
            a.play = true;
        } else if (k == "--meshes" && i + 1 < argc) {
            a.archives.emplace_back(argv[++i]);
        } else if (k == "--ws" && i + 1 < argc) {
            a.worldspace = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16));
        } else if (k == "--grid" && i + 2 < argc) {
            a.gridX = std::atoi(argv[++i]);
            a.gridY = std::atoi(argv[++i]);
        } else if (k == "--radius" && i + 1 < argc) {
            a.radius = std::atoi(argv[++i]);
        } else if (k == "--far" && i + 1 < argc) {
            a.farRadius = std::atoi(argv[++i]);
        } else if (k == "--width" && i + 1 < argc) {
            a.width = std::atoi(argv[++i]);
        } else if (k == "--height" && i + 1 < argc) {
            a.height = std::atoi(argv[++i]);
        } else if (k == "--frames" && i + 1 < argc) {
            a.frames = std::atoi(argv[++i]);
        } else if (k == "--shot" && i + 1 < argc) {
            a.shot = argv[++i];
        } else if (k == "--cam" && i + 3 < argc) {
            a.haveCam = true;
            a.camX = static_cast<float>(std::atof(argv[++i]));
            a.camY = static_cast<float>(std::atof(argv[++i]));
            a.camZ = static_cast<float>(std::atof(argv[++i]));
        } else if (k == "--look" && i + 2 < argc) {
            a.lookYaw = static_cast<float>(std::atof(argv[++i]));
            a.lookPitch = static_cast<float>(std::atof(argv[++i]));
        } else if (k == "--fov" && i + 1 < argc) {
            a.fov = static_cast<float>(std::atof(argv[++i]));
        } else if (k == "--exposure" && i + 1 < argc) {
            a.exposure = static_cast<float>(std::atof(argv[++i]));
        } else if (k == "--types") {
            a.listTypes = true;
        } else if (k == "--limit" && i + 1 < argc) {
            a.limit = std::strtoull(argv[++i], nullptr, 10);
        } else if (k == "--help" || k == "-h") {
            std::printf(
                "usage: skyrim_standalone [--data <DataFolder>] [options]\n"
                "  --data <dir>     folder holding *.esm / *.esp / *.bsa\n"
                "                   (also --game; if omitted, the install is\n"
                "                    located automatically and remembered)\n"
                "  --plugin <file>  load an extra plugin (repeatable)\n"
                "  --types          print the record-type histogram\n"
                "  --limit <n>      cap loaded records\n"
                "  --play           launch the windowed, playable world\n"
                "  --ws <hex>       worldspace FormID for --play (default 3C = Tamriel)\n"
                "  --grid <x> <y>   starting exterior cell for --play\n"
                "  --radius <n>     cells streamed around the player (default 4)\n"
                "  --far <n>        cells out to the distant terrain ring (default 32,\n"
                "                   0 turns the ring off)\n"
                "  --meshes <bsa>   archive to load meshes/textures from (repeatable;\n"
                "                   default: every *.bsa in the data folder)\n"
                "  --width/--height window size for --play\n"
                "  --frames <n>     render n frames then exit (smoke test)\n"
                "  --shot <file>    save the final frame as a BMP\n"
                "  --cam <x> <y> <z> place a fixed camera (implies no simulation)\n"
                "  --look <yawDeg> <pitchDeg>  fixed camera angles with --cam\n"
                "  --fov <deg>      vertical field of view (default 68)\n"
                "  --exposure <n>   post-lighting gain, 1..2 (default 1.8)\n");
            std::exit(0);
        }
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);

    const skyrim::ModuleVersion v = skyrim::moduleVersion();
    std::printf("skyrim_standalone  (skyrim_module %u.%u.%u)\n", v.major, v.minor, v.patch);

    // Resolve the player's own game folder. A --data path wins; otherwise the
    // saved setting, the environment, the program folder, Steam, then a folder
    // picker are tried in turn.
    const bool explicitDir = !args.dataDir.empty();
    std::string dataDir = args.dataDir;
    {
        const skyrim::host::LocatedInstall install = skyrim::host::locateSkyrimData(dataDir, true);
        if (install.ok) {
            dataDir = install.dataDir;
            std::printf("Skyrim data: %s  (%s)\n", install.dataDir.c_str(), install.source.c_str());
            if (!explicitDir) skyrim::host::saveInstallPreference(install.dataDir);
        }
    }
    if (dataDir.empty()) {
        std::printf(
            "\nCould not find a Skyrim install. Point the program at it once:\n"
            "  skyrim_standalone --data \"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Skyrim\\Data\"\n"
            "or set SKYRIM_DIR to your Skyrim folder.\n"
            "The program reads YOUR OWN game files; nothing is bundled with it.\n");
        return 1;
    }

    // Playable mode: hand everything to the windowed engine and run it.
    if (args.play) {
        skyrim::host::EngineOptions engine;
        engine.dataDir = dataDir;
        engine.plugins = args.plugins;
        engine.archives = args.archives;
        engine.worldspace = args.worldspace;
        engine.gridX = args.gridX;
        engine.gridY = args.gridY;
        engine.radius = args.radius;
        engine.farRadius = args.farRadius;
        engine.width = args.width;
        engine.height = args.height;
        engine.frames = args.frames;
        engine.shotPath = args.shot;
        engine.haveCam = args.haveCam;
        engine.camX = args.camX;
        engine.camY = args.camY;
        engine.camZ = args.camZ;
        engine.lookYawDeg = args.lookYaw;
        engine.lookPitchDeg = args.lookPitch;
        engine.fovDeg = args.fov;
        engine.exposure = args.exposure;
        return skyrim::host::runEngine(engine);
    }

    skyrim::SkyrimConfig config;
    config.dataDirectory  = dataDir;
    config.plugins        = args.plugins;
    config.loadArchives   = false;   // archive reader not wired into the world yet
    config.maxRecords     = args.limit;
    config.logSink        = &logLine;

    skyrim::ISkyrimWorld* world = skyrim::createSkyrimWorld();

    const skyrim::Status st = world->init(config);
    if (st != skyrim::Status::Ok) {
        std::printf("\ninit failed: %s\n", skyrim::toString(st));
        skyrim::destroySkyrimWorld(world);
        return 1;
    }

    const skyrim::FormDatabase* db = world->database();

    std::printf("\nsources (%zu):\n", db->sourceCount());
    for (const auto& s : db->sources()) {
        std::printf("  [%u] %-20s %-8s %8llu forms\n",
                    s.index, s.filename.c_str(),
                    s.isMaster ? "master" : "plugin",
                    static_cast<unsigned long long>(s.recordCount));
    }

    std::printf("\nspot checks:\n");
    struct Probe { skyrim::RecordType t; const char* label; };
    const Probe probes[] = {
        { skyrim::rec::kCELL, "CELL" },
        { skyrim::rec::kWRLD, "WRLD" },
        { skyrim::rec::kREFR, "REFR" },
        { skyrim::rec::kACHR, "ACHR" },
        { skyrim::rec::kNPC_, "NPC_" },
        { skyrim::rec::kSTAT, "STAT" },
        { skyrim::rec::kFACT, "FACT" },
        { skyrim::rec::kPERK, "PERK" },
        { skyrim::rec::kSKIL, "SKIL" },
        { skyrim::rec::kSPEL, "SPEL" },
        { skyrim::rec::kSCPT, "SCPT" },
    };
    for (const Probe& p : probes) {
        std::printf("  %-5s %8zu\n", p.label, db->countOf(p.t));
    }

    if (args.listTypes) {
        std::printf("\nrecord types (top 30):\n");
        const auto hist = db->typeHistogram();
        for (size_t i = 0; i < hist.size() && i < 30; ++i) {
            std::printf("  %-6s %8zu\n", skyrim::toString(hist[i].first).c_str(),
                        hist[i].second);
        }
    }

    // Sample a few named forms to prove subrecord decoding works end to end.
    // Note FULL on Skyrim SE is a 4-byte string-table reference, not text;
    // the id is shown until the BSA string-table reader resolves it.
    std::printf("\nsample forms (FULL is a string-table ref on Skyrim SE):\n");
    int shown = 0;
    for (const skyrim::TESForm* f : db->findAll(skyrim::rec::kWRLD)) {
        const std::string id = f->editorId();
        if (id.empty()) continue;
        const skyrim::LocalizedString full = f->displayNameRaw();
        char nameBuf[64];
        if (full.isReference) {
            const std::string resolved = world->strings().lookup(full.id);
            if (!resolved.empty()) {
                std::snprintf(nameBuf, sizeof(nameBuf), "str#%u = \"%s\"",
                              full.id, resolved.c_str());
            } else {
                std::snprintf(nameBuf, sizeof(nameBuf), "str#%u (unresolved)",
                              full.id);
            }
        } else {
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", full.text.c_str());
        }
        std::printf("  WRLD %06X  %-28s %s\n", skyrim::formIndexOf(f->id()),
                    id.c_str(), nameBuf);
        if (++shown >= 8) break;
    }

    // Drive the lifecycle once so the host exercises update/shutdown.
    world->update(1.0f / 60.0f);

    world->shutdown();
    skyrim::destroySkyrimWorld(world);

    std::printf("\nclean shutdown.\n");
    return 0;
}
