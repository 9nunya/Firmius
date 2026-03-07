#ifndef FIRMIUS_TUI_COMPONENTS_TOOL_WINDOW_HPP
#define FIRMIUS_TUI_COMPONENTS_TOOL_WINDOW_HPP

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

namespace firmius::tui {

/// Renders a bordered tool window with content lines and a footer label.
/// The footer appears as: `── footer_label ── [action_label]`
/// action_label is optional; if empty, no action button text is shown.
ftxui::Element ToolWindow(const std::vector<ftxui::Element> &lines,
                          const std::string &footer_label,
                          const std::string &action_label = "");

/// Returns the last N lines from a multiline string.
std::vector<std::string> TailLines(const std::string &text, int maxLines);

/// Summarizes a tool call into a short ~3-word description.
/// e.g. file_read with {"path":"src/file.ts","start_line":200,"end_line":400}
///      → "Read src/file.ts[200:400]"
std::string SummarizeToolCall(const std::string &name, const std::string &args);

} // namespace firmius::tui

#endif
