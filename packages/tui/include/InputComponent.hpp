#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <functional>
#include <vector>
#include <chrono>

namespace firmius::tui {

/**
 * @brief Configuration for the InputComponent.
 */
struct InputComponentOptions {
    /// Callback triggered when Enter is pressed (without Shift).
    std::function<void(std::string)> onSubmit;
    /// Callback triggered when Esc is pressed.
    std::function<void()> onInterrupt;
    /// Callback to check if vision/image support is active.
    std::function<bool()> isVisionSupported;
    std::function<ftxui::Element(std::string)> renderHelper;

    /// If true, the input is disabled and shows a special message.
    bool isDisabled = false;
    /// The name of the subagent being viewed (if any).
    std::string subagentName = "";
};

/**
 * @brief A multi-line input component for FTXUI.
 * 
 * Supports:
 * - Shift+Enter for newlines.
 * - Enter to submit.
 * - Esc to interrupt.
 * - Auto-grow up to 10 lines, then scrolls.
 * - Ctrl+V detection for text and pseudo-images.
 * - Prompt in blue bold.
 */
class InputComponent : public ftxui::ComponentBase {
public:
    explicit InputComponent(InputComponentOptions options);

    ftxui::Element Render() override;
    bool OnEvent(ftxui::Event event) override;
    bool Focusable() const override { return !options_.isDisabled; }

    /// Get the current content of the input.
    std::string GetContent() const { return content_; }
    /// Set the current content and move cursor to end.
    void SetContent(const std::string& content);

private:
    bool handleKeyEvent(ftxui::Event event);
    void insertText(const std::string& text);
    void handlePaste(const std::string& text);
    void backspace();
    void del();
    void moveCursorLeft();
    void moveCursorRight();
    void moveCursorUp();
    void moveCursorDown();

    struct LineInfo {
        std::string content;
        int startOffset;
    };
    std::vector<LineInfo> getLines() const;

    InputComponentOptions options_;
    std::string content_;
    int cursor_position_ = 0;

    struct PasteFeedback {
        std::string label;
        std::chrono::steady_clock::time_point startTime;
    };
    std::vector<PasteFeedback> activeFeedbacks_;
    int imageCount_ = 0;
};

/**
 * @brief Factory function for creating an InputComponent.
 */
ftxui::Component MakeInputComponent(InputComponentOptions options);

} // namespace firmius::tui
