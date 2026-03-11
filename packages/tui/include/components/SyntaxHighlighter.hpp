#ifndef FIRMIUS_TUI_SYNTAX_HIGHLIGHTER_HPP
#define FIRMIUS_TUI_SYNTAX_HIGHLIGHTER_HPP

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <filesystem>

namespace firmius::tui {

// Token types for syntax highlighting
enum class TokenType {
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
  Plain
};

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

// A highlighted token
struct HighlightedToken {
  std::string text;
  TokenType type = TokenType::Plain;
};

// Grammar metadata
struct GrammarInfo {
  std::string name;
  std::string version;
  std::vector<std::string> fileExtensions;
  std::string downloadUrl;
  bool downloaded = false;
  bool downloading = false;
};

class SyntaxHighlighter {
public:
  static SyntaxHighlighter& instance();
  
  // Initialize - loads cached grammars
  void initialize(const std::filesystem::path& cacheDir = 
    std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / ".firmius" / "grammars");
  
  // Download grammar in background
  void downloadGrammar(const std::string& language);
  
  // Check if grammar is available
  bool hasGrammar(const std::string& language) const;
  
  // Get grammar info
  const GrammarInfo* getGrammarInfo(const std::string& language) const;
  
  // Get all available grammars
  std::vector<std::string> getAvailableLanguages() const;
  
  // Detect language from filename
  std::string detectLanguage(const std::string& filename) const;
  
  // Highlight code
  std::vector<HighlightedToken> highlight(const std::string& code, 
                                           const std::string& language) const;
  
  // Highlight and render to FTXUI element
  ftxui::Element highlightRender(const std::string& code,
                                  const std::string& language,
                                  bool showLineNumbers = true) const;
  
  // Get color scheme
  const SyntaxColorScheme& getColorScheme() const { return colorScheme_; }
  
  // Set color scheme
  void setColorScheme(const SyntaxColorScheme& scheme) { colorScheme_ = scheme; }

private:
  SyntaxHighlighter() = default;
  
  // Simple pattern-based highlighter (fallback when tree-sitter not available)
  std::vector<HighlightedToken> highlightWithPatterns(const std::string& code,
                                                       const std::string& language) const;
  
  std::filesystem::path cacheDir_;
  std::unordered_map<std::string, GrammarInfo> grammars_;
  SyntaxColorScheme colorScheme_;
  mutable bool initialized_ = false;
};

// Helper to render highlighted tokens
ftxui::Element RenderHighlightedTokens(const std::vector<HighlightedToken>& tokens,
                                        const SyntaxColorScheme& colors);

} // namespace firmius::tui

#endif
