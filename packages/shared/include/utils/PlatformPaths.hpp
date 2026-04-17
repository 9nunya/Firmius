#ifndef FIRMIUS_SHARED_UTILS_PLATFORM_PATHS_HPP
#define FIRMIUS_SHARED_UTILS_PLATFORM_PATHS_HPP

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
};

} // namespace firmius::shared

#endif