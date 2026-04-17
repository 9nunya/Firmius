#include "utils/PlatformPaths.hpp"

#include <cstdlib>

namespace firmius::shared {

Platform PlatformPaths::detectPlatform() {
#if defined(_WIN32)
    return Platform::Windows;
#elif defined(__APPLE__)
    return Platform::MacOS;
#elif defined(__linux__)
    return Platform::Linux;
#else
    return Platform::Unknown;
#endif
}

std::filesystem::path PlatformPaths::firmiusHomeDir() {
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA")) {
        return std::filesystem::path(appData) / "Firmius";
    }
    if (const char* userProfile = std::getenv("USERPROFILE")) {
        return std::filesystem::path(userProfile) / "AppData" / "Roaming" / "Firmius";
    }
#else
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".firmius";
    }
#endif

    return std::filesystem::current_path() / ".firmius";
}

std::filesystem::path PlatformPaths::firmiusDataDir() {
#if defined(_WIN32)
    return firmiusHomeDir() / "data";
#else
    if (const char* xdgDataHome = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::path(xdgDataHome) / "firmius";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".local" / "share" / "firmius";
    }
    return firmiusHomeDir() / "data";
#endif
}

std::filesystem::path PlatformPaths::firmiusTempDir() {
    return std::filesystem::temp_directory_path() / "firmius";
}

} // namespace firmius::shared