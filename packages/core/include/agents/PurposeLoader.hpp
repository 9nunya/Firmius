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

/**
 * @brief Representation of an agent persona loaded from Markdown/YAML.
 */
struct Persona {
    std::string name;                   ///< Machine name of the persona.
    std::string title;                  ///< Display title.
    std::string description;            ///< High-level description.
    std::vector<ToolScope> allowedScopes; ///< Tools the persona is allowed to use.
    bool canSpawn = false;              ///< Whether this persona can spawn sub-agents.
    std::string identityPrompt;         ///< The core instructions for the persona.
};

/**
 * @brief Loader and composer for agent personas.
 */
class PurposeLoader {
public:
    /**
     * @brief Loads a persona from the prompts/ directory.
     * @param purpose The name of the persona file (without .md).
     * @return The loaded Persona struct.
     */
    static Persona load(const std::string& purpose);

    /**
     * @brief Composes the final system prompt for an agent.
     */
    static std::string composeSystemPrompt(const Persona& persona, const AgentContext& context, const std::string& toolsBlock);

    /**
     * @brief Formats a list of tool definitions into a Markdown block.
     */
    static std::string buildToolsBlock(const std::vector<firmius::provider::ToolDefinition>& tools);

    /**
     * @brief Loads the compaction prompt from the prompts/ directory.
     */
    static std::string loadCompactionPrompt();
};

}

#endif
