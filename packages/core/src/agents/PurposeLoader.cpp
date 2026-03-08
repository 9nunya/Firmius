#include "agents/PurposeLoader.hpp"
#include "utils/StringUtil.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <cstdlib>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @file PurposeLoader.cpp
 * @brief Implementation of agent persona loading and prompt composition.
 */

namespace {
/**
 * @brief Maps a string scope identifier to the ToolScope enum.
 */
firmius::shared::ToolScope stringToScope(const std::string& s) {
    using firmius::shared::ToolScope;
    if (s == "fs:read") return ToolScope::FilesystemRead;
    if (s == "fs:write") return ToolScope::FilesystemWrite;
    if (s == "process:exec") return ToolScope::Process;
    if (s == "semantic") return ToolScope::Semantic;
    if (s == "delegation") return ToolScope::Delegation;
    if (s == "web") return ToolScope::Web;
    if (s == "git") return ToolScope::Git;
    throw std::runtime_error("Unknown scope: " + s);
}
}

std::map<std::string, std::string> PurposeLoader::customPlaceholders;

void PurposeLoader::registerPlaceholder(const std::string& key, const std::string& value) {
    customPlaceholders[key] = value;
}

Persona PurposeLoader::load(const std::string& purpose) {
    std::string promptsDir = resolvePromptsDir();
    std::string path = promptsDir + purpose + ".md";
    std::ifstream file(path);
    if (!file.is_open()) {
        std::vector<std::string> purposes;
        if (std::filesystem::exists(promptsDir)) {
            for (const auto& entry : std::filesystem::directory_iterator(promptsDir)) {
                if (entry.path().extension() == ".md" && 
                    entry.path().stem() != "base" && 
                    entry.path().stem() != "COMPACTION_PROMPT") {
                    purposes.push_back(entry.path().stem().string());
                }
            }
        }
        std::string purposeList;
        for (size_t i = 0; i < purposes.size(); ++i) {
            purposeList += "'" + purposes[i] + "'";
            if (i < purposes.size() - 1) purposeList += ", ";
        }
        throw std::runtime_error("Could not load persona '" + purpose + "'. Available purposes are: " + purposeList);
    }

    std::string line;
    std::string frontmatter;
    std::string body;
    bool inFrontmatter = false;
    int dashCount = 0;

    while (std::getline(file, line)) {
        if (line == "---") {
            dashCount++;
            if (dashCount == 1) inFrontmatter = true;
            else if (dashCount == 2) inFrontmatter = false;
            continue;
        }

        if (inFrontmatter) frontmatter += line + "\n";
        else body += line + "\n";
    }

    Persona persona;
    persona.identityPrompt = StringUtil::trim(body);

    std::stringstream ss_fm(frontmatter);
    while (std::getline(ss_fm, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        
        std::string key = StringUtil::trim(line.substr(0, colon));
        std::string value = StringUtil::trim(line.substr(colon + 1));

        if (key == "name") persona.name = value;
        else if (key == "title") persona.title = value;
        else if (key == "description") persona.description = value;
        else if (key == "canSpawn") persona.canSpawn = (value == "true");
        else if (key == "scopes") {
            if (value.front() == '[' && value.back() == ']') {
                std::string list = value.substr(1, value.size() - 2);
                auto parts = StringUtil::split(list, ',');
                for (auto& p : parts) {
                    std::string cleaned = p;
                    if (cleaned.front() == '"' && cleaned.back() == '"') cleaned = cleaned.substr(1, cleaned.size() - 2);
                    persona.allowedScopes.push_back(stringToScope(cleaned));
                }
            }
        } else if (key == "stop") {
            if (value.front() == '[' && value.back() == ']') {
                std::string list = value.substr(1, value.size() - 2);
                auto parts = StringUtil::split(list, ',');
                for (auto& p : parts) {
                    std::string cleaned = p;
                    if (cleaned.front() == '"' && cleaned.back() == '"') cleaned = cleaned.substr(1, cleaned.size() - 2);
                    persona.stopSequences.push_back(cleaned);
                }
            }
        }
    }

    return persona;
}

std::string PurposeLoader::composeSystemPrompt(const Persona& persona, const AgentContext& context, const std::string& toolsBlock) {
    std::string basePrompt;
    std::ifstream baseFile(resolvePromptsDir() + "base.md");
    if (baseFile.is_open()) {
        std::stringstream buffer;
        buffer << baseFile.rdbuf();
        basePrompt = buffer.str();
    }

    // Dynamic placeholders
    std::map<std::string, std::string> placeholders;
    placeholders["{{AGENT_NAME}}"] = persona.name;
    placeholders["{{AGENT_TITLE}}"] = persona.title;
    placeholders["{{CWD}}"] = context.environment.cwd;

    std::string promptsDir = resolvePromptsDir();
    std::vector<std::string> purposes;
    if (std::filesystem::exists(promptsDir)) {
        for (const auto& entry : std::filesystem::directory_iterator(promptsDir)) {
            if (entry.path().extension() == ".md" && 
                entry.path().stem() != "base" && 
                entry.path().stem() != "COMPACTION_PROMPT") {
                purposes.push_back(entry.path().stem().string());
            }
        }
    }
    std::string purposeList;
    std::sort(purposes.begin(), purposes.end());
    purposes.erase(std::unique(purposes.begin(), purposes.end()), purposes.end());
    for (size_t i = 0; i < purposes.size(); ++i) {
        purposeList += purposes[i];
        if (i < purposes.size() - 1) purposeList += ", ";
    }
    placeholders["{{REGISTERED_PURPOSES}}"] = purposeList;

    // Custom placeholders
    for (const auto& [key, value] : customPlaceholders) {
        placeholders[key] = value;
    }

    for (const auto& [key, value] : placeholders) {
        size_t pos = 0;
        while ((pos = basePrompt.find(key, pos)) != std::string::npos) {
            basePrompt.replace(pos, key.length(), value);
            pos += value.length();
        }
    }

    std::stringstream ss;
    ss << basePrompt << "\n\n";
    ss << "# AGENT IDENTITY\n" << persona.identityPrompt << "\n\n";
    
    ss << "# ENVIRONMENT\n";
    ss << "Host: " << context.environment.identifier << "\n";
    ss << "CWD: " << context.environment.cwd << "\n\n";

    if (!toolsBlock.empty()) {
        ss << "# AVAILABLE TOOLS\n" << toolsBlock << "\n";
    }

    return ss.str();
}

std::string PurposeLoader::buildToolsBlock(const std::vector<firmius::provider::ToolDefinition>& tools) {
    std::stringstream ss;
    for (const auto& t : tools) {
        ss << "- " << t.name << ": " << t.description << "\n";
        ss << "  Args: " << t.inputSchema << "\n";
    }
    return ss.str();
}

std::string PurposeLoader::loadCompactionPrompt() {
    std::ifstream file(resolvePromptsDir() + "COMPACTION_PROMPT.md");
    if (!file.is_open()) return "You must summarize the session. Preserve critical state.";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string PurposeLoader::resolvePromptsDir() {
    const char* envDir = std::getenv("FIRMIUS_PROMPTS_DIR");
    if (envDir && std::filesystem::exists(envDir)) {
        std::string dir = envDir;
        if (dir.back() != '/') dir += '/';
        return dir;
    }

    const char* home = std::getenv("HOME");
    if (home) {
        std::string userDir = std::string(home) + "/.firmius/prompts/";
        if (std::filesystem::exists(userDir)) {
            return userDir;
        }
    }

    return "prompts/";
}

void PurposeLoader::bootstrapDefaults(const std::string& builtinPromptsDir) {
    const char* home = std::getenv("HOME");
    if (!home) return;

    std::string userDir = std::string(home) + "/.firmius/prompts";
    if (std::filesystem::exists(userDir)) return;

    if (!std::filesystem::exists(builtinPromptsDir)) return;

    std::filesystem::create_directories(userDir);
    for (const auto& entry : std::filesystem::directory_iterator(builtinPromptsDir)) {
        if (entry.is_regular_file()) {
            std::filesystem::copy_file(
                entry.path(),
                std::string(userDir) + "/" + entry.path().filename().string(),
                std::filesystem::copy_options::skip_existing
            );
        }
    }
}

}
