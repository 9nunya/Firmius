#ifndef FIRMIUS_CORE_LSP_SERVER_SPEC_HPP
#define FIRMIUS_CORE_LSP_SERVER_SPEC_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>

namespace firmius::core {

struct LspServerSpec {
    std::string id;                                            // "python", "clangd", etc.
    std::vector<std::string> extensions;                       // file extensions handled (with dot)
    std::vector<std::string> markers;                          // project root markers
    std::vector<std::vector<std::string>> commands;            // ordered fallback command chains
    std::unordered_map<std::string, std::string> languageIds;  // extension → LSP languageId
    std::string defaultLanguageId;                             // fallback language ID
    bool isCustom = false;

    // Get languageId for a file path based on extension
    std::string languageIdForPath(const std::string& path) const {
        auto ext = std::filesystem::path(path).extension().string();
        auto it = languageIds.find(ext);
        if (it != languageIds.end()) {
            return it->second;
        }
        return defaultLanguageId;
    }

    // Find first available command from fallback chain by searching PATH
    std::vector<std::string> resolveCommand() const {
        const char* pathEnv = std::getenv("PATH");
        if (!pathEnv) {
            return {};
        }

        std::vector<std::string> pathDirs;
        std::string pathStr(pathEnv);
        std::string::size_type start = 0;
        std::string::size_type pos = 0;
        while ((pos = pathStr.find(':', start)) != std::string::npos) {
            pathDirs.push_back(pathStr.substr(start, pos - start));
            start = pos + 1;
        }
        pathDirs.push_back(pathStr.substr(start));

        for (const auto& cmd : commands) {
            if (cmd.empty()) continue;
            const auto& executable = cmd[0];

            // If executable contains a slash, check directly
            if (executable.find('/') != std::string::npos) {
                if (access(executable.c_str(), X_OK) == 0) {
                    return cmd;
                }
                continue;
            }

            // Search PATH directories
            for (const auto& dir : pathDirs) {
                if (dir.empty()) continue;
                std::string candidate = dir + "/" + executable;
                if (access(candidate.c_str(), X_OK) == 0) {
                    return cmd;
                }
            }
        }

        return {};
    }
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_LSP_SERVER_SPEC_HPP
