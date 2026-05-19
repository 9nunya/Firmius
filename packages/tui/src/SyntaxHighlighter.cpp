#include "SyntaxHighlighter.hpp"

#include "Terminal.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

extern "C" {
#include <tree_sitter/api.h>

const TSLanguage* tree_sitter_c(void);
const TSLanguage* tree_sitter_cpp(void);
const TSLanguage* tree_sitter_java(void);
const TSLanguage* tree_sitter_rust(void);
const TSLanguage* tree_sitter_python(void);
const TSLanguage* tree_sitter_javascript(void);
const TSLanguage* tree_sitter_typescript(void);
const TSLanguage* tree_sitter_json(void);
const TSLanguage* tree_sitter_yaml(void);
const TSLanguage* tree_sitter_toml(void);
const TSLanguage* tree_sitter_cmake(void);
const TSLanguage* tree_sitter_lua(void);
const TSLanguage* tree_sitter_luau(void);
const TSLanguage* tree_sitter_markdown(void);
const TSLanguage* tree_sitter_bash(void);
}

namespace firmius::tui {

namespace {

// ─── Node classification ────────────────────────────────────────────────────
// Lifted from v1's SyntaxHighlighter::classifyNode. The logic is language-
// agnostic over tree-sitter type names; v2 just changes the consumer side.

bool startsWith(const char* str, const char* prefix) {
  return std::strncmp(str, prefix, std::strlen(prefix)) == 0;
}

bool isKeywordText(const char* text) {
  static const char* keywords[] = {
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
      // YAML / TOML
      "true", "false", "null", "yes", "no", "on", "off",
      // Common
      "True", "False", "None",
      // Bash extras
      "then", "fi", "esac", "until", "done", "select", "time",
      nullptr};
  for (int i = 0; keywords[i]; ++i) {
    if (std::strcmp(text, keywords[i]) == 0) return true;
  }
  return false;
}

std::string nodeText(TSNode node, const char* source) {
  if (!source) return "";
  std::uint32_t s = ts_node_start_byte(node);
  std::uint32_t e = ts_node_end_byte(node);
  return (s < e) ? std::string(source + s, e - s) : std::string{};
}

HighlightKind classifyNode(TSNode node, const std::string& language,
                           const char* source) {
  const char* type = ts_node_type(node);
  bool named = ts_node_is_named(node);

  // ── Bash-specific classification ───────────────────────────────────────
  if (language == "bash") {
    if (named && std::strcmp(type, "word") == 0) {
      TSNode parent = ts_node_parent(node);
      if (!ts_node_is_null(parent)) {
        const char* pt = ts_node_type(parent);
        if (std::strcmp(pt, "command_name") == 0) {
          return HighlightKind::Function;
        }
      }
      if (source) {
        const std::string txt = nodeText(node, source);
        if (!txt.empty() && txt[0] == '-') return HighlightKind::Tag;
      }
      return HighlightKind::Plain;
    }
    if (std::strcmp(type, "variable_name") == 0 ||
        std::strcmp(type, "special_variable_name") == 0 ||
        std::strcmp(type, "simple_expansion") == 0 ||
        std::strcmp(type, "expansion") == 0) {
      return HighlightKind::Variable;
    }
    if (std::strcmp(type, "test_operator") == 0) return HighlightKind::Operator;
    if (std::strcmp(type, "file_descriptor") == 0) return HighlightKind::Number;
  }

  if (std::strcmp(type, "comment") == 0 ||
      std::strcmp(type, "line_comment") == 0 ||
      std::strcmp(type, "block_comment") == 0) {
    return HighlightKind::Comment;
  }

  if (std::strcmp(type, "string") == 0 ||
      std::strcmp(type, "string_literal") == 0 ||
      std::strcmp(type, "string_content") == 0 ||
      std::strcmp(type, "raw_string_literal") == 0 ||
      std::strcmp(type, "template_string") == 0 ||
      std::strcmp(type, "string_fragment") == 0 ||
      std::strcmp(type, "char_literal") == 0 ||
      std::strcmp(type, "heredoc_body") == 0 ||
      std::strcmp(type, "concatenated_string") == 0 ||
      startsWith(type, "\"") || startsWith(type, "'")) {
    return HighlightKind::String;
  }

  if (std::strcmp(type, "number") == 0 ||
      std::strcmp(type, "number_literal") == 0 ||
      std::strcmp(type, "integer") == 0 ||
      std::strcmp(type, "integer_literal") == 0 ||
      std::strcmp(type, "float") == 0 ||
      std::strcmp(type, "float_literal") == 0) {
    return HighlightKind::Number;
  }

  if (std::strcmp(type, "true") == 0 || std::strcmp(type, "false") == 0 ||
      std::strcmp(type, "null") == 0 || std::strcmp(type, "none") == 0 ||
      std::strcmp(type, "True") == 0 || std::strcmp(type, "False") == 0 ||
      std::strcmp(type, "None") == 0 || std::strcmp(type, "nullptr") == 0 ||
      std::strcmp(type, "boolean") == 0 ||
      std::strcmp(type, "null_literal") == 0) {
    return HighlightKind::Constant;
  }

  if (std::strcmp(type, "type_identifier") == 0 ||
      std::strcmp(type, "primitive_type") == 0 ||
      std::strcmp(type, "builtin_type") == 0 ||
      std::strcmp(type, "sized_type_specifier") == 0 ||
      std::strcmp(type, "type_builtin") == 0 ||
      std::strcmp(type, "generic_type") == 0 ||
      std::strcmp(type, "scoped_type_identifier") == 0 ||
      std::strcmp(type, "class_name") == 0 ||
      std::strcmp(type, "interface_type") == 0) {
    return HighlightKind::Type;
  }

  if (named && std::strcmp(type, "identifier") == 0) {
    TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent)) {
      const char* pt = ts_node_type(parent);
      if (std::strcmp(pt, "call_expression") == 0 ||
          std::strcmp(pt, "function_declarator") == 0 ||
          std::strcmp(pt, "function_definition") == 0 ||
          std::strcmp(pt, "method_declaration") == 0 ||
          std::strcmp(pt, "function_item") == 0 ||
          std::strcmp(pt, "decorated_definition") == 0 ||
          std::strcmp(pt, "attribute") == 0 ||
          std::strcmp(pt, "normal_command") == 0 ||
          std::strcmp(pt, "function_command") == 0 ||
          std::strcmp(pt, "macro_command") == 0) {
        TSNode firstChild = ts_node_child(parent, 0);
        if (ts_node_eq(firstChild, node)) return HighlightKind::Function;
      }
    }
    return HighlightKind::Variable;
  }

  if (std::strcmp(type, "field_identifier") == 0 ||
      std::strcmp(type, "property_identifier") == 0 ||
      std::strcmp(type, "shorthand_property_identifier") == 0) {
    return HighlightKind::Variable;
  }

  if (std::strcmp(type, "quoted_argument") == 0 ||
      std::strcmp(type, "bracket_argument") == 0) {
    return HighlightKind::String;
  }
  if (std::strcmp(type, "variable_reference") == 0) {
    return HighlightKind::Variable;
  }

  if (!named) {
    if (std::strlen(type) <= 3 && type[0] != '(' && type[0] != ')' &&
        type[0] != '{' && type[0] != '}' && type[0] != '[' && type[0] != ']' &&
        type[0] != ';' && type[0] != ',' && type[0] != '.' && type[0] != '"' &&
        type[0] != '\'') {
      bool allOp = true;
      for (const char* c = type; *c; ++c) {
        if (!((*c >= '!' && *c <= '/') || (*c >= ':' && *c <= '@') ||
              *c == '^' || *c == '~' || *c == '|' || *c == '&')) {
          allOp = false;
          break;
        }
      }
      if (allOp && std::strlen(type) > 0) return HighlightKind::Operator;
    }

    if (type[0] == '(' || type[0] == ')' || type[0] == '{' || type[0] == '}' ||
        type[0] == '[' || type[0] == ']' || type[0] == ';' || type[0] == ',' ||
        type[0] == '.' || type[0] == ':') {
      return HighlightKind::Punctuation;
    }

    if (isKeywordText(type)) return HighlightKind::Keyword;
  }

  if (std::strcmp(type, "tag") == 0 || std::strcmp(type, "anchor") == 0 ||
      std::strcmp(type, "alias") == 0) {
    return HighlightKind::Tag;
  }

  if (startsWith(type, "preproc") ||
      std::strcmp(type, "include") == 0 ||
      std::strcmp(type, "system_lib_string") == 0 ||
      std::strcmp(type, "header_name") == 0) {
    return HighlightKind::Tag;
  }

  if (std::strcmp(type, "attribute") == 0 ||
      std::strcmp(type, "decorator") == 0) {
    return HighlightKind::Attribute;
  }

  return HighlightKind::Plain;
}

} // namespace

// ─── Public API ────────────────────────────────────────────────────────────

SyntaxHighlighter& SyntaxHighlighter::instance() {
  static SyntaxHighlighter inst;
  return inst;
}

void SyntaxHighlighter::initialize() {
  if (initialized_) return;

  grammars_["c"] = {"C", {".c", ".h"}, tree_sitter_c};
  grammars_["cpp"] = {"C++", {".cpp", ".hpp", ".cc", ".cxx", ".hxx"},
                     tree_sitter_cpp};
  grammars_["bash"] = {"Bash", {".sh", ".bash", ".zsh"}, tree_sitter_bash};
  grammars_["java"] = {"Java", {".java"}, tree_sitter_java};
  grammars_["rust"] = {"Rust", {".rs"}, tree_sitter_rust};
  grammars_["python"] = {"Python", {".py", ".pyw"}, tree_sitter_python};
  grammars_["javascript"] = {"JavaScript",
                             {".js", ".jsx", ".mjs"},
                             tree_sitter_javascript};
  grammars_["typescript"] = {"TypeScript", {".ts", ".tsx"}, tree_sitter_typescript};
  grammars_["json"] = {"JSON", {".json"}, tree_sitter_json};
  grammars_["yaml"] = {"YAML", {".yaml", ".yml"}, tree_sitter_yaml};
  grammars_["toml"] = {"TOML", {".toml"}, tree_sitter_toml};
  grammars_["cmake"] = {"CMake",
                        {".cmake", "CMakeLists.txt"},
                        tree_sitter_cmake};
  grammars_["lua"] = {"Lua", {".lua"}, tree_sitter_lua};
  grammars_["luau"] = {"Luau", {".luau"}, tree_sitter_luau};
  grammars_["markdown"] = {"Markdown",
                           {".md", ".markdown"},
                           tree_sitter_markdown};

  initialized_ = true;
}

bool SyntaxHighlighter::hasGrammar(const std::string& language) const {
  if (!initialized_) const_cast<SyntaxHighlighter*>(this)->initialize();
  return grammars_.find(language) != grammars_.end();
}

std::vector<std::string> SyntaxHighlighter::availableLanguages() const {
  std::vector<std::string> langs;
  for (const auto& [lang, _] : grammars_) langs.push_back(lang);
  std::sort(langs.begin(), langs.end());
  return langs;
}

std::string SyntaxHighlighter::detectLanguage(const std::string& filename) const {
  if (!initialized_) const_cast<SyntaxHighlighter*>(this)->initialize();

  std::string ext;
  auto pos = filename.find_last_of('.');
  if (pos != std::string::npos) ext = filename.substr(pos);

  // Filename match wins (CMakeLists.txt etc.)
  for (const auto& [lang, info] : grammars_) {
    for (const auto& known : info.fileExtensions) {
      if (filename == known) return lang;
    }
  }
  // Extension match.
  if (!ext.empty()) {
    for (const auto& [lang, info] : grammars_) {
      for (const auto& known : info.fileExtensions) {
        if (ext == known) return lang;
      }
    }
  }
  return "";
}

ThemeRgb SyntaxHighlighter::colorFor(HighlightKind kind,
                                    const ThemeRgb& fallback) const {
  const auto& syntax = ThemeManager::instance().currentTheme().syntax;
  switch (kind) {
  case HighlightKind::Keyword: return syntax.keyword;
  case HighlightKind::Type: return syntax.type;
  case HighlightKind::Function: return syntax.function;
  case HighlightKind::Variable: return syntax.variable;
  case HighlightKind::String: return syntax.string;
  case HighlightKind::Comment: return syntax.comment;
  case HighlightKind::Number: return syntax.number;
  case HighlightKind::Operator: return syntax.op;
  case HighlightKind::Constant: return syntax.constant;
  case HighlightKind::Tag: return syntax.tag;
  case HighlightKind::Attribute: return syntax.attr;
  case HighlightKind::Punctuation:
  case HighlightKind::Plain:
  default:
    return fallback;
  }
}

void SyntaxHighlighter::parseAndCollect(const std::string& code,
                                       const std::string& language,
                                       std::vector<HighlightSpan>& spans) const {
  auto it = grammars_.find(language);
  if (it == grammars_.end()) return;

  const TSLanguage* tsLang = it->second.languageFn();
  TSParser* parser = ts_parser_new();
  ts_parser_set_language(parser, tsLang);

  TSTree* tree = ts_parser_parse_string(parser, nullptr, code.c_str(),
                                        static_cast<std::uint32_t>(code.size()));
  if (!tree) {
    ts_parser_delete(parser);
    return;
  }

  TSNode root = ts_tree_root_node(tree);
  spans.reserve(256);

  // Recursive lambda — collectSpansImpl can't access HighlightSpan because
  // it's a private member type; doing the recursion inline keeps everything
  // tidy and avoids friending an anon-namespace function.
  std::vector<TSNode> stack;
  stack.push_back(root);
  while (!stack.empty()) {
    TSNode node = stack.back();
    stack.pop_back();

    std::uint32_t childCount = ts_node_child_count(node);
    if (childCount == 0) {
      HighlightKind kind = classifyNode(node, language, code.c_str());
      std::uint32_t s = ts_node_start_byte(node);
      std::uint32_t e = ts_node_end_byte(node);
      if (s < e) spans.push_back({s, e, kind});
      continue;
    }

    HighlightKind nodeKind = classifyNode(node, language, code.c_str());
    if (nodeKind == HighlightKind::Comment ||
        nodeKind == HighlightKind::String) {
      std::uint32_t s = ts_node_start_byte(node);
      std::uint32_t e = ts_node_end_byte(node);
      if (s < e) spans.push_back({s, e, nodeKind});
      continue;
    }

    // Push children in reverse so we visit them left-to-right (DFS pre-order).
    for (std::uint32_t i = childCount; i-- > 0;) {
      stack.push_back(ts_node_child(node, i));
    }
  }

  std::sort(spans.begin(), spans.end(),
            [](const HighlightSpan& a, const HighlightSpan& b) {
              return a.startByte < b.startByte;
            });

  ts_tree_delete(tree);
  ts_parser_delete(parser);
}

std::vector<std::string>
SyntaxHighlighter::highlightLines(const std::string& code,
                                  const std::string& language,
                                  const ThemeRgb& fallbackFg) const {
  if (!initialized_) const_cast<SyntaxHighlighter*>(this)->initialize();

  std::vector<std::string> rendered;
  if (code.empty()) {
    rendered.emplace_back("");
    return rendered;
  }

  // No grammar — return plain-fg lines so callers don't have to special-case.
  if (grammars_.find(language) == grammars_.end()) {
    std::istringstream ss(code);
    std::string line;
    while (std::getline(ss, line)) {
      rendered.push_back(ansi::fgRgb(fallbackFg.r, fallbackFg.g, fallbackFg.b,
                                     line));
    }
    if (rendered.empty()) {
      rendered.push_back(ansi::fgRgb(fallbackFg.r, fallbackFg.g, fallbackFg.b,
                                     code));
    }
    return rendered;
  }

  std::vector<HighlightSpan> spans;
  parseAndCollect(code, language, spans);

  // ── Walk lines, intersect with spans ─────────────────────────────────────
  std::istringstream ss(code);
  std::string line;
  std::uint32_t lineStart = 0;
  std::size_t spanIdx = 0;
  const auto* src = code.c_str();
  (void)src;

  while (std::getline(ss, line)) {
    std::uint32_t lineEnd =
        lineStart + static_cast<std::uint32_t>(line.size());

    std::string out;
    out.reserve(line.size() + 32);
    std::uint32_t cursor = lineStart;

    // Catch up spanIdx to the first span that touches this line.
    while (spanIdx < spans.size() && spans[spanIdx].endByte <= lineStart) {
      ++spanIdx;
    }

    for (std::size_t i = spanIdx; i < spans.size(); ++i) {
      const auto& span = spans[i];
      if (span.startByte >= lineEnd) break;

      std::uint32_t sStart = std::max(span.startByte, lineStart);
      std::uint32_t sEnd = std::min(span.endByte, lineEnd);

      if (sStart > cursor) {
        // Plain gap.
        const std::string gap = code.substr(cursor, sStart - cursor);
        out += ansi::fgRgb(fallbackFg.r, fallbackFg.g, fallbackFg.b, gap);
      }

      const ThemeRgb color = colorFor(span.kind, fallbackFg);
      const std::string segment = code.substr(sStart, sEnd - sStart);
      out += ansi::fgRgb(color.r, color.g, color.b, segment);
      cursor = sEnd;
    }

    if (cursor < lineEnd) {
      const std::string tail = code.substr(cursor, lineEnd - cursor);
      out += ansi::fgRgb(fallbackFg.r, fallbackFg.g, fallbackFg.b, tail);
    }

    rendered.push_back(std::move(out));
    lineStart = lineEnd + 1; // skip the consumed '\n'
  }

  if (rendered.empty()) {
    rendered.emplace_back("");
  }
  return rendered;
}

std::string SyntaxHighlighter::highlightLine(const std::string& code,
                                            const std::string& language,
                                            const ThemeRgb& fallbackFg) const {
  auto lines = highlightLines(code, language, fallbackFg);
  if (lines.empty()) return "";
  return lines.front();
}

} // namespace firmius::tui
