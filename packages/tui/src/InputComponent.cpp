#include "InputComponent.hpp"
#include <algorithm>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

InputComponent::InputComponent(InputComponentOptions options)
    : options_(std::move(options)), content_("") {}

ftxui::Element InputComponent::Render() {
    if (options_.isDisabled) {
        // Requirement 5: Disabled state when viewing subagent: show "Viewing: [Name] — Esc to return".
        return ftxui::hbox({
            ftxui::text("Viewing: [" + options_.subagentName + "] — Esc to return") |
            ftxui::color(ftxui::Color::GrayDark)
        });
    }

    auto lines = getLines();
    int totalLines = std::max(1, (int)lines.size());
    
    // Requirement 3: Auto-grow up to 10 lines then scroll internally.
    int displayLinesCount = std::min(totalLines, 10);
    
    // Calculate cursor line and column for rendering.
    int cursorLine = 0;
    int cursorCol = 0;
    if (content_.empty()) {
        cursorLine = 0;
        cursorCol = 0;
    } else {
        for (int i = 0; i < (int)lines.size(); ++i) {
            int lineEnd = (i == (int)lines.size() - 1) ? (int)content_.size() : lines[i+1].startOffset - 1;
            if (cursor_position_ >= lines[i].startOffset && cursor_position_ <= lineEnd) {
                cursorLine = i;
                cursorCol = cursor_position_ - lines[i].startOffset;
                break;
            }
        }
    }

    // Scroll if cursor goes beyond the 10-line window.
    int startLine = 0;
    if (totalLines > 10) {
        startLine = std::max(0, std::min(totalLines - 10, cursorLine - 5));
    }

    ftxui::Elements lineElements;
    auto now = std::chrono::steady_clock::now();

    for (int i = startLine; i < startLine + displayLinesCount && i < (int)lines.size(); ++i) {
        // Requirement 6: Prompt "> " in blue bold on the first line.
        ftxui::Element prompt = (i == 0) ? ftxui::text("> ") | ftxui::bold | ftxui::color(ftxui::Color::Blue) : ftxui::text("  ");
        
        std::string lineContent = lines[i].content;
        
        // Handle cursor rendering within the line.
        if (i == cursorLine && Focused()) {
            std::string before = (cursorCol <= (int)lineContent.size()) ? lineContent.substr(0, cursorCol) : lineContent;
            std::string at = (cursorCol < (int)lineContent.size()) ? lineContent.substr(cursorCol, 1) : " ";
            std::string after = (cursorCol < (int)lineContent.size() - 1) ? lineContent.substr(cursorCol + 1) : "";
            
            lineElements.push_back(ftxui::hbox({
                prompt,
                ftxui::text(before),
                ftxui::text(at) | ftxui::inverted | ftxui::focus,
                ftxui::text(after)
            }));
        } else {
            lineElements.push_back(ftxui::hbox({
                prompt,
                ftxui::text(lineContent)
            }));
        }
    }

    // For empty content, still show the prompt and cursor.
    if (content_.empty()) {
        lineElements.clear();
        lineElements.push_back(ftxui::hbox({
            ftxui::text("> ") | ftxui::bold | ftxui::color(ftxui::Color::Blue),
            ftxui::text(" ") | ftxui::inverted | ftxui::focus
        }));
    }

    // Show paste feedback for a few seconds.
    ftxui::Elements feedbackElements;
    activeFeedbacks_.erase(
        std::remove_if(activeFeedbacks_.begin(), activeFeedbacks_.end(),
            [&now](const PasteFeedback& f) {
                return std::chrono::duration_cast<std::chrono::seconds>(now - f.startTime).count() > 3;
            }),
        activeFeedbacks_.end());

    for (const auto& feedback : activeFeedbacks_) {
        feedbackElements.push_back(ftxui::text(feedback.label) | ftxui::color(ftxui::Color::GreenLight));
    }

    if (!feedbackElements.empty()) {
        lineElements.push_back(ftxui::hbox(std::move(feedbackElements)));
    }

    ftxui::Element inputElement = ftxui::vbox(std::move(lineElements));

    if (options_.renderHelper) {
        return ftxui::vbox({
            options_.renderHelper(content_),
            inputElement
        });
    }

    return inputElement;
}

bool InputComponent::OnEvent(ftxui::Event event) {
    if (options_.isDisabled) {
        if (event == ftxui::Event::Escape) {
            if (options_.onInterrupt) options_.onInterrupt();
            return true;
        }
        return false;
    }

    // Requirement 1: Enter = send (calls onSubmit).
    if (event == ftxui::Event::Return) {
        if (options_.onSubmit) {
            options_.onSubmit(content_);
            content_ = "";
            cursor_position_ = 0;
        }
        return true;
    }

    // Requirement 1: Shift+Enter = newline.
    // Some terminals send \r or \n for Shift+Enter while \r for Enter, or vice versa.
    if (event.is_character() && event.character() == "\n") {
        insertText("\n");
        return true;
    }
    
    // Requirement 2: Esc = interrupt (calls onInterrupt).
    if (event == ftxui::Event::Escape) {
        if (options_.onInterrupt) options_.onInterrupt();
        return true;
    }

    if (event == ftxui::Event::Special("\x16")) { // Ctrl+V
        return false; // Let characters fall through or implement paste later
    }

    return handleKeyEvent(event);
}

void InputComponent::SetContent(const std::string& content) {
    content_ = content;
    cursor_position_ = (int)content_.size();
}

bool InputComponent::handleKeyEvent(ftxui::Event event) {
    if (event.is_character()) {
        insertText(event.character());
        return true;
    }

    if (event == ftxui::Event::Backspace) {
        backspace();
        return true;
    }

    if (event == ftxui::Event::Delete) {
        del();
        return true;
    }

    if (event == ftxui::Event::ArrowLeft) {
        moveCursorLeft();
        return true;
    }

    if (event == ftxui::Event::ArrowRight) {
        moveCursorRight();
        return true;
    }

    if (event == ftxui::Event::ArrowUp) {
        moveCursorUp();
        return true;
    }

    if (event == ftxui::Event::ArrowDown) {
        moveCursorDown();
        return true;
    }

    return false;
}

void InputComponent::insertText(const std::string& text) {
    // Basic multi-line paste detection for character input streams.
    int lineCount = 0;
    for (char c : text) if (c == '\n') lineCount++;
    
    if (lineCount > 0 && text.size() > 1) {
        handlePaste(text);
        return;
    }

    content_.insert(cursor_position_, text);
    cursor_position_ += (int)text.size();
}

void InputComponent::handlePaste(const std::string& text) {
    // Requirement 4: Ctrl+V / Paste detection.
    
    // Check if it's an image (placeholder detection for vision support).
    if (options_.isVisionSupported && options_.isVisionSupported()) {
        if (text.find("data:image") == 0 || text.find("[IMAGE]") != std::string::npos) {
            imageCount_++;
            insertText("[Image " + std::to_string(imageCount_) + "]");
            return;
        }
    }

    // Requirement 4: If text with newlines -> insert [Pasted: +N lines] visual.
    int lineCount = 0;
    for (char c : text) if (c == '\n') lineCount++;

    if (lineCount > 0) {
        activeFeedbacks_.push_back({
            "[Pasted: +" + std::to_string(lineCount + 1) + " lines]",
            std::chrono::steady_clock::now()
        });
    }

    content_.insert(cursor_position_, text);
    cursor_position_ += (int)text.size();
}

void InputComponent::backspace() {
    if (cursor_position_ > 0) {
        content_.erase(cursor_position_ - 1, 1);
        cursor_position_--;
    }
}

void InputComponent::del() {
    if (cursor_position_ < (int)content_.size()) {
        content_.erase(cursor_position_, 1);
    }
}

void InputComponent::moveCursorLeft() {
    if (cursor_position_ > 0) cursor_position_--;
}

void InputComponent::moveCursorRight() {
    if (cursor_position_ < (int)content_.size()) cursor_position_++;
}

void InputComponent::moveCursorUp() {
    auto lines = getLines();
    int currentLine = 0;
    int currentCol = 0;
    for (int i = 0; i < (int)lines.size(); ++i) {
        int lineEnd = (i == (int)lines.size() - 1) ? (int)content_.size() : lines[i+1].startOffset - 1;
        if (cursor_position_ >= lines[i].startOffset && cursor_position_ <= lineEnd) {
            currentLine = i;
            currentCol = cursor_position_ - lines[i].startOffset;
            break;
        }
    }

    if (currentLine > 0) {
        int targetCol = std::min(currentCol, (int)lines[currentLine-1].content.size());
        cursor_position_ = lines[currentLine-1].startOffset + targetCol;
    }
}

void InputComponent::moveCursorDown() {
    auto lines = getLines();
    int currentLine = 0;
    int currentCol = 0;
    for (int i = 0; i < (int)lines.size(); ++i) {
        int lineEnd = (i == (int)lines.size() - 1) ? (int)content_.size() : lines[i+1].startOffset - 1;
        if (cursor_position_ >= lines[i].startOffset && cursor_position_ <= lineEnd) {
            currentLine = i;
            currentCol = cursor_position_ - lines[i].startOffset;
            break;
        }
    }

    if (currentLine < (int)lines.size() - 1) {
        int targetCol = std::min(currentCol, (int)lines[currentLine+1].content.size());
        cursor_position_ = lines[currentLine+1].startOffset + targetCol;
    }
}

std::vector<InputComponent::LineInfo> InputComponent::getLines() const {
    std::vector<LineInfo> lines;
    size_t start = 0;
    size_t end = content_.find('\n');
    while (end != std::string::npos) {
        lines.push_back({content_.substr(start, end - start), (int)start});
        start = end + 1;
        end = content_.find('\n', start);
    }
    lines.push_back({content_.substr(start), (int)start});
    return lines;
}

ftxui::Component MakeInputComponent(InputComponentOptions options) {
    return ftxui::Make<InputComponent>(std::move(options));
}

} // namespace firmius::tui

