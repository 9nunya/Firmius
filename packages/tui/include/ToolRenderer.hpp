#pragma once

#include <string>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

/**
 * @brief Current state of a tool call for rendering.
 */
struct ToolCallState {
    std::string name;     ///< Tool name (e.g., "bash", "read_file")
    std::string args;     ///< JSON string of arguments
    std::string result;   ///< Tool output/result
    bool isError = false;   ///< Whether the tool failed
    bool isAborted = false; ///< Whether the tool was aborted/interrupted
};

/**
 * @brief Renders agent tool calls with pretty formatting.
 */
class ToolRenderer {
public:
    /**
     * @brief Dispatch to specific tool renderer based on state.name.
     */
    static ftxui::Element render(const ToolCallState& state);

private:
    static ftxui::Element renderSearchReplace(const std::string& args, const std::string& result);
    static ftxui::Element renderWriteFile(const std::string& args);
    static ftxui::Element renderBash(const std::string& args, const std::string& result, bool isError);
    static ftxui::Element renderReadFile(const std::string& args, const std::string& result);
    static ftxui::Element renderGrepGlob(const std::string& name, const std::string& args, const std::string& result);
    static ftxui::Element renderSummonSubagent(const std::string& args, const std::string& result);
    static ftxui::Element renderSubagentWait(const std::string& args);
    static ftxui::Element renderError(const std::string& name, const std::string& result);
    static ftxui::Element renderAborted(const std::string& name);
};

} // namespace firmius::tui
