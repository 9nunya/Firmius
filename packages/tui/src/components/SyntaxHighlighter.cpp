#include "components/SyntaxHighlighter.hpp"
#include "ThemeManager.hpp"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace firmius::tui {

namespace {

void appendStyledTextParts(std::vector<ftxui::Element> &parts,
                           const std::string &text, ftxui::Color color) {
  if (text.empty()) {
    return;
  }
  for (char ch : text) {
    parts.push_back(ftxui::text(std::string(1, ch)) | ftxui::color(color));
  }
}

} // namespace

SyntaxHighlighter &SyntaxHighlighter::instance() {
  static SyntaxHighlighter inst;
  return inst;
}

void SyntaxHighlighter::initialize() {
  if (initialized_)
    return;

  // Register all compiled-in grammars with their TSLanguage constructors
  grammars_["c"] = GrammarInfo{
      .name = "C",
      .fileExtensions = {".c", ".h"},
      .languageFn = tree_sitter_c,
  };

  grammars_["cpp"] = GrammarInfo{
      .name = "C++",
      .fileExtensions = {".cpp", ".hpp", ".cc", ".cxx", ".hxx"},
      .languageFn = tree_sitter_cpp,
  };

  grammars_["bash"] = GrammarInfo{
      .name = "Bash",
      .fileExtensions = {".sh", ".bash", ".zsh"},
      .languageFn = tree_sitter_bash,
  };

  grammars_["java"] = GrammarInfo{
      .name = "Java",
      .fileExtensions = {".java"},
      .languageFn = tree_sitter_java,
  };

  grammars_["rust"] = GrammarInfo{
      .name = "Rust",
      .fileExtensions = {".rs"},
      .languageFn = tree_sitter_rust,
  };

  grammars_["python"] = GrammarInfo{
      .name = "Python",
      .fileExtensions = {".py", ".pyw"},
      .languageFn = tree_sitter_python,
  };

  grammars_["javascript"] = GrammarInfo{
      .name = "JavaScript",
      .fileExtensions = {".js", ".jsx", ".mjs"},
      .languageFn = tree_sitter_javascript,
  };

  grammars_["typescript"] = GrammarInfo{
      .name = "TypeScript",
      .fileExtensions = {".ts", ".tsx"},
      .languageFn = tree_sitter_typescript,
  };

  grammars_["json"] = GrammarInfo{
      .name = "JSON",
      .fileExtensions = {".json"},
      .languageFn = tree_sitter_json,
  };

  grammars_["yaml"] = GrammarInfo{
      .name = "YAML",
      .fileExtensions = {".yaml", ".yml"},
      .languageFn = tree_sitter_yaml,
  };

  grammars_["toml"] = GrammarInfo{
      .name = "TOML",
      .fileExtensions = {".toml"},
      .languageFn = tree_sitter_toml,
  };

  grammars_["cmake"] = GrammarInfo{
      .name = "CMake",
      .fileExtensions = {".cmake", "CMakeLists.txt"},
      .languageFn = tree_sitter_cmake,
  };

  grammars_["lua"] = GrammarInfo{
      .name = "Lua",
      .fileExtensions = {".lua"},
      .languageFn = tree_sitter_lua,
  };

  grammars_["luau"] = GrammarInfo{
      .name = "Luau",
      .fileExtensions = {".luau"},
      .languageFn = tree_sitter_luau,
  };

  grammars_["markdown"] = GrammarInfo{
      .name = "Markdown",
      .fileExtensions = {".md", ".markdown"},
      .languageFn = tree_sitter_markdown,
  };

  initialized_ = true;
}

bool SyntaxHighlighter::hasGrammar(const std::string &language) const {
  if (!initialized_) {
    const_cast<SyntaxHighlighter *>(this)->initialize();
  }
  return grammars_.find(language) != grammars_.end();
}

const GrammarInfo *
SyntaxHighlighter::getGrammarInfo(const std::string &language) const {
  auto it = grammars_.find(language);
  return it != grammars_.end() ? &it->second : nullptr;
}

std::vector<std::string> SyntaxHighlighter::getAvailableLanguages() const {
  std::vector<std::string> langs;
  for (const auto &[lang, _] : grammars_) {
    langs.push_back(lang);
  }
  std::sort(langs.begin(), langs.end());
  return langs;
}

std::string
SyntaxHighlighter::detectLanguage(const std::string &filename) const {
  if (!initialized_) {
    const_cast<SyntaxHighlighter *>(this)->initialize();
  }

  std::string ext;
  auto pos = filename.find_last_of('.');
  if (pos != std::string::npos) {
    ext = filename.substr(pos);
  }

  for (const auto &[lang, info] : grammars_) {
    for (const auto &knownExt : info.fileExtensions) {
      if (filename == knownExt || (!ext.empty() && ext == knownExt)) {
        return lang;
      }
    }
  }

  return "";
}

std::string SyntaxHighlighter::highlight(const std::string &code,
                                         const std::string &language) const {
  // Plain text passthrough — use highlightRender() for actual highlighting
  (void)language;
  return code;
}

// ─── Node classification ────────────────────────────────────────────────────

// Helper: check if a C string starts with a prefix
static bool startsWith(const char *str, const char *prefix) {
  return strncmp(str, prefix, strlen(prefix)) == 0;
}

// Helper: check if an anonymous (literal) leaf node is a keyword
static bool isKeywordText(const char *text) {
  static const char *keywords[] = {
      // C / C++
      "if", "else", "for", "while", "do", "switch", "case", "break", "continue",
      "return", "goto", "typedef", "struct", "union", "enum", "sizeof",
      "static", "extern", "inline", "const", "volatile", "register", "void",
      "auto", "default", "signed", "unsigned",
      // C++ extras
      "class", "namespace", "template", "typename", "public", "private",
      "protected", "virtual", "override", "final", "new", "delete", "try",
      "catch", "throw", "using", "constexpr", "noexcept", "explicit", "friend",
      "operator", "this", "nullptr", "static_cast", "dynamic_cast",
      "const_cast", "reinterpret_cast", "co_await", "co_yield", "co_return",
      "concept", "requires",
      // Java
      "abstract", "implements", "extends", "interface", "package", "import",
      "instanceof", "synchronized", "throws", "transient", "native", "strictfp",
      "assert", "super",
      // Rust
      "fn", "let", "mut", "impl", "trait", "pub", "mod", "use", "crate", "self",
      "Self", "where", "as", "in", "ref", "move", "unsafe", "async", "await",
      "dyn", "type", "loop", "match",
      // Python
      "def", "lambda", "import", "from", "pass", "raise", "with", "yield",
      "global", "nonlocal", "del", "and", "or", "not", "is", "in", "elif",
      "except", "finally",
      // JavaScript / TypeScript
      "void", "delete", "in", "instanceof", "export", "import", "from",
      // Lua / Luau
      "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
      "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return",
      "then", "true", "until", "while",
      // CMake
      "if", "else", "elseif", "endif", "foreach", "endforeach", "while",
      "endwhile", "macro", "endmacro", "function", "endfunction", "return",
      // YAML / TOML (few keywords)
      "true", "false", "null", "yes", "no", "on", "off",
      // Common
      "True", "False", "None",
      // Bash extras
      "then", "fi", "esac", "until", "done", "select", "time",
      nullptr};
  for (int i = 0; keywords[i]; ++i) {
    if (strcmp(text, keywords[i]) == 0)
      return true;
  }
  return false;
}

// Helper: extract the source text for a node (caller must supply the full source)
static std::string nodeText(TSNode node, const char *source) {
  if (!source) return "";
  uint32_t s = ts_node_start_byte(node);
  uint32_t e = ts_node_end_byte(node);
  return (s < e) ? std::string(source + s, e - s) : std::string{};
}

HighlightKind
SyntaxHighlighter::classifyNode(TSNode node, const std::string &language,
                                const char *source) const {
  const char *type = ts_node_type(node);
  bool named = ts_node_is_named(node);

  // ── Bash-specific classification ──────────────────────────────────────────
  // tree-sitter-bash uses `word` for commands, flags, and arguments alike.
  // We disambiguate via parent context and source-text peeking.
  if (language == "bash") {
    // command_name > word  →  Function (the executable)
    if (named && strcmp(type, "word") == 0) {
      TSNode parent = ts_node_parent(node);
      if (!ts_node_is_null(parent)) {
        const char *pt = ts_node_type(parent);
        if (strcmp(pt, "command_name") == 0) {
          return HighlightKind::Function;
        }
      }
      // argument words: flags start with '-'
      if (source) {
        const std::string txt = nodeText(node, source);
        if (!txt.empty() && txt[0] == '-') {
          return HighlightKind::Tag; // flag
        }
      }
      return HighlightKind::Plain; // plain argument
    }
    // $VAR expansions
    if (strcmp(type, "variable_name") == 0 ||
        strcmp(type, "special_variable_name") == 0) {
      return HighlightKind::Variable;
    }
    if (strcmp(type, "simple_expansion") == 0 ||
        strcmp(type, "expansion") == 0) {
      return HighlightKind::Variable;
    }
    // $(...) substitution delimiters
    if (strcmp(type, "command_substitution") == 0) {
      return HighlightKind::Plain; // children handle inner tokens
    }
    // test operators: -f, -d, -z, etc.
    if (strcmp(type, "test_operator") == 0) {
      return HighlightKind::Operator;
    }
    // file_redirect, heredoc_redirect
    if (strcmp(type, "file_redirect") == 0 ||
        strcmp(type, "heredoc_redirect") == 0) {
      return HighlightKind::Plain;
    }
    // file descriptor number
    if (strcmp(type, "file_descriptor") == 0) {
      return HighlightKind::Number;
    }
    // fall through to generic rules for comments, strings, pipes, etc.
  }

  // ── Comments ──
  if (strcmp(type, "comment") == 0 || strcmp(type, "line_comment") == 0 ||
      strcmp(type, "block_comment") == 0) {
    return HighlightKind::Comment;
  }

  // ── Strings ──
  if (strcmp(type, "string") == 0 || strcmp(type, "string_literal") == 0 ||
      strcmp(type, "string_content") == 0 ||
      strcmp(type, "raw_string_literal") == 0 ||
      strcmp(type, "template_string") == 0 ||
      strcmp(type, "string_fragment") == 0 ||
      strcmp(type, "char_literal") == 0 || strcmp(type, "heredoc_body") == 0 ||
      strcmp(type, "concatenated_string") == 0 ||
      startsWith(type, "\"") || // anonymous string delimiters
      startsWith(type, "'")) {
    return HighlightKind::String;
  }

  // ── Numbers ──
  if (strcmp(type, "number") == 0 || strcmp(type, "number_literal") == 0 ||
      strcmp(type, "integer") == 0 || strcmp(type, "integer_literal") == 0 ||
      strcmp(type, "float") == 0 || strcmp(type, "float_literal") == 0) {
    return HighlightKind::Number;
  }

  // ── Boolean / null constants ──
  if (strcmp(type, "true") == 0 || strcmp(type, "false") == 0 ||
      strcmp(type, "null") == 0 || strcmp(type, "none") == 0 ||
      strcmp(type, "True") == 0 || strcmp(type, "False") == 0 ||
      strcmp(type, "None") == 0 || strcmp(type, "nullptr") == 0 ||
      strcmp(type, "boolean") == 0 || strcmp(type, "null_literal") == 0) {
    return HighlightKind::Constant;
  }

  // ── Types (named type identifiers) ──
  if (strcmp(type, "type_identifier") == 0 ||
      strcmp(type, "primitive_type") == 0 ||
      strcmp(type, "builtin_type") == 0 ||
      strcmp(type, "sized_type_specifier") == 0 ||
      strcmp(type, "type_builtin") == 0 || strcmp(type, "generic_type") == 0 ||
      strcmp(type, "scoped_type_identifier") == 0 ||
      strcmp(type, "class_name") == 0 || strcmp(type, "interface_type") == 0) {
    return HighlightKind::Type;
  }

  // ── Function names (identifiers inside call/definition nodes) ──
  if (named && strcmp(type, "identifier") == 0) {
    TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent)) {
      const char *parentType = ts_node_type(parent);
      if (strcmp(parentType, "call_expression") == 0 ||
          strcmp(parentType, "function_declarator") == 0 ||
          strcmp(parentType, "function_definition") == 0 ||
          strcmp(parentType, "method_declaration") == 0 ||
          strcmp(parentType, "function_item") == 0 ||
          strcmp(parentType, "decorated_definition") == 0 ||
          strcmp(parentType, "attribute") == 0 ||
          // CMake
          strcmp(parentType, "normal_command") == 0 ||
          strcmp(parentType, "function_command") == 0 ||
          strcmp(parentType, "macro_command") == 0) {
        // Only the first child (function name) — not arguments
        TSNode firstChild = ts_node_child(parent, 0);
        if (ts_node_eq(firstChild, node)) {
          return HighlightKind::Function;
        }
      }
    }

    // CMake variables
    if (startsWith(type, "variable")) {
      return HighlightKind::Variable;
    }

    return HighlightKind::Variable;
  }

  // ── CMake specific ──
  if (strcmp(type, "normal_command") == 0 ||
      strcmp(type, "function_command") == 0 ||
      strcmp(type, "macro_command") == 0) {
    return HighlightKind::Plain; // Children handle it
  }
  if (strcmp(type, "argument") == 0 || strcmp(type, "unquoted_argument") == 0) {
    return HighlightKind::Plain;
  }
  if (strcmp(type, "quoted_argument") == 0 ||
      strcmp(type, "bracket_argument") == 0) {
    return HighlightKind::String;
  }
  if (strcmp(type, "variable_reference") == 0) {
    return HighlightKind::Variable;
  }

  // ── Field / property identifiers ──
  if (strcmp(type, "field_identifier") == 0 ||
      strcmp(type, "property_identifier") == 0 ||
      strcmp(type, "shorthand_property_identifier") == 0) {
    return HighlightKind::Variable;
  }

  // ── Operators ──
  if (!named) {
    // Anonymous nodes that are operator symbols
    if (strlen(type) <= 3 && type[0] != '(' && type[0] != ')' &&
        type[0] != '{' && type[0] != '}' && type[0] != '[' && type[0] != ']' &&
        type[0] != ';' && type[0] != ',' && type[0] != '.' && type[0] != '"' &&
        type[0] != '\'') {
      // Likely an operator: +, -, *, /, =, ==, !=, <, >, etc.
      bool allOp = true;
      for (const char *c = type; *c; ++c) {
        if (!((*c >= '!' && *c <= '/') || (*c >= ':' && *c <= '@') ||
              *c == '^' || *c == '~' || *c == '|' || *c == '&')) {
          allOp = false;
          break;
        }
      }
      if (allOp && strlen(type) > 0) {
        return HighlightKind::Operator;
      }
    }

    // Punctuation: brackets, parens, braces, semicolons, commas, dots
    if (type[0] == '(' || type[0] == ')' || type[0] == '{' || type[0] == '}' ||
        type[0] == '[' || type[0] == ']' || type[0] == ';' || type[0] == ',' ||
        type[0] == '.' || type[0] == ':') {
      return HighlightKind::Punctuation;
    }

    // Anonymous keyword-like text nodes (e.g., "if", "for", "class")
    if (isKeywordText(type)) {
      return HighlightKind::Keyword;
    }
  }

  // ── YAML / JSON specific tags ──
  if (strcmp(type, "tag") == 0 || strcmp(type, "anchor") == 0 ||
      strcmp(type, "alias") == 0) {
    return HighlightKind::Tag;
  }

  // ── Keys in key-value pairs ──
  if (strcmp(type, "pair") == 0) {
    // Don't color the pair node itself — children handle it
    return HighlightKind::Plain;
  }

  // ── Preproc / include ──
  if (startsWith(type, "preproc") || strcmp(type, "include") == 0 ||
      strcmp(type, "system_lib_string") == 0 ||
      strcmp(type, "header_name") == 0) {
    return HighlightKind::Tag;
  }

  // ── Attribute-like nodes ──
  if (strcmp(type, "attribute") == 0 || strcmp(type, "decorator") == 0) {
    return HighlightKind::Attribute;
  }

  return HighlightKind::Plain;
}

ftxui::Color SyntaxHighlighter::colorFor(HighlightKind kind) const {
  const auto &theme = ThemeManager::instance().getCurrentTheme();
  switch (kind) {
  case HighlightKind::Keyword:
    return theme.syntax.keyword;
  case HighlightKind::Type:
    return theme.syntax.type;
  case HighlightKind::Function:
    return theme.syntax.function;
  case HighlightKind::Variable:
    return theme.syntax.variable;
  case HighlightKind::String:
    return theme.syntax.string;
  case HighlightKind::Comment:
    return theme.syntax.comment;
  case HighlightKind::Number:
    return theme.syntax.number;
  case HighlightKind::Operator:
    return theme.syntax.op;
  case HighlightKind::Punctuation:
    return theme.base.fg;
  case HighlightKind::Constant:
    return theme.syntax.constant;
  case HighlightKind::Tag:
    return theme.syntax.tag;
  case HighlightKind::Attribute:
    return theme.syntax.attr;
  case HighlightKind::Plain:
  default:
    return theme.base.fg;
  }
}

// ─── AST span collection ────────────────────────────────────────────────────

void SyntaxHighlighter::collectSpans(TSNode node, const std::string &language,
                                     std::vector<HighlightSpan> &spans,
                                     const char *source) const {
  uint32_t childCount = ts_node_child_count(node);

  if (childCount == 0) {
    // Leaf node — emit a span
    HighlightKind kind = classifyNode(node, language, source);
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start < end) {
      spans.push_back({start, end, kind});
    }
    return;
  }

  // For comments and strings, treat the whole subtree as one span
  HighlightKind nodeKind = classifyNode(node, language, source);
  if (nodeKind == HighlightKind::Comment || nodeKind == HighlightKind::String) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start < end) {
      spans.push_back({start, end, nodeKind});
    }
    return;
  }

  // Recurse into children (including anonymous children)
  for (uint32_t i = 0; i < childCount; ++i) {
    TSNode child = ts_node_child(node, i);
    collectSpans(child, language, spans, source);
  }
}

// ─── Public rendering ───────────────────────────────────────────────────────

std::vector<ftxui::Element>
SyntaxHighlighter::highlightRenderLines(const std::string &code,
                                        const std::string &language) const {

  if (!initialized_) {
    const_cast<SyntaxHighlighter *>(this)->initialize();
  }

  auto it = grammars_.find(language);
  if (it == grammars_.end()) {
    // Unknown language — plain text fallback
    std::vector<ftxui::Element> lines;
    std::istringstream ss(code);
    std::string lineStr;
    while (std::getline(ss, lineStr)) {
      lines.push_back(ftxui::text(lineStr));
    }
    return lines;
  }

  // ── Parse with tree-sitter ──
  const TSLanguage *tsLang = it->second.languageFn();
  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, tsLang);

  TSTree *tree = ts_parser_parse_string(parser, nullptr, code.c_str(),
                                        static_cast<uint32_t>(code.size()));

  if (!tree) {
    // Parse failure — plain text fallback
    ts_parser_delete(parser);
    std::vector<ftxui::Element> lines;
    std::istringstream ss(code);
    std::string lineStr;
    while (std::getline(ss, lineStr)) {
      lines.push_back(ftxui::text(lineStr));
    }
    return lines;
  }

  TSNode root = ts_tree_root_node(tree);

  // Collect highlight spans from the AST
  std::vector<HighlightSpan> spans;
  spans.reserve(256);
  collectSpans(root, language, spans, code.c_str());

  // Sort spans by start byte (should already be mostly sorted from DFS)
  std::sort(spans.begin(), spans.end(),
            [](const HighlightSpan &a, const HighlightSpan &b) {
              return a.startByte < b.startByte;
            });

  // ── Build line-by-line FTXUI elements from highlight spans ──
  std::vector<ftxui::Element> renderedLines;
  std::istringstream ss(code);
  std::string lineStr;
  uint32_t lineStartByte = 0;

  while (std::getline(ss, lineStr)) {
    uint32_t lineEndByte =
        lineStartByte + static_cast<uint32_t>(lineStr.size());

    std::vector<ftxui::Element> parts;

    // Walk spans that intersect this line
    uint32_t cursor = lineStartByte;
    for (const auto &span : spans) {
      if (span.endByte <= lineStartByte)
        continue; // span is before this line
      if (span.startByte >= lineEndByte)
        break; // past this line

      // Clip span to this line
      uint32_t sStart = std::max(span.startByte, lineStartByte);
      uint32_t sEnd = std::min(span.endByte, lineEndByte);

      // Gap between cursor and this span = plain text
      if (sStart > cursor) {
        std::string gap = code.substr(cursor, sStart - cursor);
        parts.push_back(
            ftxui::text(gap) |
            ftxui::color(ThemeManager::instance().getCurrentTheme().base.fg));
      }

      // The highlighted span text
      std::string spanText = code.substr(sStart, sEnd - sStart);
      parts.push_back(ftxui::text(spanText) |
                      ftxui::color(colorFor(span.kind)));

      cursor = sEnd;
    }

    // Remaining text after last span on this line
    if (cursor < lineEndByte) {
      std::string tail = code.substr(cursor, lineEndByte - cursor);
      parts.push_back(
          ftxui::text(tail) |
          ftxui::color(ThemeManager::instance().getCurrentTheme().base.fg));
    }

    // If the line was completely empty, ensure we push an empty text element
    if (parts.empty()) {
      parts.push_back(ftxui::text(""));
    }

    renderedLines.push_back(ftxui::hbox(parts));
    lineStartByte = lineEndByte + 1; // +1 for the newline character
  }

  ts_tree_delete(tree);
  ts_parser_delete(parser);

  return renderedLines;
}

ftxui::Element
SyntaxHighlighter::highlightRenderWrappedLine(const std::string &code,
                                              const std::string &language) const {
  if (!initialized_) {
    const_cast<SyntaxHighlighter *>(this)->initialize();
  }

  const auto &theme = ThemeManager::instance().getCurrentTheme();
  auto it = grammars_.find(language);
  if (it == grammars_.end()) {
    return ftxui::paragraph(code.empty() ? " " : code) |
           ftxui::color(theme.base.fg);
  }

  const TSLanguage *tsLang = it->second.languageFn();
  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, tsLang);

  TSTree *tree = ts_parser_parse_string(parser, nullptr, code.c_str(),
                                        static_cast<uint32_t>(code.size()));

  if (!tree) {
    ts_parser_delete(parser);
    return ftxui::paragraph(code.empty() ? " " : code) |
           ftxui::color(theme.base.fg);
  }

  TSNode root = ts_tree_root_node(tree);
  std::vector<HighlightSpan> spans;
  spans.reserve(64);
  collectSpans(root, language, spans, code.c_str());
  std::sort(spans.begin(), spans.end(),
            [](const HighlightSpan &a, const HighlightSpan &b) {
              return a.startByte < b.startByte;
            });

  std::vector<ftxui::Element> parts;
  uint32_t cursor = 0;
  const uint32_t lineEndByte = static_cast<uint32_t>(code.size());
  for (const auto &span : spans) {
    if (span.endByte <= cursor) {
      continue;
    }
    if (span.startByte >= lineEndByte) {
      break;
    }

    const uint32_t sStart = std::max(span.startByte, cursor);
    const uint32_t sEnd = std::min(span.endByte, lineEndByte);
    if (sStart > cursor) {
      appendStyledTextParts(parts, code.substr(cursor, sStart - cursor),
                            theme.base.fg);
    }

    appendStyledTextParts(parts, code.substr(sStart, sEnd - sStart),
                          colorFor(span.kind));
    cursor = sEnd;
  }

  if (cursor < lineEndByte) {
    appendStyledTextParts(parts, code.substr(cursor, lineEndByte - cursor),
                          theme.base.fg);
  }

  if (parts.empty()) {
    parts.push_back(ftxui::text(" ") | ftxui::color(theme.base.fg));
  }

  ts_tree_delete(tree);
  ts_parser_delete(parser);

  return ftxui::hflow(std::move(parts));
}

ftxui::Element SyntaxHighlighter::highlightRender(const std::string &code,
                                                  const std::string &language,
                                                  bool showLineNumbers) const {
  auto lines = highlightRenderLines(code, language);

  if (!showLineNumbers) {
    return ftxui::vbox(lines) | ftxui::frame;
  }

  std::vector<ftxui::Element> linesWithNumbers;
  linesWithNumbers.reserve(lines.size());

  int lineNum = 1;
  const auto &theme = ThemeManager::instance().getCurrentTheme();
  for (auto &line : lines) {
    std::string gutter = std::to_string(lineNum);
    linesWithNumbers.push_back(
        ftxui::hbox({ftxui::text(gutter + " │ ") | ftxui::dim |
                         ftxui::color(theme.base.dim),
                     line}));
    lineNum++;
  }

  return ftxui::vbox(linesWithNumbers) | ftxui::frame;
}

} // namespace firmius::tui
