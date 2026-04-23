#include "EnvLoader.hpp"
#include "utils/Logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace firmius::shared {

namespace {
void setEnvironmentVariable(const std::string& key, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(key.c_str(), value.c_str());
#else
    setenv(key.c_str(), value.c_str(), 1);
#endif
}
} // namespace

void EnvLoader::load(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        Logger::instance().logWarning("[EnvLoader] Warning: Could not open " + filePath);
        return;
    }
    Logger::instance().logInfo("[EnvLoader] Loading environment from " + filePath);

    std::string line;
    while (std::getline(file, line)) {
        auto commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }

        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty()) {
            continue;
        }

        auto equalPos = line.find('=');
        if (equalPos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, equalPos);
        std::string value = line.substr(equalPos + 1);

        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));

        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        } else if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
            value = value.substr(1, value.size() - 2);
        }

        setEnvironmentVariable(key, value);
    }
}

std::string EnvLoader::get(const std::string& key, const std::string& defaultValue) {
    const char* val = std::getenv(key.c_str());
    return val ? std::string(val) : defaultValue;
}

} // namespace firmius::shared
