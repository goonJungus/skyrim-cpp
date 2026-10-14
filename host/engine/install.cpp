// install.cpp - locating the player's Skyrim install.
//
// Portable core plus platform-specific discovery. Windows adds the Steam
// registry, the Steam library list and a folder picker; other platforms rely on
// the explicit path, the saved setting, the environment, or the program folder.
// No game data is read here - only the presence of the game's own files.

#include "install.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <objbase.h>
#include <shlobj.h>
#include <windows.h>
#endif

namespace skyrim::host {
namespace {

namespace fs = std::filesystem;

// A folder qualifies if the game's master file, or the stock mesh archive, is
// present. That is enough to know it is a Skyrim "Data" folder and not, say,
// the install root or an unrelated directory.
bool looksLikeDataDir(const std::string& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    if (fs::exists(fs::path(dir) / "Skyrim.esm", ec)) return true;
    if (fs::exists(fs::path(dir) / "Skyrim - Meshes.bsa", ec)) return true;
    return false;
}

// Accept either the Data folder itself or the install root that contains it.
std::string normalizeDataDir(const std::string& dir) {
    if (dir.empty()) return dir;
    if (looksLikeDataDir(dir)) return dir;
    const std::string sub = (fs::path(dir) / "Data").string();
    if (looksLikeDataDir(sub)) return sub;
    return dir;
}

std::string iniPath() {
    const std::string dir = executableDir();
    if (dir.empty()) return {};
    return (fs::path(dir) / "skyrim_cpp.ini").string();
}

std::string readSavedPreference() {
    const std::string path = iniPath();
    if (path.empty()) return {};
    std::ifstream in(path);
    if (!in) return {};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        constexpr const char* kKey = "dataDir=";
        if (line.rfind(kKey, 0) == 0) return line.substr(std::string(kKey).size());
    }
    return {};
}

#ifdef _WIN32

std::string wideToUtf8(const wchar_t* w) {
    if (w == nullptr) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

// HKCU\Software\Valve\Steam "SteamPath" - the primary Steam install location.
std::string steamPath() {
    wchar_t buf[32768];
    DWORD size = sizeof(buf);
    const LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam",
                                    L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size);
    if (st != ERROR_SUCCESS) return {};
    return wideToUtf8(buf);
}

// Every Steam library root: the main install plus each "path" in
// steamapps/libraryfolders.vdf. The values are VDF-escaped ("C:\\Games").
std::vector<std::string> steamLibraries(const std::string& root) {
    std::vector<std::string> libs;
    if (!root.empty()) libs.push_back(root);

    std::ifstream in((fs::path(root) / "steamapps" / "libraryfolders.vdf").string());
    std::string line;
    while (std::getline(in, line)) {
        const size_t key = line.find("\"path\"");
        if (key == std::string::npos) continue;
        const size_t open = line.find('"', key + 6);
        if (open == std::string::npos) continue;
        const size_t close = line.find('"', open + 1);
        if (close == std::string::npos) continue;

        const std::string raw = line.substr(open + 1, close - open - 1);
        std::string unescaped;
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') {
                unescaped.push_back('\\');
                i++;
            } else {
                unescaped.push_back(raw[i]);
            }
        }
        if (!unescaped.empty()) libs.push_back(unescaped);
    }
    return libs;
}

// Last resort: ask the player. Opens the modern folder dialog; the returned
// path may be the Data folder or the install root (normalizeDataDir handles it).
std::string pickDataFolder() {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    std::string result;
    IFileDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                               FOS_PATHMUSTEXIST);
        }
        dialog->SetTitle(L"Select your Skyrim \"Data\" folder");
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = wideToUtf8(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}

#endif  // _WIN32

}  // namespace

std::string executableDir() {
#ifdef _WIN32
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return {};
    std::wstring dir(buf, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    return wideToUtf8(dir.c_str());
#elif defined(__linux__)
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return exe.parent_path().string();
#else
    return {};
#endif
}

void saveInstallPreference(const std::string& dataDir) {
    const std::string path = iniPath();
    if (path.empty()) return;
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    out << "dataDir=" << dataDir << "\n";
}

LocatedInstall locateSkyrimData(const std::string& explicitDir, bool allowPicker) {
    const auto tryDir = [](const std::string& dir, const char* source) -> LocatedInstall {
        if (dir.empty()) return {};
        const std::string normalized = normalizeDataDir(dir);
        if (looksLikeDataDir(normalized)) return LocatedInstall{normalized, source, true};
        return {};
    };

    if (const LocatedInstall hit = tryDir(explicitDir, "command line"); hit.ok) return hit;
    if (const LocatedInstall hit = tryDir(readSavedPreference(), "saved setting"); hit.ok) return hit;

    if (const char* env = std::getenv("SKYRIM_DATA_DIR")) {
        if (const LocatedInstall hit = tryDir(env, "SKYRIM_DATA_DIR"); hit.ok) return hit;
    }
    if (const char* env = std::getenv("SKYRIM_DIR")) {
        if (const LocatedInstall hit = tryDir(env, "SKYRIM_DIR"); hit.ok) return hit;
    }

    const std::string exeDir = executableDir();
    if (!exeDir.empty()) {
        if (const LocatedInstall hit = tryDir(exeDir, "program folder"); hit.ok) return hit;
        if (const LocatedInstall hit = tryDir((fs::path(exeDir) / "Data").string(), "program folder");
            hit.ok) {
            return hit;
        }
    }

#ifdef _WIN32
    const std::string steam = steamPath();
    if (!steam.empty()) {
        for (const std::string& lib : steamLibraries(steam)) {
            for (const char* game : {"Skyrim", "Skyrim Special Edition"}) {
                const std::string dir =
                    (fs::path(lib) / "steamapps" / "common" / game / "Data").string();
                if (const LocatedInstall hit = tryDir(dir, "Steam"); hit.ok) return hit;
            }
        }
    }
#endif

    if (allowPicker) {
#ifdef _WIN32
        if (const LocatedInstall hit = tryDir(pickDataFolder(), "folder picker"); hit.ok) {
            return hit;
        }
#endif
    }

    return {};
}

}  // namespace skyrim::host
