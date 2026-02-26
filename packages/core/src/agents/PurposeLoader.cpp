#include "agents/PurposeLoader.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

namespace firmius::core {
using namespace firmius::shared;

namespace {
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

std::string trim(const std::string& s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(trim(token));
    }
    return tokens;
}
}

Persona PurposeLoader::load(const std::string& purpose) {
    std::string path = "prompts/" + purpose + ".md";
    std::ifstream file(path);
    if (!file.is_open()) throw std::runtime_error("Could not load persona: " + path);

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
    persona.identityPrompt = trim(body);

    std::stringstream ss_fm(frontmatter);
    while (std::getline(ss_fm, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        
        std::string key = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));

        if (key == "name") persona.name = value;
        else if (key == "title") persona.title = value;
        else if (key == "description") persona.description = value;
        else if (key == "canSpawn") persona.canSpawn = (value == "true");
        else if (key == "scopes") {
            if (value.front() == '[' && value.back() == ']') {
                std::string list = value.substr(1, value.size() - 2);
                auto parts = split(list, ',');
                for (auto& p : parts) {
                    if (p.front() == '"' && p.back() == '"') p = p.substr(1, p.size() - 2);
                    persona.allowedScopes.push_back(stringToScope(p));
                }
            }
        }
    }

    return persona;
}

std::string PurposeLoader::composeSystemPrompt(const Persona& persona, const AgentContext& context, const std::string& toolsBlock) {
    std::string basePrompt;
    std::ifstream baseFile("prompts/base.md");
    if (baseFile.is_open()) {
        std::stringstream buffer;
        buffer << baseFile.rdbuf();
        basePrompt = buffer.str();
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

}
