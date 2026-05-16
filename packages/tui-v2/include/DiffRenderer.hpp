#pragma once

#include <string>
#include <vector>

namespace firmius::tui2 {

/// Renders unified diff text into ANSI-formatted terminal lines.
class DiffRenderer {
public:
  /// Parse a unified diff string and render it into colored terminal lines.
  /// Supports gap collapsing (>5 context lines between hunks become "... N lines omitted ...").
  static std::vector<std::string> render(const std::string& diffPreview, int width);
};

} // namespace firmius::tui2
