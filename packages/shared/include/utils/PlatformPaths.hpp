#ifndef FIRMIUS_SHARED_PLATFORMPATHS_HPP
#define FIRMIUS_SHARED_PLATFORMPATHS_HPP

#include <filesystem>

namespace firmius::shared {

enum class Platform {
    Linux,
    MacOS,
    Windows,
    Unknown,
};

class PlatformPaths {
public:
    static Platform detectPlatform();
    static std::filesystem::path firmiusHomeDir();
    static std::filesystem::path firmiusDataDir();
    static std::filesystem::path firmiusTempDir();
    static std::filesystem::path userHomeDir();
    static const char* nullDevicePath();
};

} // namespace firmius::shared

#endif