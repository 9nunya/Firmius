#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

namespace firmius::tui {

/**
 * @brief Streaming-aware Markdown parser for FTXUI.
 *
 * Handles partial tokens and maintains state across chunks.
 * Supports: **bold**, `code`, ```fence``` (with language label), - list, paragraphs.
 */
class MarkdownParser {
public:
    MarkdownParser();
    ~MarkdownParser() = default;

    /**
     * @brief Parse a chunk of markdown text.
     *
     * Strips <done /> tags, accumulates deltas, and returns any
     * complete Elements that can be rendered. Incomplete tokens
     * are kept in internal state for the next chunk.
     *
     * @param chunk Input text chunk (may be partial)
     * @return Vector of renderable Elements
     */
    std::vector<ftxui::Element> parseChunk(const std::string& chunk);

    /**
     * @brief Finish parsing and flush remaining buffered content.
     *
     * Closes any open constructs (unclosed fences, bold, etc.)
     * and returns final Elements. Resets parser state.
     *
     * @return Remaining Elements
     */
    std::vector<ftxui::Element> finish();

    /**
     * @brief Reset parser to initial state.
     *
     * Clears all buffers and state. Useful for reusing the parser.
     */
    void reset();

private:
    // Internal parsing state
    enum class State {
        TEXT,           ///< Normal text, not in any construct
        CODE_FENCE,     ///< Inside ```...```
        PARAGRAPH,      ///< Building a paragraph (multi-line)
        LIST,           ///< Building a list (with items)
    };

    // Current state
    State state_;

    // Buffer for incomplete input across chunks
    std::string buffer_;

    // Temporary storage for current paragraph being built
    std::vector<ftxui::Element> current_paragraph_;

    // For code fences: language label (if any)
    std::string fence_lang_;

    // For list items
    std::vector<ftxui::Element> current_list_;

    // Private methods
    void flushParagraph(std::vector<ftxui::Element>& out);
    void flushList(std::vector<ftxui::Element>& out);
    void processLine(const std::string& line, std::vector<ftxui::Element>& out);
    std::vector<ftxui::Element> parseInline(const std::string& text);
};

} // namespace firmius::tui
