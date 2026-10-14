// skyrim_module.hpp - Primary master header & initialization API.
//
// PUBLIC API of the Skyrim engine module. This is the only header the
// integrator needs to include to drive a Skyrim world.
//
// Design rules (see DIRECTIVE.md):
//   * No main(). The module is a library; a host links it and drives
//     ISkyrimWorld::init/update/shutdown.
//   * No game assets in the source tree. Everything is read at runtime from
//     the user's own installation, located via SkyrimConfig::dataDirectory.
//   * Cross-platform (Windows/macOS/Linux), 64-bit, C++20, no compiler
//     extensions. Standard library only.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skyrim {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// Where the user's own game files live, and how much of them to load.
//
// Layout expected under dataDirectory (the usual Steam/Bethesda layout):
//     <dataDirectory>/Skyrim.esm
//     <dataDirectory>/Update.esm
//     <dataDirectory>/<Plugin>.esp
//     <dataDirectory>/<Archive>.bsa
//
// dataDirectory is passed in rather than auto-detected so the module has no
// platform-specific registry/Steam-path knowledge. Hosts that want autodetect
// resolve it themselves and pass the result here.
struct SkyrimConfig {
    // Folder containing *.esm / *.esp / *.bsa. Empty = not configured.
    std::string dataDirectory;

    // Plugin load order after the masters. FormID overrides are applied in
    // order, so later entries win, matching Bethesda's rule that later
    // plugins overwrite earlier ones.
    std::vector<std::string> plugins;

    // Read *.bsa archives as well as loose masters/plugins. BSAs carry the
    // bulk of the art assets (meshes/textures); master data does not.
    bool loadArchives = true;

    // Upper bound on records held in the form database. Guards against a
    // corrupt or hostile file exhausting memory. 0 = module default.
    uint64_t maxRecords = 0;

    // Optional log sink. When null, the module writes nothing to stdout; a
    // host supplies its own logging. Messages are already formatted.
    void (*logSink)(const char* message) = nullptr;
};

// ---------------------------------------------------------------------------
// Status reporting
// ---------------------------------------------------------------------------

enum class Status {
    Ok = 0,
    NotConfigured,      // dataDirectory empty or does not exist
    MasterMissing,      // Skyrim.esm not found
    ReadFailed,         // I/O error reading a master/plugin
    ParseFailed,        // structurally invalid ESM/ESP data
    TooManyRecords,     // exceeded maxRecords
    ArchiveFailed,      // a .bsa could not be read (non-fatal unless strict)
};

const char* toString(Status s);

// ---------------------------------------------------------------------------
// ISkyrimWorld - engine-facing lifecycle
// ---------------------------------------------------------------------------

class FormDatabase;
class StringResolver;

class ISkyrimWorld {
public:
    virtual ~ISkyrimWorld() = default;

    // Load configuration and bring up the world. On success the module owns
    // a form database populated from the user's masters/plugins/archives.
    // Calling init() on an already-initialized world shuts it down first.
    virtual Status init(const SkyrimConfig& config) = 0;

    // Advance simulation by deltaTime seconds. Safe to call before init()
    // (no-op) and after shutdown() (no-op).
    virtual void update(float deltaTime) = 0;

    // Release all resources. Idempotent.
    virtual void shutdown() = 0;

    // Non-null once init() has returned Ok; owned by the world.
    virtual FormDatabase* database() = 0;
    virtual const FormDatabase* database() const = 0;

    // Localized string resolution. FULL/DESC on Skyrim SE are 4-byte string
    // table ids; this turns them into text. May resolve nothing if the install
    // ships its strings only inside BSA archives.
    virtual StringResolver& strings() = 0;
    virtual const StringResolver& strings() const = 0;

    // Language used for loose .STRINGS lookups, e.g. "English". Call before
    // init(). Default "English".
    virtual void setLanguage(std::string language) = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

// Create a world instance. Never returns null. The caller owns the result and
// destroys it with destroySkyrimWorld().
ISkyrimWorld* createSkyrimWorld();

// Destroy a world created by createSkyrimWorld(). Passing null is a no-op.
// Calls shutdown() first if the world is still initialized.
void destroySkyrimWorld(ISkyrimWorld* world);

// Library version, for host-side feature gating.
struct ModuleVersion {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
};
ModuleVersion moduleVersion();

}  // namespace skyrim
