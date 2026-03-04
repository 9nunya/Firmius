#include "FooterBar.hpp"
#include "AppState.hpp"
#include "Colors.hpp"
#include <ftxui/dom/elements.hpp>
#include <iomanip>
#include <sstream>

namespace firmius::tui {

using namespace ftxui;

FooterBar::FooterBar(std::shared_ptr<AppState> state) : state_(std::move(state)) {}

Element FooterBar::Render() const {
    return hbox({
               text(renderLeftSection()) | flex,
               text(renderCenterSection()) | center | flex,
               text(renderRightSection()) | align_right | flex,
           }) |
           bgcolor(Color::GrayDark) | color(Color::GrayLight) | dim;
}

std::string FooterBar::renderLeftSection() const {
    auto info = state_->getFooterInfo();
    std::string thread = info.threadTitle.empty() ? info.threadId : info.threadTitle;
    if (thread.size() > 20) {
        thread = thread.substr(0, 17) + "...";
    }
    return " " + thread + " [" + statusToString(info.status) + "]";
}

std::string FooterBar::renderCenterSection() const {
    auto subs = state_->getSubagents();
    if (subs.empty()) {
        return "No active agent";
    }

    const auto& primary = subs.front();
    std::string modelInfo = primary.providerId + "/" + primary.modelId;
    if (primary.providerId.empty() || primary.modelId.empty()) {
        modelInfo = "Initializing...";
    }
    
    return modelInfo + " | " + primary.persona;
}

std::string FooterBar::renderRightSection() const {
    auto metricsMap = state_->getMetrics();
    uint32_t totalTokens = 0;
    uint32_t currentCtx = 0;
    
    for (const auto& [id, metrics] : metricsMap) {
        totalTokens += metrics.tokens.total;
        currentCtx = std::max(currentCtx, metrics.tokens.contextSize);
    }

    std::stringstream ss;
    ss << "ctx: " << currentCtx << " | total: " << totalTokens << " tk ";
    
    auto info = state_->getFooterInfo();
    if (info.isCompacting) {
        ss << "[COMPACTING] ";
    } else if (info.tokensSaved > 0) {
        ss << "(saved " << info.tokensSaved << ") ";
    }

    return ss.str();
}

std::string FooterBar::statusToString(::firmius::shared::AgentStatus status) const {
    switch (status) {
        case firmius::shared::AgentStatus::Idle: return "IDLE";
        case firmius::shared::AgentStatus::Streaming: return "STREAM";
        case firmius::shared::AgentStatus::ExecutingTool: return "TOOL";
        case firmius::shared::AgentStatus::Compacting: return "COMPACT";
        case firmius::shared::AgentStatus::Error: return "ERROR";
        case firmius::shared::AgentStatus::AwaitingInput: return "WAIT";
        case firmius::shared::AgentStatus::Cancelled: return "CANCEL";
    }
    return "UNKNOWN";
}

}
