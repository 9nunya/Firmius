#ifndef FIRMIUS_CORE_ENV_LOADER_HPP
#define FIRMIUS_CORE_ENV_LOADER_HPP

#include <string>

/**
 * @brief Utilities for loading environment variables.
 */
namespace firmius::shared {

/**
 * @brief Parses and loads environment variables from a file.
 */
class EnvLoader {
public:
    /**
     * @brief Loads and parses a .env file.
     * @param filePath Absolute path to the .env file.
     */
    static void load(const std::string& filePath);

    /**
     * @brief Safely retrieves an environment variable.
     * @param key The variable name.
     * @param defaultValue Fallback value if not set.
     * @return The variable value or defaultValue.
     */
    static std::string get(const std::string& key, const std::string& defaultValue = "");
};

}

#endif
