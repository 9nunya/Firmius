#ifndef FIRMIUS_CORE_ARTIFACTS_REFERENCE_EXPANSION_HPP
#define FIRMIUS_CORE_ARTIFACTS_REFERENCE_EXPANSION_HPP

#include <string>

namespace firmius::core::artifacts {

/**
 * @brief Expands @artifact and @file references into structured XML snippets.
 *
 * Supported forms:
 * - @artifact:friendly-name/REPORT.md
 * - @artifact:REPORT.md (must be unambiguous within thread artifacts)
 * - @path/to/file.ts
 * - @path/to/file.ts:370-500
 *
 * Throws std::runtime_error for recognized references that are ambiguous,
 * missing, invalid, or fail owner resolution. Non-reference `@...` tokens are
 * left untouched.
 */
std::string expandInboundReferences(const std::string &threadId,
                                    const std::string &cwd,
                                    const std::string &text);

} // namespace firmius::core::artifacts

#endif
