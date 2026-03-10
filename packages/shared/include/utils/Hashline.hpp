#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace firmius::shared::utils {

/**
 * @brief Utility for Hash-Anchored Edits (Hashline pattern).
 * 
 * Provides mechanisms to identify lines by "lineNumber#hash|content" to ensure
 * edits remain stable even when file content shifts.
 */
class Hashline {
public:
    /**
     * @brief Computes a 4-character mnemonic-style hash for the given content.
     * @param content The line content to hash.
     * @return A 4-character hex string representing the hash.
     */
    static std::string computeHash(std::string_view content) noexcept;

    /**
     * @brief Formats a line into the hashline pattern: "lineNum#hash|content".
     * @param lineNum The 1-indexed line number.
     * @param content The line content.
     * @return The formatted hashline string.
     */
    static std::string formatLine(int lineNum, std::string_view content);

    /**
     * @brief Verifies if the actual content matches the expected hash.
     * @param expectedHash The 4-character hash string.
     * @param actualContent The actual content of the line.
     * @return true if matches, false otherwise.
     */
    static bool verifyAnchor(std::string_view expectedHash, std::string_view actualContent) noexcept;
};

/**
 * @brief Enhances raw text with Hashline prefixes.
 */
class HashlineReadEnhancer {
public:
    /**
     * @brief Transforms file content into enhanced hashline format.
     * @param content The raw file content.
     * @return The enhanced string with line-by-line hashline prefixes.
     */
    static std::string enhance(std::string_view content);
};

} // namespace firmius::shared::utils
