#pragma once

#include <string>
#include <vector>

namespace firmius::tui2 {

/// Renders unified diff text into ANSI-formatted terminal lines.
///
/// Two header dialects are supported:
///   • Real unified-diff hunks:   `@@ -312,5 +315,7 @@`
///   • Firmius anchor headers:    `@@ replace_range "foo" -> "bar" @@`
///
/// When an anchor header is encountered, line numbers are omitted (we don't
/// have the real numbers); when a real hunk header is present, line numbers
/// are taken from it and counted forward through the hunk body.
///
/// The returned strings are full-width: every diff body row is padded with
/// spaces to `width` so the background color reaches the right edge of the
/// screen. Tokens inside each row are syntax-highlighted using the
/// SyntaxHighlighter, with `sourcePath` used to detect the language. When
/// `sourcePath` is empty or has no registered grammar, content is rendered
/// in plain foreground only — no syntax colors.
class DiffRenderer {
public:
  static std::vector<std::string> render(const std::string& diffPreview,
                                         int width,
                                         const std::string& sourcePath = "");
};

} // namespace firmius::tui2
