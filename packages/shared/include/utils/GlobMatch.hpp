#ifndef FIRMIUS_SHARED_GLOB_MATCH_HPP
#define FIRMIUS_SHARED_GLOB_MATCH_HPP

#include <regex>
#include <string>

namespace firmius::shared::utils {

/// Compile a glob pattern (`*`, `**`, `?`, `[abc]`, `{a,b,c}`) into a regex
/// anchored to start/end. The double-star (`**`) crosses path separators;
/// single-star (`*`) does not. Use this for matching paths and URLs.
std::regex compileGlobRegex(const std::string &pattern);

/// Convenience wrapper that compiles + matches in one shot.
/// Returns true iff `value` fully matches `pattern`.
bool globMatches(const std::string &pattern, const std::string &value);

} // namespace firmius::shared::utils

#endif // FIRMIUS_SHARED_GLOB_MATCH_HPP
