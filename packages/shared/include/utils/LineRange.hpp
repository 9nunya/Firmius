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
};

/**
 * @brief Utility for Line-Range Edits.
 * 
 * Provides mechanisms to identify lines by "lineNumber|content" to ensure
 * edits remain stable.
 */
class LineRange {
public:
    /**
     * @brief Formats an anchor: "lineNum".
     * @param lineNum The 1-indexed line number.
     * @return The formatted anchor string.
     */
    static std::string formatAnchor(int lineNum);

    /**
     * @brief Formats a line into the line-range pattern: "lineNum|content".
     * @param lineNum The 1-indexed line number.
     * @param content The line content.
     * @return The formatted line string.
     */
    static std::string formatLine(int lineNum, std::string_view content);

    /**
     * @brief Resolves an anchor in a file buffer.
     * @param lines The lines of the file.
     * @param anchorText The line number anchor. Must be a plain decimal line
     *        number, without hashline prefixes or trailing |content.
     * @return The resolved line index and status.
     */
    static AnchorResult resolveAnchor(const std::vector<std::string>& lines,
                                      const std::string& anchorText);

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
 * @brief Enhances raw text with LineRange prefixes.
 */
class LineRangeReadEnhancer {
public:
    /**
     * @brief Transforms file content into enhanced line-range format.
     * @param content The raw file content.
     * @return The enhanced string with line-by-line prefixes.
     */
    static std::string enhance(std::string_view content);
};

/**
 * @brief Removes line-range prefixes from text.
 * @details Strips the "lineNum|" prefix from each line, returning clean content.
 */
class LineRangeTrimmer {
public:
    /**
     * @brief Trims line-range prefix from a single line.
     * @param line The line-range formatted line (e.g., "1|cmake_minimum_required...").
     * @return The clean content without the prefix.
     */
    static std::string trimLine(std::string_view line);

    /**
     * @brief Result of a line-range sanitation pass.
     */
    struct SanitationResult {
        int lineRangePrefixesStripped = 0;
        int hashlinePrefixesStripped = 0;
        int malformedHashFragmentsStripped = 0;
        int diffMarkersStripped = 0;
        int boundaryEchoesRemoved = 0;
        bool boundaryEchoRemoved = false;
        bool suspiciousContentFound = false;
        bool suspiciousContentRejected = false;
    };

    /**
     * @brief Trims line-range prefixes from all lines in text and sanitizes diff markers.
     * @param content Text with line-range formatted lines.
     * @param outResult Optional output to store sanitation metadata.
     * @return The clean text with all prefixes and diff markers removed.
     */
    static std::string sanitizeContent(std::string_view content, SanitationResult* outResult = nullptr);
    static std::string trimAll(std::string_view content);
    static std::string sanitizeDiffMarkers(std::string_view line);
    static bool startsWithSuspiciousMetadata(std::string_view line) noexcept;
    static bool startsWithSuspiciousDiffJunk(std::string_view line) noexcept;
};

} // namespace firmius::shared::utils
