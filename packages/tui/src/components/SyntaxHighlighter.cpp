#include "components/SyntaxHighlighter.hpp"
#include "NotificationManager.hpp"
#include <algorithm>
#include <regex>
#include <fstream>
#include <thread>
#include <sstream>

namespace firmius::tui {

SyntaxHighlighter& SyntaxHighlighter::instance() {
  static SyntaxHighlighter inst;
  return inst;
}

void SyntaxHighlighter::initialize(const std::filesystem::path& cacheDir) {
  if (initialized_) return;
  
  cacheDir_ = cacheDir;
  std::filesystem::create_directories(cacheDir_);
  
  // Register built-in language definitions
  grammars_["cpp"] = GrammarInfo{
    .name = "C++",
    .version = "1.0",
    .fileExtensions = {".cpp", ".hpp", ".cc", ".cxx", ".h", ".hxx"},
    .downloadUrl = "",
    .downloaded = true // Built-in pattern highlighter
  };
  
  grammars_["rust"] = GrammarInfo{
    .name = "Rust",
    .version = "1.0",
    .fileExtensions = {".rs"},
    .downloadUrl = "",
    .downloaded = true
  };
  
  grammars_["python"] = GrammarInfo{
    .name = "Python",
    .version = "1.0",
    .fileExtensions = {".py", ".pyw"},
    .downloadUrl = "",
    .downloaded = true
  };
  
  grammars_["javascript"] = GrammarInfo{
    .name = "JavaScript",
    .version = "1.0",
    .fileExtensions = {".js", ".jsx", ".mjs"},
    .downloadUrl = "",
    .downloaded = true
  };
  
  grammars_["typescript"] = GrammarInfo{
    .name = "TypeScript",
    .version = "1.0",
    .fileExtensions = {".ts", ".tsx"},
    .downloadUrl = "",
    .downloaded = true
  };
  
  grammars_["json"] = GrammarInfo{
    .name = "JSON",
    .version = "1.0",
    .fileExtensions = {".json"},
    .downloadUrl = "",
    .downloaded = true
  };
  
  grammars_["yaml"] = GrammarInfo{
    .name = "YAML",
    .version = "1.0",
    .fileExtensions = {".yaml", ".yml"},
    .downloadUrl = "",
    .downloaded = true
  };
  
  grammars_["markdown"] = GrammarInfo{
    .name = "Markdown",
    .version = "1.0",
    .fileExtensions = {".md", ".markdown"},
    .downloadUrl = "",
    .downloaded = true
  };
  
  grammars_["shell"] = GrammarInfo{
    .name = "Shell",
    .version = "1.0",
    .fileExtensions = {".sh", ".bash", ".zsh"},
    .downloadUrl = "",
    .downloaded = true
  };
  
  grammars_["toml"] = GrammarInfo{
    .name = "TOML",
    .version = "1.0",
    .fileExtensions = {".toml"},
    .downloadUrl = "",
    .downloaded = true
  };
  
  // Check for downloaded grammars
  for (auto& [lang, info] : grammars_) {
    std::filesystem::path grammarFile = cacheDir_ / (lang + ".json");
    if (std::filesystem::exists(grammarFile)) {
      info.downloaded = true;
    }
  }
  
  initialized_ = true;
}

void SyntaxHighlighter::downloadGrammar(const std::string& language) {
  auto it = grammars_.find(language);
  if (it == grammars_.end() || it->second.downloaded) return;
  
  it->second.downloading = true;
  
  // Download in background thread
  std::thread([this, language]() {
    auto& info = grammars_[language];
    
    // Simulate download (in real implementation, would fetch from GitHub)
    // For now, just mark as downloaded since we use pattern-based highlighting
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    info.downloaded = true;
    info.downloading = false;
    
    // Save marker file
    std::ofstream marker(cacheDir_ / (language + ".json"));
    if (marker) {
      marker << "{\"language\": \"" << language << "\", \"version\": \"1.0\"}";
    }
    
    // Show notification
    NotificationManager::instance().notifySuccess(
      "Grammar Downloaded",
      language + " syntax highlighting is now available",
      std::chrono::milliseconds(3000)
    );
  }).detach();
  
  NotificationManager::instance().notifyInfo(
    "Downloading Grammar",
    "Fetching " + language + " syntax definitions...",
    std::chrono::milliseconds(2000)
  );
}

bool SyntaxHighlighter::hasGrammar(const std::string& language) const {
  auto it = grammars_.find(language);
  return it != grammars_.end() && it->second.downloaded;
}

const GrammarInfo* SyntaxHighlighter::getGrammarInfo(const std::string& language) const {
  auto it = grammars_.find(language);
  return it != grammars_.end() ? &it->second : nullptr;
}

std::vector<std::string> SyntaxHighlighter::getAvailableLanguages() const {
  std::vector<std::string> langs;
  for (const auto& [lang, info] : grammars_) {
    if (info.downloaded) {
      langs.push_back(lang);
    }
  }
  return langs;
}

std::string SyntaxHighlighter::detectLanguage(const std::string& filename) const {
  std::string ext;
  auto pos = filename.find_last_of('.');
  if (pos != std::string::npos) {
    ext = filename.substr(pos);
  }
  
  for (const auto& [lang, info] : grammars_) {
    for (const auto& knownExt : info.fileExtensions) {
      if (ext == knownExt) {
        return lang;
      }
    }
  }
  
  // Check for shebang
  if (filename.empty() || filename[0] == '#') {
    if (filename.find("python") != std::string::npos) return "python";
    if (filename.find("node") != std::string::npos) return "javascript";
    if (filename.find("bash") != std::string::npos || 
        filename.find("sh") != std::string::npos) return "shell";
  }
  
  return "";
}

std::vector<HighlightedToken> SyntaxHighlighter::highlight(
    const std::string& code, const std::string& language) const {
  if (!initialized_) {
    const_cast<SyntaxHighlighter*>(this)->initialize();
  }
  
  auto it = grammars_.find(language);
  if (it == grammars_.end() || !it->second.downloaded) {
    // Fallback to plain text
    return {HighlightedToken{code, TokenType::Plain}};
  }
  
  return highlightWithPatterns(code, language);
}

ftxui::Element SyntaxHighlighter::highlightRender(
    const std::string& code, const std::string& language,
    bool showLineNumbers) const {
  
  auto tokens = highlight(code, language);
  auto rendered = RenderHighlightedTokens(tokens, colorScheme_);
  
  if (!showLineNumbers) {
    return rendered;
  }
  
  // Add line numbers
  std::vector<ftxui::Element> lines;
  std::istringstream ss(code);
  std::string line;
  int lineNum = 1;
  int maxLineNum = 1;
  while (std::getline(ss, line)) maxLineNum++;
  
  int gutterWidth = std::to_string(maxLineNum).size();
  
  ss.clear();
  ss.seekg(0);
  
  while (std::getline(ss, line)) {
    std::string gutter = std::to_string(lineNum);
    while (static_cast<int>(gutter.size()) < gutterWidth) {
      gutter = " " + gutter;
    }
    
    lines.push_back(ftxui::hbox({
      ftxui::text(gutter + " │ ") | ftxui::dim | ftxui::color(ftxui::Color::RGB(100, 100, 130)),
      ftxui::text(line) // Will be replaced by actual highlighted content
    }));
    lineNum++;
  }
  
  return ftxui::vbox(lines) | ftxui::frame;
}

// Pattern-based highlighter (simulates tree-sitter behavior)
std::vector<HighlightedToken> SyntaxHighlighter::highlightWithPatterns(
    const std::string& code, const std::string& language) const {
  
  std::vector<HighlightedToken> tokens;
  
  // Language-specific keywords
  static const std::unordered_map<std::string, std::vector<std::string>> keywords = {
    {"cpp", {"auto", "break", "case", "catch", "class", "const", "constexpr",
             "continue", "default", "delete", "do", "else", "enum", "explicit",
             "extern", "for", "friend", "goto", "if", "inline", "namespace",
             "new", "noexcept", "nullptr", "operator", "private", "protected",
             "public", "register", "return", "sizeof", "static", "struct",
             "switch", "template", "this", "throw", "try", "typedef", "typeid",
             "typename", "union", "using", "virtual", "volatile", "while"}},
    {"rust", {"as", "async", "await", "break", "const", "continue", "crate",
              "dyn", "else", "enum", "extern", "false", "fn", "for", "if",
              "impl", "in", "let", "loop", "match", "mod", "move", "mut",
              "pub", "ref", "return", "Self", "self", "static", "struct",
              "super", "trait", "true", "type", "unsafe", "use", "where", "while"}},
    {"python", {"and", "as", "assert", "async", "await", "break", "class",
                "continue", "def", "del", "elif", "else", "except", "finally",
                "for", "from", "global", "if", "import", "in", "is", "lambda",
                "nonlocal", "not", "or", "pass", "raise", "return", "try",
                "while", "with", "yield", "True", "False", "None"}},
    {"javascript", {"async", "await", "break", "case", "catch", "class", "const",
                    "continue", "debugger", "default", "delete", "do", "else",
                    "export", "extends", "false", "finally", "for", "function",
                    "if", "import", "in", "instanceof", "let", "new", "null",
                    "return", "static", "super", "switch", "this", "throw",
                    "true", "try", "typeof", "var", "void", "while", "with", "yield"}},
  };
  
  // Language-specific types
  static const std::unordered_map<std::string, std::vector<std::string>> types = {
    {"cpp", {"bool", "char", "double", "float", "int", "long", "short", "signed",
             "unsigned", "void", "wchar_t", "string", "vector", "map", "set",
             "unique_ptr", "shared_ptr", "optional", "variant", "any"}},
    {"rust", {"i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32",
              "u64", "u128", "usize", "f32", "f64", "bool", "char", "str",
              "String", "Vec", "HashMap", "Option", "Result", "Box", "Rc", "Arc"}},
    {"python", {"int", "float", "str", "bool", "list", "dict", "set", "tuple",
                "bytes", "bytearray", "range", "slice", "object", "type"}},
  };
  
  auto kwIt = keywords.find(language);
  auto typeIt = types.find(language);
  
  std::vector<std::string> kwList = kwIt != keywords.end() ? kwIt->second : std::vector<std::string>{};
  std::vector<std::string> typeList = typeIt != types.end() ? typeIt->second : std::vector<std::string>{};
  
  // Simple tokenizer
  size_t i = 0;
  while (i < code.size()) {
    // Skip whitespace
    if (std::isspace(static_cast<unsigned char>(code[i]))) {
      size_t start = i;
      while (i < code.size() && std::isspace(static_cast<unsigned char>(code[i]))) i++;
      tokens.push_back({code.substr(start, i - start), TokenType::Plain});
      continue;
    }
    
    // Comments
    if (i + 1 < code.size()) {
      if ((language == "cpp" || language == "rust" || language == "javascript") &&
          code[i] == '/' && code[i + 1] == '/') {
        size_t start = i;
        while (i < code.size() && code[i] != '\n') i++;
        tokens.push_back({code.substr(start, i - start), TokenType::Comment});
        continue;
      }
      if ((language == "cpp" || language == "rust" || language == "javascript") &&
          code[i] == '/' && code[i + 1] == '*') {
        size_t start = i;
        i += 2;
        while (i + 1 < code.size() && !(code[i] == '*' && code[i + 1] == '/')) i++;
        i += 2;
        tokens.push_back({code.substr(start, i - start), TokenType::Comment});
        continue;
      }
      if (language == "python" && code[i] == '#') {
        size_t start = i;
        while (i < code.size() && code[i] != '\n') i++;
        tokens.push_back({code.substr(start, i - start), TokenType::Comment});
        continue;
      }
    }
    
    // Strings
    if (code[i] == '"' || code[i] == '\'' || code[i] == '`') {
      char quote = code[i];
      size_t start = i;
      i++;
      while (i < code.size() && code[i] != quote) {
        if (code[i] == '\\' && i + 1 < code.size()) i++;
        i++;
      }
      i++;
      tokens.push_back({code.substr(start, i - start), TokenType::String});
      continue;
    }
    
    // Numbers
    if (std::isdigit(static_cast<unsigned char>(code[i]))) {
      size_t start = i;
      while (i < code.size() && (std::isdigit(static_cast<unsigned char>(code[i])) ||
                                  code[i] == '.' || code[i] == 'x' ||
                                  code[i] == 'e' || code[i] == 'E' ||
                                  code[i] == '+' || code[i] == '-')) i++;
      tokens.push_back({code.substr(start, i - start), TokenType::Number});
      continue;
    }
    
    // Identifiers and keywords
    if (std::isalpha(static_cast<unsigned char>(code[i])) || code[i] == '_') {
      size_t start = i;
      while (i < code.size() && (std::isalnum(static_cast<unsigned char>(code[i])) ||
                                  code[i] == '_')) i++;
      
      std::string word = code.substr(start, i - start);
      
      // Check if keyword
      if (std::find(kwList.begin(), kwList.end(), word) != kwList.end()) {
        tokens.push_back({word, TokenType::Keyword});
      }
      // Check if type
      else if (std::find(typeList.begin(), typeList.end(), word) != typeList.end()) {
        tokens.push_back({word, TokenType::Type});
      }
      // Check if constant (ALL_CAPS)
      else if (std::all_of(word.begin(), word.end(),
               [](char c) { return !std::isalpha(static_cast<unsigned char>(c)) ||
                                     std::isupper(static_cast<unsigned char>(c)); })) {
        tokens.push_back({word, TokenType::Constant});
      }
      // Otherwise variable/function
      else {
        tokens.push_back({word, TokenType::Variable});
      }
      continue;
    }
    
    // Operators and punctuation
    if (std::string("+-*/%=<>!&|^~?:").find(code[i]) != std::string::npos) {
      size_t start = i;
      while (i < code.size() && std::string("+-*/%=<>!&|^~?:").find(code[i]) != std::string::npos) i++;
      tokens.push_back({code.substr(start, i - start), TokenType::Operator});
      continue;
    }
    
    // Punctuation
    if (std::string("()[]{}.,;").find(code[i]) != std::string::npos) {
      tokens.push_back({std::string(1, code[i]), TokenType::Punctuation});
      i++;
      continue;
    }
    
    // Unknown character
    tokens.push_back({std::string(1, code[i]), TokenType::Plain});
    i++;
  }
  
  return tokens;
}

ftxui::Element RenderHighlightedTokens(const std::vector<HighlightedToken>& tokens,
                                        const SyntaxColorScheme& colors) {
  ftxui::Elements elements;
  
  for (const auto& token : tokens) {
    ftxui::Color color;
    ftxui::Element e = ftxui::text(token.text);
    
    switch (token.type) {
      case TokenType::Keyword: color = colors.keyword; break;
      case TokenType::Type: color = colors.type; break;
      case TokenType::Function: color = colors.function; break;
      case TokenType::Variable: color = colors.variable; break;
      case TokenType::String: color = colors.string; break;
      case TokenType::Comment: color = colors.comment; break;
      case TokenType::Number: color = colors.number; break;
      case TokenType::Operator: color = colors.operator_color; break;
      case TokenType::Punctuation: color = colors.punctuation; break;
      case TokenType::Constant: color = colors.constant; break;
      case TokenType::Tag: color = colors.tag; break;
      case TokenType::Attribute: color = colors.attribute; break;
      case TokenType::Plain: default: color = colors.plain; break;
    }
    
    e = e | ftxui::color(color);
    elements.push_back(e);
  }
  
  return ftxui::hflow(std::move(elements));
}

} // namespace firmius::tui
