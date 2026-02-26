#ifndef FIRMIUS_CORE_PURPOSE_LOADER_HPP
#define FIRMIUS_CORE_PURPOSE_LOADER_HPP

#include "Context.hpp"
#include "Enums.hpp"
#include "IProvider.hpp"
#include <string>
#include <vector>
#include <map>

namespace firmius::core {
using namespace firmius::shared;

using namespace firmius::shared;

struct Persona {
    std::string name;
    std::string title;
    std::string description;
    std::vector<ToolScope> allowedScopes;
    bool canSpawn = false;
    std::string identityPrompt;
};

class PurposeLoader {
public:
    static Persona load(const std::string& purpose);
    static std::string composeSystemPrompt(const Persona& persona, const AgentContext& context, const std::string& toolsBlock);
    static std::string buildToolsBlock(const std::vector<firmius::provider::ToolDefinition>& tools);
};

}

#endif
