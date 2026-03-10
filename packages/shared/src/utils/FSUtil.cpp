#include "utils/FSUtil.hpp"
#include <algorithm>

namespace firmius::shared {

std::string FSUtil::resolvePath(const std::string &path,
                                const std::string &baseDir) {
  if (path.empty())
    return baseDir;

  std::filesystem::path fspath(path);
  if (fspath.is_relative()) {
    fspath = std::filesystem::path(baseDir) / fspath;
  }

  // Use lexical normalization to avoid host-side filesystem access
  // and potential permission/ABI issues with weakly_canonical.
  fspath = fspath.lexically_normal();

  return fspath.string();
}

bool FSUtil::isSubpath(const std::string &path,
                       const std::string &allowedRoot) {
  if (allowedRoot.empty())
    return false;

  std::string normPath = path;
  std::string pattern = allowedRoot;

  // Normalize both for comparison
  if (normPath.size() > 1 &&
      normPath.back() == std::filesystem::path::preferred_separator) {
    normPath.pop_back();
  }

  if (pattern.size() >= 3 && pattern.substr(pattern.size() - 3) == "/**") {
    pattern = pattern.substr(0, pattern.size() - 3);
  }

  if (pattern.size() > 1 &&
      pattern.back() == std::filesystem::path::preferred_separator) {
    pattern.pop_back();
  }

  if (pattern.empty()) {
    // pattern was "/**", allow everything
    return true;
  }

  // Exact match (after normalization)
  if (normPath == pattern)
    return true;

  // Subpath check
  if (normPath.size() > pattern.size() &&
      normPath.compare(0, pattern.size(), pattern) == 0 &&
      normPath[pattern.size()] == std::filesystem::path::preferred_separator) {
    return true; // Any descendant is a valid subpath
  }

  return false;
}

} // namespace firmius::shared
