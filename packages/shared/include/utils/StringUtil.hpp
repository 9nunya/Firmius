#ifndef FIRMIUS_SHARED_STRING_UTIL_HPP
#define FIRMIUS_SHARED_STRING_UTIL_HPP

#include <string>
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

    /**
     * @brief Splits a string by a delimiter.
     * @param s The string to split.
     * @param delimiter The delimiter character.
     * @return A vector of tokens.
     */
    static std::vector<std::string> split(const std::string& s, char delimiter);

    /**
     * @brief Checks if a string starts with a prefix.
     * @param s The string to check.
     * @param prefix The prefix.
     * @return True if s starts with prefix.
     */
    static bool startsWith(const std::string& s, const std::string& prefix);

    /**
     * @brief Checks if a string ends with a suffix.
     * @param s The string to check.
     * @param suffix The suffix.
     * @return True if s ends with suffix.
     */
    static bool endsWith(const std::string& s, const std::string& suffix);

    /**
     * @brief Converts a string to lowercase.
     * @param s The string to convert.
     * @return The lowercase string.
     */
    static std::string toLower(const std::string& s);
};

}

#endif
