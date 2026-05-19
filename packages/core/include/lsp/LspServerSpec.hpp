#ifndef FIRMIUS_CORE_LSPSERVERSPEC_HPP
#define FIRMIUS_CORE_LSPSERVERSPEC_HPP

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace firmius::core {

struct LspServerSpec {
    std::string id;                                            // "python", "clangd", etc.
    std::vector<std::string> extensions;                       // file extensions handled (with dot)
    std::vector<std::string> markers;                          // project root markers
    std::vector<std::vector<std::string>> commands;            // ordered fallback command chains
    std::unordered_map<std::string, std::string> languageIds;  // extension → LSP languageId
    std::string defaultLanguageId;                             // fallback language ID
    bool isCustom = false;

    std::string languageIdForPath(const std::string& path) const {
        auto ext = std::filesystem::path(path).extension().string();
        auto it = languageIds.find(ext);
        if (it != languageIds.end()) {
            return it->second;
        }
        return defaultLanguageId;
    }

    static bool isExecutablePath(const std::filesystem::path& path) {
        std::error_code ec;
        const auto status = std::filesystem::status(path, ec);
        if (ec || !std::filesystem::exists(status) || std::filesystem::is_directory(status)) {
            return false;
        }

#if defined(_WIN32)
        return true;
#else
        const auto perms = status.permissions();
        using perms_t = std::filesystem::perms;
        return (perms & perms_t::owner_exec) != perms_t::none ||
               (perms & perms_t::group_exec) != perms_t::none ||
               (perms & perms_t::others_exec) != perms_t::none;
#endif
    }

    std::vector<std::string> resolveCommand() const {
        const char* pathEnv = std::getenv("PATH");
        std::vector<std::string> pathDirs;
        if (pathEnv != nullptr) {
            const char separator =
#if defined(_WIN32)
                ';';
#else
                ':';
#endif
            std::string pathStr(pathEnv);
            std::string::size_type start = 0;
            std::string::size_type pos = 0;
            while ((pos = pathStr.find(separator, start)) != std::string::npos) {
                pathDirs.push_back(pathStr.substr(start, pos - start));
                start = pos + 1;
            }
            pathDirs.push_back(pathStr.substr(start));
        }

        for (const auto& cmd : commands) {
            if (cmd.empty()) continue;
            const std::filesystem::path executable(cmd[0]);

            if (executable.has_parent_path() || executable.is_absolute()) {
                if (isExecutablePath(executable)) {
                    return cmd;
                }
                continue;
            }

            for (const auto& dir : pathDirs) {
                if (dir.empty()) continue;
                if (isExecutablePath(std::filesystem::path(dir) / executable)) {
                    return cmd;
                }
            }
        }

        return {};
    }
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_LSPSERVERSPEC_HPP
