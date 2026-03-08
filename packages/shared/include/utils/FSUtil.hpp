#ifndef FIRMIUS_SHARED_FS_UTIL_HPP
#define FIRMIUS_SHARED_FS_UTIL_HPP

#include <string>
#include <filesystem>

namespace firmius::shared {

/**
 * @brief Utilities for filesystem operations and path resolution.
 */
class FSUtil {
public:
    /**
     * @brief Resolves a path relative to a base directory and normalizes it.
     * @param path The path to resolve (can be absolute or relative).
     * @param baseDir The base directory (CWD).
     * @return The absolute, normalized path.
     */
    static std::string resolvePath(const std::string& path, const std::string& baseDir);

    /**
     * @brief Checks if a path is within an allowed root directory.
     * Supports basic wildcard prefix matching (e.g. "/tmp/" followed by double-asterisk).
     * @param path The absolute path to check.
     * @param allowedRoot The allowed root directory or wildcard pattern.
     * @return True if path is within allowedRoot.
     */
    static bool isSubpath(const std::string& path, const std::string& allowedRoot);
};

}

#endif
