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
    /**
     * @brief Checks if a path (resolved via weakly_canonical) is within a root (resolved via canonical).
     * @param candidate The path to check.
     * @param root The root directory.
     * @return True if the canonicalized candidate is within the canonicalized root.
     */
    static bool isCanonicalSubpath(const std::filesystem::path& candidate, const std::filesystem::path& root);
};
}

#endif
