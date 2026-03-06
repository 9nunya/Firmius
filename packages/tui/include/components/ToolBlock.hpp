#ifndef FIRMIUS_COMPONENTS_TOOL_BLOCK_HPP
#define FIRMIUS_COMPONENTS_TOOL_BLOCK_HPP

#include <ftxui/component/component_base.hpp>
#include <memory>
#include <string>

namespace firmius::tui {

enum class ToolPhase {
    Preparing,
    Called,
    Finished,
};

struct ToolCallView {
    std::string agentId;
    std::string toolCallId;
    std::string name;
    std::string args;
    std::string result;
    bool success = false;
    ToolPhase phase = ToolPhase::Preparing;
    bool show_result = false;
    std::string toggle_label = "show";
};

ftxui::Component ToolBlock(const std::shared_ptr<ToolCallView>& view);

}

#endif
