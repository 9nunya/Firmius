#include "MarkdownParser.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include "utils/StringUtil.hpp"

namespace firmius::tui {

namespace {

using firmius::shared::StringUtil;

std::vector<std::string> splitLines(const std::string& input, std::string& remainder) {
    std::vector<std::string> lines;
    size_t pos = 0, last = 0;
    while ((pos = input.find('\n', last)) != std::string::npos) {
        lines.push_back(input.substr(last, pos - last));
        last = pos + 1;
    }
    remainder = input.substr(last);
    return lines;
}

bool isCodeFence(const std::string& line) {
    return StringUtil::startsWith(line, "```");
}

std::string extractFenceLang(const std::string& line) {
    if (line.size() > 3) {
        std::string rest = line.substr(3);
        size_t space = rest.find(' ');
        if (space != std::string::npos) return rest.substr(0, space);
        return rest;
    }
    return "";
}

} // namespace

std::vector<ftxui::Element> MarkdownParser::parseInline(const std::string& text) {
    std::vector<ftxui::Element> elements;
    size_t i = 0;
    std::string current;
    while (i < text.size()) {
        if (i + 1 < text.size() && text[i] == '*' && text[i+1] == '*') {
            if (!current.empty()) {
                elements.push_back(ftxui::text(current));
                current.clear();
            }
            size_t end_bold = text.find("**", i+2);
            if (end_bold == std::string::npos) {
                current += "**";
                i += 2;
            } else {
                elements.push_back(ftxui::bold(ftxui::text(text.substr(i+2, end_bold - (i+2)))));
                i = end_bold + 2;
            }
        } else if (text[i] == '`') {
            if (!current.empty()) {
                elements.push_back(ftxui::text(current));
                current.clear();
            }
            size_t end_code = text.find('`', i+1);
            if (end_code == std::string::npos) {
                current += '`';
                i++;
            } else {
                elements.push_back(ftxui::text(text.substr(i+1, end_code - (i+1))) | ftxui::color(ftxui::Color::GrayLight));
                i = end_code + 1;
            }
        } else {
            current += text[i];
            i++;
        }
    }
    if (!current.empty()) elements.push_back(ftxui::text(current));
    return elements;
}

MarkdownParser::MarkdownParser() : state_(State::TEXT), fence_lang_() { reset(); }

std::vector<ftxui::Element> MarkdownParser::parseChunk(const std::string& chunk) {
    std::vector<ftxui::Element> output;
    std::string cleaned = StringUtil::stripDoneTag(chunk);
    buffer_ += cleaned;
    std::string remainder;
    auto lines = splitLines(buffer_, remainder);
    buffer_ = remainder;
    for (const auto& line : lines) processLine(line, output);
    return output;
}

std::vector<ftxui::Element> MarkdownParser::finish() {
    std::vector<ftxui::Element> output;
    if (!buffer_.empty()) {
        processLine(buffer_, output);
        buffer_.clear();
    }
    if (state_ == State::PARAGRAPH) flushParagraph(output);
    else if (state_ == State::LIST) flushList(output);
    else if (state_ == State::CODE_FENCE) {
        if (!current_paragraph_.empty()) {
            output.push_back(ftxui::vbox(std::move(current_paragraph_)));
            current_paragraph_.clear();
        }
        state_ = State::TEXT;
    }
    reset();
    return output;
}

void MarkdownParser::reset() {
    state_ = State::TEXT;
    buffer_.clear();
    current_paragraph_.clear();
    fence_lang_.clear();
    current_list_.clear();
}

void MarkdownParser::processLine(const std::string& line, std::vector<ftxui::Element>& out) {
    if (line.empty() || line.find_first_not_of(" \t\r\n") == std::string::npos) {
        if (state_ == State::PARAGRAPH) flushParagraph(out);
        else if (state_ == State::LIST) flushList(out);
        else if (state_ == State::CODE_FENCE) {
            if (!current_paragraph_.empty()) current_paragraph_.push_back(ftxui::text("\n"));
        }
        return;
    }

    switch (state_) {
        case State::TEXT:
            if (isCodeFence(line)) {
                state_ = State::CODE_FENCE;
                fence_lang_ = extractFenceLang(line);
                current_paragraph_.clear();
            } else if (line.size() >= 2 && line[0] == '-' && line[1] == ' ') {
                state_ = State::LIST;
                current_list_.clear();
                auto inline_elems = parseInline(line.substr(2));
                std::vector<ftxui::Element> hbox_elems;
                hbox_elems.push_back(ftxui::text("- ") | ftxui::color(ftxui::Color::Cyan));
                hbox_elems.insert(hbox_elems.end(), inline_elems.begin(), inline_elems.end());
                current_list_.push_back(ftxui::hbox(hbox_elems));
            } else {
                state_ = State::PARAGRAPH;
                current_paragraph_ = parseInline(line);
            }
            break;

        case State::CODE_FENCE:
            if (isCodeFence(line)) {
                state_ = State::TEXT;
                if (!current_paragraph_.empty()) {
                    auto code_content = ftxui::vbox(std::move(current_paragraph_));
                    ftxui::Element fence = fence_lang_.empty()
                        ? ftxui::border(code_content)
                        : ftxui::window(ftxui::text(fence_lang_) | ftxui::color(ftxui::Color::RGB(80,80,100)), code_content) | ftxui::border;
                    out.push_back(std::move(fence));
                    current_paragraph_.clear();
                    fence_lang_.clear();
                }
            } else {
                current_paragraph_.push_back(ftxui::text(line + "\n"));
            }
            break;

        case State::PARAGRAPH:
            if (isCodeFence(line)) {
                flushParagraph(out);
                state_ = State::CODE_FENCE;
                fence_lang_ = extractFenceLang(line);
            } else if (line.size() >= 2 && line[0] == '-' && line[1] == ' ') {
                flushParagraph(out);
                state_ = State::LIST;
                current_list_.clear();
                auto inline_elems = parseInline(line.substr(2));
                std::vector<ftxui::Element> hbox_elems;
                hbox_elems.push_back(ftxui::text("- ") | ftxui::color(ftxui::Color::Cyan));
                hbox_elems.insert(hbox_elems.end(), inline_elems.begin(), inline_elems.end());
                current_list_.push_back(ftxui::hbox(hbox_elems));
            } else {
                current_paragraph_.push_back(ftxui::text("\n"));
                auto inline_elems = parseInline(line);
                current_paragraph_.insert(current_paragraph_.end(),
                    std::make_move_iterator(inline_elems.begin()),
                    std::make_move_iterator(inline_elems.end()));
            }
            break;

        case State::LIST:
            if (isCodeFence(line)) {
                flushList(out);
                state_ = State::CODE_FENCE;
                fence_lang_ = extractFenceLang(line);
            } else if (line.size() >= 2 && line[0] == '-' && line[1] == ' ') {
                flushList(out);
                state_ = State::LIST;
                current_list_.clear();
                auto inline_elems = parseInline(line.substr(2));
                std::vector<ftxui::Element> hbox_elems;
                hbox_elems.push_back(ftxui::text("- ") | ftxui::color(ftxui::Color::Cyan));
                hbox_elems.insert(hbox_elems.end(), inline_elems.begin(), inline_elems.end());
                current_list_.push_back(ftxui::hbox(hbox_elems));
            } else {
                if (!current_list_.empty()) current_list_.push_back(ftxui::text(" "));
                auto inline_elems = parseInline(line);
                current_list_.insert(current_list_.end(),
                    std::make_move_iterator(inline_elems.begin()),
                    std::make_move_iterator(inline_elems.end()));
            }
            break;

        default:
            state_ = State::TEXT;
            current_paragraph_ = parseInline(line);
            break;
    }
}

void MarkdownParser::flushParagraph(std::vector<ftxui::Element>& out) {
    if (!current_paragraph_.empty()) {
        out.push_back(ftxui::vbox(std::move(current_paragraph_)));
        current_paragraph_.clear();
    }
}

void MarkdownParser::flushList(std::vector<ftxui::Element>& out) {
    if (!current_list_.empty()) {
        out.push_back(ftxui::vbox(std::move(current_list_)));
        current_list_.clear();
    }
}

} // namespace firmius::tui
