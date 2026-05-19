#ifndef FIRMIUS_TUI_SYNTAXHIGHLIGHTER_HPP
#define FIRMIUS_TUI_SYNTAXHIGHLIGHTER_HPP

#include "ThemeManager.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Forward-declare opaque tree-sitter types so callers don't need the C
// headers. The implementation pulls them in.
struct TSLanguage;
typedef struct TSNode TSNode_t; // unused — kept to anchor the type
struct TSTree;

namespace firmius::tui {

/// Token classification — mapped onto theme.syntax.* at render time.
enum class HighlightKind {
  Plain,
  Keyword,
  Type,
  Function,
  Variable,
  String,
  Comment,
  Number,
  Operator,
  Punctuation,
  Constant,
  Tag,
  Attribute,
};

struct GrammarInfo {
  std::string name;
  std::vector<std::string> fileExtensions;
  const ::TSLanguage* (*languageFn)();
};

/// Tree-sitter backed syntax highlighter producing ANSI-colored strings
/// suitable for the tui line-based terminal renderer.
///
/// Output strings carry foreground colors only — callers are responsible
/// for any background color so a diff renderer can wrap a whole row with
/// `bgRgb(...)` and still get colored tokens inside it.
class SyntaxHighlighter {
public:
  static SyntaxHighlighter& instance();

  /// Register every compiled-in grammar. Idempotent.
  void initialize();

  /// True when a tree-sitter grammar is registered for `language`.
  bool hasGrammar(const std::string& language) const;

  /// Available language keys (e.g. "cpp", "python", "bash").
  std::vector<std::string> availableLanguages() const;

  /// Best-effort language detection from a filename or extension.
  /// Returns "" when no grammar matches.
  std::string detectLanguage(const std::string& filename) const;

  /// Render a code snippet (single line or multi-line) as a vector of
  /// ANSI-colored display lines. One result entry per source line.
  ///
  /// `fallbackFg` is used for Plain text and unmatched tokens. When the
  /// language has no grammar, every line is wrapped with `fallbackFg` only.
  std::vector<std::string> highlightLines(const std::string& code,
                                          const std::string& language,
                                          const ThemeRgb& fallbackFg) const;

  /// Render a single line with syntax highlighting. Caller guarantees no
  /// embedded newlines. Convenience wrapper around `highlightLines`.
  std::string highlightLine(const std::string& code,
                            const std::string& language,
                            const ThemeRgb& fallbackFg) const;

private:
  SyntaxHighlighter() = default;

  struct HighlightSpan {
    std::uint32_t startByte;
    std::uint32_t endByte;
    HighlightKind kind;
  };

  // Wrappers around tree-sitter's C API. These take a TSNode by value but
  // forward-declared structs can't be passed by value, so the helpers are
  // private and live in the .cpp file via templated indirection.
  void parseAndCollect(const std::string& code, const std::string& language,
                       std::vector<HighlightSpan>& spans) const;

  ThemeRgb colorFor(HighlightKind kind, const ThemeRgb& fallback) const;

  std::unordered_map<std::string, GrammarInfo> grammars_;
  bool initialized_ = false;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_SYNTAXHIGHLIGHTER_HPP
