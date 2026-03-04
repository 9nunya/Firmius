#include "ChatRenderer.hpp"
#include "Colors.hpp"
#include "MarkdownParser.hpp"
#include "ToolRenderer.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/dom/node.hpp>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace firmius::tui {

using namespace ftxui;
namespace c = firmius::tui::colors;

ChatRenderer::ChatRenderer() {
    autoFollow_ = true;
}

Element ChatRenderer::render(const std::vector<ChatMessage>& history, 
                           int viewportWidth, 
                           int,
                           const std::string& focusedAgentId) {
    Elements elements;
    
    std::vector<ChatMessage> filtered;
    if (!focusedAgentId.empty()) {
        for (const auto& msg : history) {
            if (msg.agentId == focusedAgentId || msg.agentId.empty()) {
                filtered.push_back(msg);
            }
        }
    } else {
        filtered = history;
    }

    if (filtered.empty()) {
        return filler();
    }

    std::unordered_map<std::string, const firmius::shared::ToolResultContent*> toolResults;
    for (const auto& msg : filtered) {
        for (const auto& part : msg.message.content) {
            if (auto* p = std::get_if<firmius::shared::ToolResultContent>(&part)) {
                toolResults[p->toolCallId] = p;
            }
        }
    }

    for (size_t i = 0; i < filtered.size(); ++i) {
        const auto& msg = filtered[i];
        
        if (msg.message.role == firmius::shared::Role::ToolResult) {
            continue; 
        }

        bool isLatest = (i == filtered.size() - 1);
        
        if (!isLatest && !msg.message.id.empty() && cache_.count(msg.message.id)) {
            elements.push_back(cache_[msg.message.id]);
        } else {
            Element rendered = renderMessage(msg, viewportWidth, isLatest, toolResults);
            if (!isLatest && !msg.message.id.empty()) {
                cache_[msg.message.id] = rendered;
            }
            
            if (isLatest && autoFollow_) {
                rendered = rendered | focus;
            }
            
            elements.push_back(rendered);
        }
    }

    auto content = vbox(std::move(elements));
    return content | vscroll_indicator | frame | yflex;
}

Element ChatRenderer::renderMessage(const ChatMessage& msg, int width, bool isLatest, const std::unordered_map<std::string, const firmius::shared::ToolResultContent*>& toolResults) {
    (void)width;
    if (msg.type != ChatMessageType::Normal) {
        return renderSpecialBlock(msg, width);
    }
    return renderNormalMessage(msg, width, isLatest, toolResults);
}

Element ChatRenderer::renderNormalMessage(const ChatMessage& msg, int width, bool isLatest, const std::unordered_map<std::string, const firmius::shared::ToolResultContent*>& toolResults) {
    (void)width;
    auto header = renderHeader(msg, width);
    auto content = renderContent(msg, width, isLatest, toolResults);
    
    return vbox({
        header,
        content | border,
        separator() | dim
    });
}

Element ChatRenderer::renderHeader(const ChatMessage& msg, int width) {
    (void)width;
    std::string name = "firmius";
    Color roleColor = Color::Cyan;

    if (msg.message.role == firmius::shared::Role::User) {
        name = "user";
        roleColor = Color::Green;
    } else if (msg.message.role == firmius::shared::Role::Assistant) {
        if (!msg.friendlyName.empty()) {
            name = msg.friendlyName;
        }
        roleColor = Color::Cyan;
    } else if (msg.message.role == firmius::shared::Role::System) {
        name = "system";
        roleColor = Color::Yellow;
    }

    Elements headerParts;
    headerParts.push_back(text(name) | bold | color(roleColor));
    
    if (msg.type == ChatMessageType::Queued) {
        headerParts.push_back(text(" [QUEUED]") | dim | color(c::queuedTag()));
    }

    headerParts.push_back(filler());

    if (msg.metrics.has_value()) {
        std::stringstream ss;
        ss << msg.metrics->tokens.total << " tokens";
        headerParts.push_back(text(ss.str()) | dim);
    }

    return hbox(std::move(headerParts));
}

Element ChatRenderer::renderContent(const ChatMessage& msg, int width, bool isLatest, const std::unordered_map<std::string, const firmius::shared::ToolResultContent*>& toolResults) {
    (void)width;
    (void)isLatest;
    Elements parts;
    MarkdownParser parser;

    for (const auto& part : msg.message.content) {
        std::visit([&](auto&& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, firmius::shared::TextContent>) {
                auto elements = parser.parseChunk(p.text);
                for (auto& e : elements) {
                    parts.push_back(e);
                }
            } else if constexpr (std::is_same_v<T, firmius::shared::ThinkingContent>) {
                parts.push_back(renderThinking(p.thinking, isLatest, isLatest));
            } else if constexpr (std::is_same_v<T, firmius::shared::ToolCallContent>) {
                ToolCallState state;
                state.name = p.name;
                state.args = p.args;
                
                auto it = toolResults.find(p.id);
                if (it != toolResults.end()) {
                    state.result = it->second->result;
                    state.isError = !it->second->success;
                }
                
                parts.push_back(ToolRenderer::render(state));
            } else if constexpr (std::is_same_v<T, firmius::shared::ToolResultContent>) {
                parts.push_back(text("Result: " + p.result) | dim);
            }
        }, part);
    }

    auto finalElements = parser.finish();
    for (auto& e : finalElements) {
        parts.push_back(e);
    }

    return vbox(std::move(parts));
}

Element ChatRenderer::renderThinking(const std::string& thinking, bool, bool isExpanded) {
    if (thinking.empty()) return emptyElement();

    if (!isExpanded) {
        return text("> Thinking...") | dim | color(c::thinking());
    }

    return vbox({
        text("THINKING") | bold | dim | color(c::thinking()),
        paragraph(thinking) | color(c::thinking()) | dim
    }) | borderLight | dim;
}

Element ChatRenderer::renderSpecialBlock(const ChatMessage& msg, int width) {
    (void)width;
    switch (msg.type) {
        case ChatMessageType::Compaction: {
            std::string label = " Context compacted: saved " + std::to_string(msg.tokensSaved) + " tokens ";
            return hbox({
                text(label) | color(c::compaction()) | border,
                filler()
            });
        }
        case ChatMessageType::Retry: {
            std::stringstream ss;
            ss << " Retrying... (attempt " << msg.attempt << "/" << msg.maxAttempts << ")";
            if (msg.delayMs > 0) {
                ss << " in " << msg.delayMs << "ms";
            }
            ss << " ";
            return hbox({
                text(ss.str()) | color(c::retry()) | border,
                filler()
            });
        }
        case ChatMessageType::Error: {
            return vbox({
                text(" ERROR ") | bold | color(Color::Red),
                paragraph(msg.specialText) | color(Color::Red)
            }) | border | color(Color::Red);
        }
        default:
            return emptyElement();
    }
}

} // namespace firmius::tui
