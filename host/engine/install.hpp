// install.hpp - finding the player's own Skyrim installation.
//
// The program ships no game data. On first run it looks for a Skyrim install
// the player already owns, remembers the choice, and can fall back to asking
// them where it is. This mirrors how the Harbour Masters ports locate the
// original game: point it at your copy once, then just run it.
//
// Portable interface; the platform-specific searches (the Steam registry and
// the Windows folder picker) are guarded inside install.cpp.

#pragma once

#include <string>

namespace skyrim::host {

struct LocatedInstall {
    std::string dataDir;   // the "Data" folder holding *.esm / *.bsa
    std::string source;    // how it was found, for logging
    bool ok = false;
};

// Resolve the Data folder. Tried in order: the explicit path, the saved
// setting, the SKYRIM_DATA_DIR / SKYRIM_DIR environment variables, next to the
// program, the usual Steam libraries, then (Windows only, when `allowPicker`)
// a folder picker. Returns ok=false when nothing usable was found.
LocatedInstall locateSkyrimData(const std::string& explicitDir, bool allowPicker);

// Remember the chosen Data folder in skyrim_cpp.ini next to the program.
void saveInstallPreference(const std::string& dataDir);

// Directory the running program lives in ("" if it cannot be determined).
std::string executableDir();

}  // namespace skyrim::host
