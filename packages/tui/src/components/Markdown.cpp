#include "components/Markdown.hpp"
#include <cctype>
#include <functional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/terminal.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace firmius::tui {

// Global width constraint for markdown rendering (set before rendering)
static thread_local int g_markdown_width = 0;

void SetMarkdownWidth(int width) { g_markdown_width = width; }

namespace {

struct MarkdownCacheKey {
  std::string text;
  bool dim;
  int width;

  bool operator==(const MarkdownCacheKey& other) const {
    return text == other.text && dim == other.dim && width == other.width;
  }
};

struct MarkdownCacheHasher {
  std::size_t operator()(const MarkdownCacheKey& key) const {
    std::size_t h = std::hash<std::string>{}(key.text);
    h ^= std::hash<bool>{}(key.dim) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(key.width) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

using MarkdownCache = std::unordered_map<MarkdownCacheKey, ftxui::Element, MarkdownCacheHasher>;

MarkdownCache& getMarkdownCache() {
  static thread_local MarkdownCache cache;
  return cache;
}

std::vector<MarkdownCacheKey>& getMarkdownCacheOrder() {
  static thread_local std::vector<MarkdownCacheKey> order;
  return order;
}
std::size_t& getMarkdownCacheBytes() {
  static thread_local std::size_t bytes = 0;
  return bytes;
}

std::size_t markdownCacheKeyBytes(const MarkdownCacheKey& key) {
  return key.text.size() + sizeof(key.dim) + sizeof(key.width);
}


void rememberMarkdownCacheKey(const MarkdownCacheKey& key) {
  auto& order = getMarkdownCacheOrder();
  auto& bytes = getMarkdownCacheBytes();
  order.push_back(key);
  bytes += markdownCacheKeyBytes(key);
  // Bound by both entries and text bytes. Entry count alone is misleading: a
  // few long transcript/tool rows can dominate RSS because the key stores the
  // full text and the value stores a recursively-owned FTXUI node tree.
  constexpr std::size_t kMaxEntries = 1500;
  constexpr std::size_t kTrimToEntries = 1000;
  constexpr std::size_t kMaxBytes = 8 * 1024 * 1024;
  constexpr std::size_t kTrimToBytes = 6 * 1024 * 1024;
  if (order.size() <= kMaxEntries && bytes <= kMaxBytes) {
    return;
  }
  auto& cache = getMarkdownCache();
  std::size_t drop_count = 0;
  while (drop_count < order.size() &&
         (order.size() - drop_count > kTrimToEntries || bytes > kTrimToBytes)) {
    const auto& old = order[drop_count];
    if (cache.erase(old) > 0) {
      const auto old_bytes = markdownCacheKeyBytes(old);
      bytes = old_bytes > bytes ? 0 : bytes - old_bytes;
    }
    ++drop_count;
  }
  order.erase(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(drop_count));
}

std::string_view trimAsciiWhitespace(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return value;
}

std::string extractXmlAttribute(std::string_view attrs,
                                std::string_view name) {
  std::size_t pos = 0;
  while (pos < attrs.size()) {
    while (pos < attrs.size() &&
           std::isspace(static_cast<unsigned char>(attrs[pos]))) {
      ++pos;
    }
    if (pos >= attrs.size()) {
      break;
    }

    const std::size_t keyStart = pos;
    while (pos < attrs.size() && attrs[pos] != '=' &&
           !std::isspace(static_cast<unsigned char>(attrs[pos]))) {
      ++pos;
    }
    const std::string_view key = attrs.substr(keyStart, pos - keyStart);

    while (pos < attrs.size() &&
           std::isspace(static_cast<unsigned char>(attrs[pos]))) {
      ++pos;
    }
    if (pos >= attrs.size() || attrs[pos] != '=') {
      while (pos < attrs.size() && attrs[pos] != '>' &&
             !std::isspace(static_cast<unsigned char>(attrs[pos]))) {
        ++pos;
      }
      continue;
    }
    ++pos;

    while (pos < attrs.size() &&
           std::isspace(static_cast<unsigned char>(attrs[pos]))) {
      ++pos;
    }
    if (pos >= attrs.size()) {
      break;
    }

    std::string_view value;
    if (attrs[pos] == '"' || attrs[pos] == '\'') {
      const char quote = attrs[pos++];
      const std::size_t valueStart = pos;
      while (pos < attrs.size() && attrs[pos] != quote) {
        ++pos;
      }
      value = attrs.substr(valueStart, pos - valueStart);
      if (pos < attrs.size()) {
        ++pos;
      }
    } else {
      const std::size_t valueStart = pos;
      while (pos < attrs.size() &&
             !std::isspace(static_cast<unsigned char>(attrs[pos])) &&
             attrs[pos] != '>') {
        ++pos;
      }
      value = attrs.substr(valueStart, pos - valueStart);
    }

    if (key == name) {
      return std::string(value);
    }
  }
  return "";
}

std::optional<std::size_t> findTagEnd(std::string_view input,
                                      std::size_t start) {
  bool inQuote = false;
  char quote = '\0';
  for (std::size_t i = start; i < input.size(); ++i) {
    const char ch = input[i];
    if (inQuote) {
      if (ch == quote) {
        inQuote = false;
      }
      continue;
    }
    if (ch == '"' || ch == '\'') {
      inQuote = true;
      quote = ch;
      continue;
    }
    if (ch == '>') {
      return i;
    }
  }
  return std::nullopt;
}

std::string collapseXmlTagReferences(
    std::string_view input, std::string_view tagName,
    const std::function<std::string(std::string_view, std::string_view)> &render) {
  const std::string openTag = "<" + std::string(tagName);
  const std::string closeTag = "</" + std::string(tagName) + ">";

  std::string output;
  std::size_t cursor = 0;
  while (cursor < input.size()) {
    const std::size_t start = input.find(openTag, cursor);
    if (start == std::string_view::npos) {
      output.append(input.substr(cursor));
      break;
    }

    output.append(input.substr(cursor, start - cursor));

    const std::size_t nameEnd = start + openTag.size();
    if (nameEnd < input.size()) {
      const char next = input[nameEnd];
      if (!std::isspace(static_cast<unsigned char>(next)) && next != '>') {
        output.append(input.substr(start, openTag.size()));
        cursor = nameEnd;
        continue;
      }
    }

    const auto tagEnd = findTagEnd(input, nameEnd);
    if (!tagEnd.has_value()) {
      output.append(input.substr(start));
      break;
    }

    const std::size_t contentStart = *tagEnd + 1;
    const std::size_t close = input.find(closeTag, contentStart);
    if (close == std::string_view::npos) {
      output.append(input.substr(start));
      break;
    }

    const std::string_view attrs =
        trimAsciiWhitespace(input.substr(nameEnd, *tagEnd - nameEnd));
    const std::string_view fullTag =
        input.substr(start, close + closeTag.size() - start);
    output += render(attrs, fullTag);
    cursor = close + closeTag.size();
  }
  return output;
}

} // namespace

std::string CollapseExpandedReferencesForDisplay(const std::string &text) {
  const std::string collapsedArtifacts = collapseXmlTagReferences(
      text, "artifact", [](std::string_view attrs, std::string_view fullTag) {
        const std::string path = extractXmlAttribute(attrs, "path");
        if (path.empty()) {
          return std::string(fullTag);
        }
        return "@artifact:" + path;
      });

  return collapseXmlTagReferences(
      collapsedArtifacts, "file",
      [](std::string_view attrs, std::string_view fullTag) {
        const std::string path = extractXmlAttribute(attrs, "path");
        if (path.empty()) {
          return std::string(fullTag);
        }
        const std::string lines = extractXmlAttribute(attrs, "lines");
        if (!lines.empty()) {
          return "@" + path + ":" + lines;
        }
        return "@" + path;
      });
}

std::string ClampTranscriptTextForDisplay(const std::string &text) {
  size_t start = 0;
  while (start < text.size() &&
         std::isspace(static_cast<unsigned char>(text[start]))) {
    start++;
  }
  size_t end = text.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    end--;
  }
  return text.substr(start, end - start);
}

static std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> lines;
  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    lines.push_back(line);
  }
  return lines;
}

static std::string trim(const std::string &s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
    start++;
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
    end--;
  return s.substr(start, end - start);
}

static std::string collapseWhitespace(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  bool in_ws = false;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!in_ws) {
        out.push_back(' ');
        in_ws = true;
      }
      continue;
    }
    in_ws = false;
    out.push_back(c);
  }
  if (!out.empty() && out.front() == ' ')
    out.erase(out.begin());
  if (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

// These functions are used by table rendering which is conditionally compiled
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

static bool isTableSeparator(const std::string &line) {
  if (line.find('|') == std::string::npos)
    return false;
  for (char c : line) {
    if (c == '|' || c == '-' || c == ':' ||
        std::isspace(static_cast<unsigned char>(c)))
      continue;
    return false;
  }
  return true;
}

static std::vector<std::string> splitTableRow(const std::string &line) {
  std::vector<std::string> cells;
  std::string cur;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (c == '|') {
      cells.push_back(trim(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty())
    cells.push_back(trim(cur));
  if (!cells.empty() && cells.front().empty())
    cells.erase(cells.begin());
  if (!cells.empty() && cells.back().empty())
    cells.pop_back();
  return cells;
}

// Wrap text into lines that fit within max_width
static std::vector<std::string> wrapTextToWidth(const std::string &text,
                                                int max_width) {
  std::vector<std::string> result;
  if (max_width <= 0)
    max_width = 80;

  std::string current_line;
  std::string current_word;

  auto flush_word = [&]() {
    if (current_word.empty())
      return;
    if (current_line.empty()) {
      current_line = current_word;
    } else if (current_line.size() + 1 + current_word.size() <=
               static_cast<size_t>(max_width)) {
      current_line += " " + current_word;
    } else {
      result.push_back(current_line);
      current_line = current_word;
    }
    current_word.clear();
  };

  auto is_break = [](char c) {
    switch (c) {
    case '/':
    case '\\':
    case '-':
    case '_':
    case '.':
    case ':':
    case '?':
    case '&':
    case '=':
    case '#':
    case '@':
    case '~':
    case '$':
    case '%':
    case '^':
    case '*':
    case '+':
    case '|':
    case '<':
    case '>':
      return true;
    default:
      return false;
    }
  };

  for (char c : text) {
    if (c == '\n') {
      flush_word();
      if (!current_line.empty()) {
        result.push_back(current_line);
        current_line.clear();
      }
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      flush_word();
      continue;
    }
    if (is_break(c)) {
      flush_word();
      result.push_back(current_line);
      current_line.clear();
      continue;
    }
    current_word += c;
    // Force break on very long words
    if (current_word.size() >= static_cast<size_t>(max_width)) {
      result.push_back(current_word);
      current_word.clear();
    }
  }

  flush_word();
  if (!current_line.empty()) {
    result.push_back(current_line);
  }

  return result;
}

static std::vector<std::string> wrapTokens(const std::string &text,
                                           int max_width) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  };

  auto is_break = [](char c) {
    switch (c) {
    case '/':
    case '\\':
    case '-':
    case '_':
    case '.':
    case ':':
    case '?':
    case '&':
    case '=':
    case '#':
    case '@':
    case '~':
      return true;
    default:
      return false;
    }
  };

  for (char c : text) {
    if (c == '\n') {
      flush();
      if (out.empty() || out.back() != " ")
        out.push_back(" ");
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      flush();
      if (out.empty() || out.back() != " ")
        out.push_back(" ");
      continue;
    }
    if (is_break(c)) {
      flush();
      out.push_back(std::string(1, c));
      continue;
    }
    cur.push_back(c);
  }
  flush();

  // Soft-wrap long unbroken tokens based on actual width
  const size_t kChunk = static_cast<size_t>(std::max(8, max_width / 4));
  std::vector<std::string> wrapped;
  for (const auto &t : out) {
    if (t.size() <= kChunk || t == " ") {
      wrapped.push_back(t);
      continue;
    }
    for (size_t i = 0; i < t.size(); i += kChunk) {
      wrapped.push_back(t.substr(i, kChunk));
    }
  }
  return wrapped;
}

static ftxui::Element renderInline(const std::string &text, bool dim) {
  std::string cleaned = collapseWhitespace(text);
  if (cleaned.empty()) {
    return ftxui::text("");
  }

  // Parse inline styles (bold, italic, code) into tokens.
  struct Token {
    std::string text;
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool is_space = false;
  };
  std::vector<Token> tokens;
  Token cur;
  bool bold = false;
  bool italic = false;
  bool code = false;

  auto flush_word = [&] {
    if (!cur.text.empty()) {
      cur.bold = bold;
      cur.italic = italic;
      cur.code = code;
      cur.is_space = false;
      tokens.push_back(cur);
      cur.text.clear();
    }
  };

  for (size_t i = 0; i < cleaned.size(); ++i) {
    if (!code && i + 1 < cleaned.size() && cleaned[i] == '*' &&
        cleaned[i + 1] == '*') {
      flush_word();
      bold = !bold;
      ++i;
      continue;
    }
    if (!code && cleaned[i] == '*') {
      flush_word();
      italic = !italic;
      continue;
    }
    if (cleaned[i] == '`') {
      flush_word();
      code = !code;
      continue;
    }
    if (cleaned[i] == ' ') {
      flush_word();
      if (!tokens.empty() && tokens.back().is_space)
        continue;
      Token space;
      space.text = " ";
      space.bold = bold;
      space.italic = italic;
      space.code = code;
      space.is_space = true;
      tokens.push_back(space);
      continue;
    }
    cur.text.push_back(cleaned[i]);
  }
  flush_word();

  // Build styled elements from tokens.
  ftxui::Elements elems;
  for (const auto &tok : tokens) {
    auto e = ftxui::text(tok.text);
    if (tok.bold)
      e = e | ftxui::bold;
    if (tok.italic)
      e = e | ftxui::dim;
    if (tok.code)
      e = e | ftxui::dim | ftxui::color(ftxui::Color::RGB(100, 180, 160));
    if (dim)
      e = e | ftxui::dim;
    elems.push_back(e);
  }
  if (elems.empty())
    return ftxui::text("");
  
  // Use hbox to flow elements horizontally with wrapping
  return ftxui::hflow(std::move(elems));
}

void ClearMarkdownCache() {
  getMarkdownCache().clear();
  getMarkdownCacheOrder().clear();
  getMarkdownCacheBytes() = 0;
}

ftxui::Element RenderMarkdown(const std::string &text, bool dim) {
  // Calculate effective width from terminal size if global not set
  int term_width = ftxui::Terminal::Size().dimx;
  int effective_width = g_markdown_width > 0
                            ? g_markdown_width
                            : (term_width > 0 ? term_width : 80);

  const bool cacheable = text.size() <= 64 * 1024;
  MarkdownCacheKey key{text, dim, effective_width};
  if (cacheable) {
    auto& cache = getMarkdownCache();
    auto it = cache.find(key);
    if (it != cache.end()) {
      return it->second;
    }
  }

  const std::string displayText = CollapseExpandedReferencesForDisplay(text);

  std::vector<ftxui::Element> out;
  auto lines = splitLines(displayText);
  bool in_code = false;
  std::vector<std::string> code_lines;
  std::string para_buf;

  auto flush_para = [&] {
    if (para_buf.empty())
      return;
    out.push_back(renderInline(para_buf, dim));
    para_buf.clear();
  };

  auto flush_code = [&] {
    if (code_lines.empty())
      return;
    std::vector<ftxui::Element> code_elems;
    // Use smaller wrap for code on narrow terminals
    const size_t kCodeWrap = static_cast<size_t>(std::max(10, effective_width - 4));
    for (const auto &l : code_lines) {
      if (l.size() <= kCodeWrap) {
        auto e = ftxui::text(l);
        if (dim)
          e = e | ftxui::dim;
        code_elems.push_back(e);
        continue;
      }
      for (size_t i = 0; i < l.size(); i += kCodeWrap) {
        auto e = ftxui::text(l.substr(i, kCodeWrap));
        if (dim)
          e = e | ftxui::dim;
        code_elems.push_back(e);
      }
    }
    out.push_back(ftxui::vbox(std::move(code_elems)) |
                  ftxui::bgcolor(ftxui::Color::RGB(36, 38, 45)));
    code_lines.clear();
  };

  for (size_t i = 0; i < lines.size(); ++i) {
    const auto &line = lines[i];
    if (line.rfind("```", 0) == 0) {
      if (in_code) {
        flush_code();
        in_code = false;
      } else {
        flush_para();
        in_code = true;
      }
      continue;
    }

    if (in_code) {
      code_lines.push_back(line);
      continue;
    }

    if (i + 1 < lines.size() && line.find('|') != std::string::npos &&
        isTableSeparator(lines[i + 1])) {
      flush_para();
      auto header = splitTableRow(line);
      std::vector<std::vector<std::string>> raw_rows;
      size_t j = i + 2;
      for (; j < lines.size(); ++j) {
        if (lines[j].find('|') == std::string::npos || lines[j].empty())
          break;
        raw_rows.push_back(splitTableRow(lines[j]));
      }
      i = j - 1;

      // Build Element grid: header row + data rows
      size_t num_cols = header.size();
      if (num_cols == 0)
        num_cols = 1;
      int col_width = (effective_width - 4) / static_cast<int>(num_cols);
      // Ensure at least some minimum width per column
      col_width = std::max(5, col_width);

      std::vector<std::vector<ftxui::Element>> table_data;

      // Header row
      std::vector<ftxui::Element> header_elems;
      for (const auto &cell : header) {
        header_elems.push_back(
            renderInline(cell, dim) | ftxui::bold |
            ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, col_width));
      }
      header_elems.resize(num_cols, ftxui::text(""));
      table_data.push_back(std::move(header_elems));

      // Data rows
      for (const auto &r : raw_rows) {
        std::vector<ftxui::Element> row_elems;
        for (size_t c = 0; c < num_cols; ++c) {
          std::string cell = c < r.size() ? r[c] : "";
          auto elem = renderInline(cell, dim) |
                      ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, col_width);
          row_elems.push_back(std::move(elem));
        }
        table_data.push_back(std::move(row_elems));
      }

      auto table = ftxui::Table(std::move(table_data));
      table.SelectAll().Border(ftxui::LIGHT);
      table.SelectAll().SeparatorHorizontal(ftxui::LIGHT);
      table.SelectAll().SeparatorVertical(ftxui::LIGHT);

      // Decorate header specifically, but only the cells (not the separators)
      table.SelectRow(0).Decorate(ftxui::bold);

      if (dim) {
        table.SelectAll().Decorate(ftxui::dim);
      }

      out.push_back(table.Render() |
                    ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, effective_width - 4));
      continue;
    }

    if (line.empty()) {
      flush_para();
      out.push_back(ftxui::text(""));
      continue;
    }

    if (line.rfind("#", 0) == 0) {
      flush_para();
      std::string title = line;
      while (!title.empty() && title.front() == '#')
        title.erase(title.begin());
      if (!title.empty() && title.front() == ' ')
        title.erase(title.begin());
      auto elem = renderInline(title, dim) | ftxui::bold;
      out.push_back(elem);
      continue;
    }

    if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
      flush_para();
      auto content = line.substr(2);
      out.push_back(ftxui::hbox({
          ftxui::text("• "),
          renderInline(content, dim) | ftxui::flex,
      }));
      continue;
    }

    // For regular text lines, render each line separately to preserve newlines
    // This is different from standard markdown paragraph merging
    flush_para();
    out.push_back(renderInline(line, dim));
  }

  if (in_code) {
    flush_code();
  } else {
    flush_para();
  }

  ftxui::Element result;
  if (out.empty()) {
    result = ftxui::text("");
  } else {
    result = ftxui::vbox(std::move(out));
  }

  if (cacheable) {
    getMarkdownCache()[key] = result;
    rememberMarkdownCacheKey(key);
  }
  return result;
}

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

} // namespace firmius::tui
