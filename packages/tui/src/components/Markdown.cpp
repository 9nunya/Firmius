#include "components/Markdown.hpp"
#include <cctype>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace firmius::tui {

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

static std::vector<std::string> wrapTokens(const std::string &text) {
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
      // Paragraphs are already handled outside; treat newlines as spaces.
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

  // Soft-wrap long unbroken tokens into smaller chunks to avoid overflow.
  const size_t kChunk = 16;
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
  if (text.find('*') == std::string::npos &&
      text.find('`') == std::string::npos) {
    auto e = ftxui::paragraph(text);
    if (dim)
      e = e | ftxui::dim;
    return e;
  }

  struct Span {
    std::string text;
    bool bold = false;
    bool italic = false;
    bool code = false;
  };
  std::vector<Span> spans;
  Span cur;
  bool bold = false;
  bool italic = false;
  bool code = false;

  auto flush = [&] {
    if (!cur.text.empty()) {
      cur.bold = bold;
      cur.italic = italic;
      cur.code = code;
      spans.push_back(cur);
      cur.text.clear();
    }
  };

  for (size_t i = 0; i < text.size(); ++i) {
    if (!code && i + 1 < text.size() && text[i] == '*' && text[i + 1] == '*') {
      flush();
      bold = !bold;
      ++i;
      continue;
    }
    if (!code && text[i] == '*') {
      flush();
      italic = !italic;
      continue;
    }
    if (text[i] == '`') {
      flush();
      code = !code;
      continue;
    }
    cur.text.push_back(text[i]);
  }
  flush();

  ftxui::Elements elems;
  for (const auto &sp : spans) {
    auto tokens = wrapTokens(sp.text);
    for (const auto &tok : tokens) {
      auto e = ftxui::text(tok);
      if (sp.bold)
        e = e | ftxui::bold;
      if (sp.italic)
        e = e | ftxui::dim;
      if (sp.code)
        e = e | ftxui::inverted;
      if (dim)
        e = e | ftxui::dim;
      elems.push_back(e);
    }
  }
  if (elems.empty())
    return ftxui::text("");
  return ftxui::hflow(std::move(elems));
}

ftxui::Element RenderMarkdown(const std::string &text, bool dim) {
  std::vector<ftxui::Element> out;
  auto lines = splitLines(text);
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
    for (const auto &l : code_lines) {
      const size_t kCodeWrap = 120;
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
    out.push_back(ftxui::vbox(std::move(code_elems)) | ftxui::border);
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
      std::vector<std::vector<ftxui::Element>> table_data;

      // Header row
      std::vector<ftxui::Element> header_elems;
      for (const auto& cell : header)
        header_elems.push_back(renderInline(cell, dim) | ftxui::bold);
      header_elems.resize(num_cols, ftxui::text(""));
      table_data.push_back(std::move(header_elems));

      // Data rows
      for (const auto& r : raw_rows) {
        std::vector<ftxui::Element> row_elems;
        for (size_t c = 0; c < num_cols; ++c) {
          std::string cell = c < r.size() ? r[c] : "";
          auto elem = renderInline(cell, dim);
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

      out.push_back(table.Render());
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

    if (!para_buf.empty())
      para_buf += " ";
    para_buf += line;
  }

  if (in_code) {
    flush_code();
  } else {
    flush_para();
  }

  if (out.empty()) {
    return ftxui::text("");
  }
  return ftxui::vbox(std::move(out));
}

} // namespace firmius::tui
