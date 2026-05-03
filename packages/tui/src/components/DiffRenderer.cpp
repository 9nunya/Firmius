#include "components/DiffRenderer.hpp"

#include "components/SyntaxHighlighter.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace firmius::tui {

namespace {

template <typename T> void HashCombine(std::size_t &seed, const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct DiffCacheKey {
  std::size_t content_hash = 0;
  std::size_t line_count = 0;
  std::size_t byte_count = 0;
  bool expanded = false;

  bool operator==(const DiffCacheKey &other) const {
    return content_hash == other.content_hash && line_count == other.line_count &&
           byte_count == other.byte_count && expanded == other.expanded;
  }
};

struct DiffCacheHasher {
  std::size_t operator()(const DiffCacheKey &key) const {
    std::size_t seed = key.content_hash;
    HashCombine(seed, key.line_count);
    HashCombine(seed, key.byte_count);
    HashCombine(seed, key.expanded);
    return seed;
  }
};

using DiffRenderCache =
    std::unordered_map<DiffCacheKey, ftxui::Element, DiffCacheHasher>;

DiffRenderCache &diffRenderCache() {
  static thread_local DiffRenderCache cache;
  return cache;
}

std::vector<DiffCacheKey> &diffRenderCacheOrder() {
  static thread_local std::vector<DiffCacheKey> order;
  return order;
}

std::size_t &diffRenderCacheBytes() {
  static thread_local std::size_t bytes = 0;
  return bytes;
}

DiffCacheKey BuildDiffCacheKey(const ToolPresentation &presentation, bool expanded) {
  DiffCacheKey key;
  key.expanded = expanded;
  HashCombine(key.content_hash, presentation.diff_source_name);
  for (const auto &section : presentation.diff_sections) {
    HashCombine(key.content_hash, section.title);
    HashCombine(key.content_hash, section.meta);
    if (section.error_text) {
      HashCombine(key.content_hash, *section.error_text);
      key.byte_count += section.error_text->size();
    }
    for (const auto &line : section.lines) {
      HashCombine(key.content_hash, line.type);
      HashCombine(key.content_hash, line.old_line);
      HashCombine(key.content_hash, line.new_line);
      HashCombine(key.content_hash, line.highlight_background);
      HashCombine(key.content_hash, line.content);
      key.byte_count += line.content.size();
      ++key.line_count;
    }
  }
  return key;
}

void RememberDiffCacheKey(const DiffCacheKey &key) {
  constexpr std::size_t kMaxEntries = 256;
  constexpr std::size_t kMaxBytes = 16 * 1024 * 1024;
  constexpr std::size_t kTrimToEntries = 192;
  constexpr std::size_t kTrimToBytes = 12 * 1024 * 1024;
  auto &order = diffRenderCacheOrder();
  auto &bytes = diffRenderCacheBytes();
  order.push_back(key);
  bytes += key.byte_count;
  if (order.size() <= kMaxEntries && bytes <= kMaxBytes) {
    return;
  }
  auto &cache = diffRenderCache();
  std::size_t drop_count = 0;
  while (drop_count < order.size() &&
         (order.size() - drop_count > kTrimToEntries || bytes > kTrimToBytes)) {
    const auto &old = order[drop_count];
    if (cache.erase(old) > 0) {
      bytes = old.byte_count > bytes ? 0 : bytes - old.byte_count;
    }
    ++drop_count;
  }
  order.erase(order.begin(), order.begin() +
                            static_cast<std::ptrdiff_t>(drop_count));
}

ftxui::Element RenderHighlightedContent(const std::string &content,
                                        const std::string &language,
                                        const Theme &theme) {
  if (!language.empty() &&
      SyntaxHighlighter::instance().hasGrammar(language)) {
    return SyntaxHighlighter::instance().highlightRenderWrappedLine(content,
                                                                    language) |
           ftxui::xflex;
  }
  return ftxui::paragraph(content.empty() ? " " : content) |
         ftxui::color(theme.base.fg) | ftxui::xflex;
}

ftxui::Color PrefixColor(const ToolPresentationDiffLine &line,
                         const Theme &theme) {
  if (line.type == '+') {
    return ftxui::Color::Green;
  }
  if (line.type == '-') {
    return theme.status_bar.error.normal.fg;
  }
  return theme.base.dim;
}

std::string FormatLinePrefix(const ToolPresentationDiffLine &line, size_t width) {
  const int line_number = line.type == '-' ? line.old_line : line.new_line;
  std::string prefix(1, line.type == '\0' ? ' ' : line.type);
  prefix += " ";
  if (line_number > 0) {
    auto number = std::to_string(line_number);
    if (number.size() < width) {
      prefix.append(width - number.size(), ' ');
    }
    prefix += number;
  } else {
    prefix.append(width, ' ');
  }
  prefix += " ";
  return prefix;
}

size_t MaxLineNumberWidth(const ToolPresentation &presentation) {
  size_t width = 1;
  for (const auto &section : presentation.diff_sections) {
    for (const auto &line : section.lines) {
      const int line_number = line.type == '-' ? line.old_line : line.new_line;
      if (line_number > 0) {
        width = std::max(width, std::to_string(line_number).size());
      }
    }
  }
  return width;
}

} // namespace

ftxui::Element RenderToolPresentationDiffs(const ToolPresentation &presentation,
                                           const Theme &theme, bool expanded) {
  if (presentation.diff_sections.empty()) {
    return ftxui::emptyElement();
  }
  const DiffCacheKey cache_key = BuildDiffCacheKey(presentation, expanded);
  if (cache_key.byte_count <= 2 * 1024 * 1024) {
    auto &cache = diffRenderCache();
    auto it = cache.find(cache_key);
    if (it != cache.end()) {
      return it->second;
    }
  }

  const std::string language =
      presentation.diff_source_name.empty()
          ? std::string{}
          : SyntaxHighlighter::instance().detectLanguage(
                presentation.diff_source_name);
  const size_t line_number_width = MaxLineNumberWidth(presentation);
  const auto full_add_bg = ftxui::Color::RGB(18, 72, 40);

  ftxui::Elements rows;
  for (size_t i = 0; i < presentation.diff_sections.size(); ++i) {
    const auto &section = presentation.diff_sections[i];
    if (i > 0) {
      rows.push_back(ftxui::separatorLight() |
                     ftxui::color(theme.base.separator));
    }
    if (!section.title.empty()) {
      rows.push_back(ftxui::text(section.title) | ftxui::bold |
                     ftxui::color(theme.tool_blocks.specific.file_edit.fg));
    }
    if (!section.meta.empty()) {
      rows.push_back(ftxui::paragraph(section.meta) |
                     ftxui::color(theme.base.dim));
    }
    if (section.error_text.has_value() && !section.error_text->empty()) {
      rows.push_back(ftxui::paragraph(*section.error_text) |
                     ftxui::color(theme.status_bar.error.normal.fg));
    }
    if (section.lines.empty()) {
      rows.push_back(
          ftxui::text(section.empty_state_text.value_or("(no textual changes)")) |
                     ftxui::color(theme.base.dim));
      continue;
    }

    for (const auto &line : section.lines) {
      const auto bg =
          line.highlight_background ? full_add_bg : theme.tool_blocks.generic_bg;
      auto content =
          RenderHighlightedContent(line.content, language, theme) | ftxui::xflex;
      rows.push_back(ftxui::hbox({
                         ftxui::text(FormatLinePrefix(line, line_number_width)) |
                             ftxui::color(PrefixColor(line, theme)),
                         std::move(content),
                     }) |
                     ftxui::bgcolor(bg) | ftxui::xflex);
    }
  }

  auto rendered = ftxui::vbox(std::move(rows)) |
                  ftxui::bgcolor(theme.tool_blocks.generic_header_bg) | ftxui::xflex;
  if (cache_key.byte_count <= 2 * 1024 * 1024) {
    diffRenderCache()[cache_key] = rendered;
    RememberDiffCacheKey(cache_key);
  }
  return rendered;
}

void ClearToolPresentationDiffCache() {
  diffRenderCache().clear();
  diffRenderCacheOrder().clear();
  diffRenderCacheBytes() = 0;
}

} // namespace firmius::tui
