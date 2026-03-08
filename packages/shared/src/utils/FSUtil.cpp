#include "utils/FSUtil.hpp"
#include <algorithm>

namespace firmius::shared {

std::string FSUtil::resolvePath(const std::string& path, const std::string& baseDir) {
    if (path.empty()) return baseDir;
    
    std::filesystem::path fspath(path);
    if (fspath.is_relative()) {
        fspath = std::filesystem::path(baseDir) / fspath;
    }
    
    // Use lexical normalization to avoid host-side filesystem access
    // and potential permission/ABI issues with weakly_canonical.
    fspath = fspath.lexically_normal();
    
    return fspath.string();
}

bool FSUtil::isSubpath(const std::string& path, const std::string& allowedRoot) {
    std::string normPath = path;
    std::string pattern = allowedRoot;
    
    if (pattern.size() >= 3 && pattern.substr(pattern.size() - 3) == "/**") {
        std::string prefix = pattern.substr(0, pattern.size() - 2); 
        return normPath.compare(0, prefix.size(), prefix) == 0;
    }

    std::string normRoot = allowedRoot;
    
    // Ensure trailing slash for root comparison
    if (!normRoot.empty() && normRoot.back() != std::filesystem::path::preferred_separator) {
        normRoot += std::filesystem::path::preferred_separator;
    }
    
    return normPath.compare(0, normRoot.size(), normRoot) == 0 || normPath == allowedRoot;
}

}
