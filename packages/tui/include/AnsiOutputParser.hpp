#ifndef FIRMIUS_TUI_ANSIOUTPUTPARSER_HPP
#define FIRMIUS_TUI_ANSIOUTPUTPARSER_HPP

#include <string>
#include <vector>

namespace firmius::tui {

/// Parses raw ANSI output from processes into display lines.
class AnsiOutputParser {
public:
  /// Split ANSI-aware text into display lines, prefixing each with a dimmed bar.
  /// Handles wrapping at width and optional max-lines truncation.
  /// When maxLines > 0 and output exceeds it, shows the LAST maxLines lines.
  static std::vector<std::string> toLines(const std::string& raw, int width, int maxLines = -1);
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_ANSIOUTPUTPARSER_HPP
