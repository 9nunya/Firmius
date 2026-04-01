#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace firmius::shared::utils {

struct AnchorResult {
  enum class Status { SUCCESS, STALE, AMBIGUOUS, MALFORMED, NOT_NUMERIC, OUT_OF_RANGE };
  Status status = Status::SUCCESS;
  int lineIndex = -1;
  bool relocated = false;
  std::string errorMessage;
  std::string foundHash;
  std::string expectedHash;
};

struct HashlineAnchor {
    int lineNumber = 0;
    std::string hash;
};

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
     * @brief Computes unique per-line hashes for a full file buffer.
     * @details Uses surrounding context to disambiguate repeated line content
     *          and falls back to a line-specific suffix only when the entire
     *          file context is still identical.
     * @param lines The file lines.
     * @return One hash per input line.
     */
    static std::vector<std::string> computeLineHashes(
        const std::vector<std::string>& lines) noexcept;

    /**
     * @brief Formats an anchor without line content: "lineNum#hash".
     * @param lineNum The 1-indexed line number.
     * @param content The line content used to derive the hash.
     * @return The formatted anchor string.
     */
    static std::string formatAnchor(int lineNum, std::string_view content);
    static std::string formatAnchor(const std::vector<std::string>& lines,
                                    int lineNum);

    /**
     * @brief Formats a line into the hashline pattern: "lineNum#hash|content".
     * @param lineNum The 1-indexed line number.
     * @param content The line content.
     * @return The formatted hashline string.
     */
    static std::string formatLine(int lineNum, std::string_view content);
    static std::string formatLine(const std::vector<std::string>& lines,
                                  int lineNum);

    /**
     * @brief Parses an anchor in the form "lineNum#hash".
     * @details Also accepts the read-output form "lineNum#hash|content" by
     *          trimming everything after the first '|'.
     * @param anchor The anchor text.
     * @return Parsed anchor if valid.
     */
    static std::optional<HashlineAnchor> parseAnchor(std::string_view anchor) noexcept;

    /**
     * @brief Verifies if the actual content matches the expected hash.
     * @param expectedHash The 4-character hash string.
     * @param actualContent The actual content of the line.
     * @return true if matches, false otherwise.
     */
    static bool verifyAnchor(std::string_view expectedHash, std::string_view actualContent) noexcept;
    /**
     * @brief Resolves an anchor in a file buffer within a search window.
     * @param lines The lines of the file.
     * @param anchorText The lineNumber#hash anchor.
     * @param searchWindow The window size to search around the expected line.
     * @return The resolved line index and whether it was relocated.
     * @throws std::runtime_error if the anchor is ambiguous or stale.
     */
    static AnchorResult resolveAnchor(const std::vector<std::string>& lines,
                                      const std::string& anchorText,
                                      int searchWindow = 15);
    /**
     * @brief Resolves a plain line number to a line index.
     * @param lines The lines of the file.
     * @param lineNum The 1-indexed line number.
     * @return The resolved line index and status.
     */
    static AnchorResult resolveLineNumber(const std::vector<std::string>& lines,
                                          int lineNum);

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

/**
 * @brief Removes hashline prefixes from text.
 * @details Strips the "lineNum#hash|" prefix from each line, returning clean content.
 *          Useful for displaying hashline-formatted text without the metadata.
 */
class HashlineTrimmer {
public:
    /**
     * @brief Trims hashline prefix from a single line.
     * @param line The hashline-formatted line (e.g., "1#f828|cmake_minimum_required...").
     * @return The clean content without the hashline prefix.
     */
    static std::string trimLine(std::string_view line);

    /**
     * @brief Result of a hashline sanitation pass.
     */
    struct SanitationResult {
        int hashlinePrefixesStripped = 0;
        int malformedHashFragmentsStripped = 0;
        int diffMarkersStripped = 0;
        int boundaryEchoesRemoved = 0;
        bool boundaryEchoRemoved = false;
        bool suspiciousContentFound = false;
        bool suspiciousContentRejected = false;
    };

    /**
     * @brief Trims hashline prefixes from all lines in text and sanitizes diff markers.
     * @param content Text with hashline-formatted lines.
     * @param outResult Optional output to store sanitation metadata.
     * @return The clean text with all hashline prefixes and diff markers removed.
     */
    static std::string sanitizeContent(std::string_view content, SanitationResult* outResult = nullptr);
    static std::string trimAll(std::string_view content);
    static std::string sanitizeDiffMarkers(std::string_view line);
    static bool startsWithSuspiciousMetadata(std::string_view line) noexcept;
    static bool startsWithSuspiciousDiffJunk(std::string_view line) noexcept;
};

} // namespace firmius::shared::utils
