#ifndef FIRMIUS_SHARED_STRING_UTIL_HPP
#define FIRMIUS_SHARED_STRING_UTIL_HPP

#include <string>
#include <string_view>
#include <vector>

namespace firmius::shared {

/**
 * @brief Utilities for string manipulation.
 */
class StringUtil {
public:
    /**
     * @brief Trims whitespace from both ends of a string.
     * @param s The string to trim.
     * @return The trimmed string.
     */
    static std::string trim(const std::string& s);
    static std::string trim(std::string_view s);

    /**
     * @brief Splits a string by a delimiter.
     * @param s The string to split.
     * @param delimiter The delimiter character.
     * @return A vector of tokens.
     */
    static std::vector<std::string> split(const std::string& s, char delimiter);

    /**
     * @brief Concatenates a string using a specified string.
     * @param arr The array of strings to concatenate.
     * @param str The string to use when concatenating.
     * @return The concatenated string.
     */
    static std::string concat(const std::vector<std::string> &arr, const std::string &str);

    /**
     * @brief Checks if a string starts with a prefix.
     * @param s The string to check.
     * @param prefix The prefix.
     * @return True if s starts with prefix.
     */
    static bool startsWith(std::string_view s, std::string_view prefix);

    /**
     * @brief Checks if a string ends with a suffix.
     * @param s The string to check.
     * @param suffix The suffix.
     * @return True if s ends with suffix.
     */
    static bool endsWith(std::string_view s, std::string_view suffix);

    /**
     * @brief Converts a string to lowercase.
     * @param s The string to convert.
     * @return The lowercase string.
     */
    static std::string toLower(const std::string& s);

    /**
     * @brief Generates a random UUID.
     * @return A UUID string.
     */
    static std::string generateUuid();

    /**
     * @brief Escapes a string for safe use as a POSIX shell argument.
     * Wraps in single quotes, escaping embedded single quotes.
     * @param s The raw string.
     * @return A shell-safe quoted string.
     */
    static std::string shellEscape(std::string_view s);

    /**
     * @brief Calculates the Levenshtein distance between two strings.
     */
    static size_t levenshteinDistance(std::string_view s1, std::string_view s2);

    /**
     * @brief Performs a fuzzy search using a sliding window and Levenshtein distance.
     * @param text The text to search in.
     * @param pattern The pattern to look for.
     * @param threshold Similarity threshold (0.0 to 1.0).
     * @return Vector of start indices of matches.
     */
    static std::vector<size_t> findFuzzy(std::string_view text, std::string_view pattern, float threshold);

    /**
     * @brief Basic HTML to Markdown converter.
     */
    static std::string htmlToMarkdown(const std::string& html);
};

}

#endif
