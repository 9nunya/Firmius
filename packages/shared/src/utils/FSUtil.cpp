#include "utils/FSUtil.hpp"
#include <algorithm>

namespace firmius::shared {

std::string FSUtil::resolvePath(const std::string& path, const std::string& baseDir) {
    if (path.empty()) return baseDir;
    
    std::filesystem::path fspath(path);
    if (fspath.is_relative()) {
        fspath = std::filesystem::path(baseDir) / fspath;
    }
    
    try {
        fspath = std::filesystem::weakly_canonical(fspath);
    } catch (...) {
        // Fallback to basic normalization if filesystem check fails
        fspath = fspath.lexically_normal();
    }
    
    return fspath.string();
}

bool FSUtil::isSubpath(const std::string& path, const std::string& allowedRoot) {
    std::string normPath = path;
    std::string normRoot = allowedRoot;
    
    // Ensure trailing slash for root comparison
    if (!normRoot.empty() && normRoot.back() != std::filesystem::path::preferred_separator) {
        normRoot += std::filesystem::path::preferred_separator;
    }
    
    return normPath.compare(0, normRoot.size(), normRoot) == 0 || normPath == allowedRoot;
}

}
