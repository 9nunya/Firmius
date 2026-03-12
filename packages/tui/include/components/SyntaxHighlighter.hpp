#ifndef FIRMIUS_TUI_SYNTAX_HIGHLIGHTER_HPP
#define FIRMIUS_TUI_SYNTAX_HIGHLIGHTER_HPP

#include <ftxui/dom/elements.hpp>
#include <string>
#include <unordered_map>
#include <vector>

// tree-sitter C API
extern "C" {
#include <tree_sitter/api.h>
}

// Forward-declare each parser's language function (defined in their parser.c)
extern "C" {
const TSLanguage *tree_sitter_c(void);
const TSLanguage *tree_sitter_cpp(void);
const TSLanguage *tree_sitter_java(void);
const TSLanguage *tree_sitter_rust(void);
const TSLanguage *tree_sitter_python(void);
const TSLanguage *tree_sitter_javascript(void);
const TSLanguage *tree_sitter_typescript(void);
const TSLanguage *tree_sitter_json(void);
const TSLanguage *tree_sitter_yaml(void);
const TSLanguage *tree_sitter_toml(void);
const TSLanguage *tree_sitter_cmake(void);
const TSLanguage *tree_sitter_lua(void);
const TSLanguage *tree_sitter_luau(void);
const TSLanguage *tree_sitter_markdown(void);
}

namespace firmius::tui {

// Color scheme for syntax highlighting
struct SyntaxColorScheme {
  ftxui::Color keyword = ftxui::Color::RGB(200, 120, 255);
  ftxui::Color type = ftxui::Color::RGB(80, 180, 220);
  ftxui::Color function = ftxui::Color::RGB(100, 200, 150);
  ftxui::Color variable = ftxui::Color::RGB(220, 180, 120);
  ftxui::Color string = ftxui::Color::RGB(230, 150, 120);
  ftxui::Color comment = ftxui::Color::RGB(100, 120, 140);
  ftxui::Color number = ftxui::Color::RGB(255, 180, 100);
  ftxui::Color operator_color = ftxui::Color::RGB(180, 180, 200);
  ftxui::Color punctuation = ftxui::Color::RGB(150, 150, 170);
  ftxui::Color constant = ftxui::Color::RGB(100, 200, 200);
  ftxui::Color tag = ftxui::Color::RGB(220, 100, 150);
  ftxui::Color attribute = ftxui::Color::RGB(180, 150, 200);
  ftxui::Color plain = ftxui::Color::RGB(200, 200, 220);
};

// Grammar metadata (all grammars are compiled in, no downloads needed)
struct GrammarInfo {
  std::string name;
  std::vector<std::string> fileExtensions;
  const TSLanguage *(*languageFn)(); // Function pointer to get TSLanguage
};

// Highlight category for a node
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

class SyntaxHighlighter {
public:
  static SyntaxHighlighter &instance();

  // Initialize — registers all compiled-in grammars
  void initialize();

  // All grammars are always available (compiled in)
  bool hasGrammar(const std::string &language) const;

  // Get grammar info
  const GrammarInfo *getGrammarInfo(const std::string &language) const;

  // Get all available language names
  std::vector<std::string> getAvailableLanguages() const;

  // Detect language from filename extension
  std::string detectLanguage(const std::string &filename) const;

  // Highlight code — returns the source as-is (use highlightRender for UI)
  std::string highlight(const std::string &code,
                        const std::string &language) const;

  // Highlight and render to a vector of FTXUI elements (one per line)
  std::vector<ftxui::Element>
  highlightRenderLines(const std::string &code,
                       const std::string &language) const;

  // Highlight and render to a single FTXUI element (framed vbox)
  ftxui::Element highlightRender(const std::string &code,
                                 const std::string &language,
                                 bool showLineNumbers = true) const;

  // Color scheme accessors
  const SyntaxColorScheme &getColorScheme() const { return colorScheme_; }
  void setColorScheme(const SyntaxColorScheme &scheme) {
    colorScheme_ = scheme;
  }

private:
  SyntaxHighlighter() = default;

  // Classify a tree-sitter node into a highlight category
  HighlightKind classifyNode(TSNode node, const std::string &language) const;

  // Get FTXUI color for a highlight kind
  ftxui::Color colorFor(HighlightKind kind) const;

  // Collect leaf-node highlight spans from the AST
  struct HighlightSpan {
    uint32_t startByte;
    uint32_t endByte;
    HighlightKind kind;
  };
  void collectSpans(TSNode node, const std::string &language,
                    std::vector<HighlightSpan> &spans) const;

  std::unordered_map<std::string, GrammarInfo> grammars_;
  SyntaxColorScheme colorScheme_;
  mutable bool initialized_ = false;
};

} // namespace firmius::tui

#endif
