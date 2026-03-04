#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <memory>

#include <ftxui/dom/elements.hpp>
#include "ChatMessage.hpp"

namespace firmius::tui {

/**
 * @brief Renders the agent's history and streaming messages.
 */
class ChatRenderer {
public:
    ChatRenderer();
    ~ChatRenderer() = default;

    /**
     * @brief Render the chat history.
     */
    ftxui::Element render(const std::vector<ChatMessage>& history, 
                        int viewportWidth, 
                        int viewportHeight,
                        const std::string& focusedAgentId = "");

    /**
     * @brief Get the current scroll offset.
     */
    int getScrollOffset() const { return scrollOffset_; }

    /**
     * @brief Set the scroll offset.
     */
    void setScrollOffset(int offset) { scrollOffset_ = offset; autoFollow_ = false; }

    /**
     * @brief Reset the cache.
     */
    void clearCache() { cache_.clear(); }

private:
    std::unordered_map<std::string, ftxui::Element> cache_;
    
    int scrollOffset_ = 0;
    bool autoFollow_ = true;
    int lastContentHeight_ = 0;

    ftxui::Element renderMessage(const ChatMessage& msg, int width, bool isLatest, const std::unordered_map<std::string, const firmius::shared::ToolResultContent*>& toolResults);
    ftxui::Element renderNormalMessage(const ChatMessage& msg, int width, bool isLatest, const std::unordered_map<std::string, const firmius::shared::ToolResultContent*>& toolResults);
    ftxui::Element renderHeader(const ChatMessage& msg, int width);
    ftxui::Element renderContent(const ChatMessage& msg, int width, bool isLatest, const std::unordered_map<std::string, const firmius::shared::ToolResultContent*>& toolResults);
    ftxui::Element renderThinking(const std::string& thinking, bool isLatest, bool isExpanded);
    ftxui::Element renderSpecialBlock(const ChatMessage& msg, int width);
};

} // namespace firmius::tui
