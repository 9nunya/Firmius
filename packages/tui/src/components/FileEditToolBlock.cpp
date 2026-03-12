#include "components/FileEditToolBlock.hpp"
#include "ThemeManager.hpp"
#include "UIState.hpp"
#include "components/LogWindow.hpp"
#include "utils/Icons.hpp"
#include <algorithm>
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>
#include <sstream>

namespace firmius::tui {

// Simple diff hunk structure
struct DiffHunk {
  int old_start = 0;
  int old_count = 0;
  int new_start = 0;
  int new_count = 0;
  std::vector<std::pair<char, std::string>> lines; // +, -, or ' ' (context)
};

// Smart hunk ranking - prioritize hunks with more changes and context
static std::vector<size_t>
rankHunksByRelevance(const std::vector<DiffHunk> &hunks) {
  std::vector<std::pair<size_t, int>> scored;

  for (size_t i = 0; i < hunks.size(); i++) {
    const auto &hunk = hunks[i];
    int score = 0;

    // More changes = more relevant
    score += (hunk.old_count + hunk.new_count) * 2;

    // Hunks at the beginning or end of file are often less relevant
    if (hunk.old_start > 1 && i < hunks.size() - 1) {
      score += 5; // Middle hunks are more relevant
    }

    // Hunks with both additions and deletions are more significant
    if (hunk.old_count > 0 && hunk.new_count > 0) {
      score += 10;
    }

    scored.push_back({i, score});
  }

  // Sort by score descending
  std::sort(scored.begin(), scored.end(),
            [](const auto &a, const auto &b) { return a.second > b.second; });

  std::vector<size_t> result;
  for (const auto &[idx, score] : scored) {
    (void)score;
    result.push_back(idx);
  }

  return result;
}

// Parse unified diff format or generate from old/new strings
static std::vector<DiffHunk> parseDiff(const std::string & /*path*/,
                                       const std::string &old_str,
                                       const std::string &new_str) {
  std::vector<DiffHunk> hunks;

  if (old_str.empty() && new_str.empty())
    return hunks;

  // Simple line-by-line diff
  std::vector<std::string> old_lines, new_lines;
  {
    std::istringstream ss(old_str);
    std::string line;
    while (std::getline(ss, line))
      old_lines.push_back(line);
  }
  {
    std::istringstream ss(new_str);
    std::string line;
    while (std::getline(ss, line))
      new_lines.push_back(line);
  }

  // Simple LCS-based diff
  size_t old_idx = 0, new_idx = 0;
  DiffHunk current_hunk;
  bool in_hunk = false;

  while (old_idx < old_lines.size() || new_idx < new_lines.size()) {
    if (old_idx < old_lines.size() && new_idx < new_lines.size() &&
        old_lines[old_idx] == new_lines[new_idx]) {
      // Context line
      if (in_hunk) {
        current_hunk.lines.push_back({' ', old_lines[old_idx]});
        current_hunk.old_count++;
        current_hunk.new_count++;
      }
      old_idx++;
      new_idx++;
    } else {
      // Change detected
      if (!in_hunk) {
        current_hunk = DiffHunk();
        current_hunk.old_start = old_idx + 1;
        current_hunk.new_start = new_idx + 1;
        in_hunk = true;
      }

      // Remove lines from old
      while (old_idx < old_lines.size() &&
             (new_idx >= new_lines.size() ||
              old_lines[old_idx] != new_lines[new_idx])) {
        current_hunk.lines.push_back({'-', old_lines[old_idx]});
        current_hunk.old_count++;
        old_idx++;
      }

      // Add lines from new
      while (new_idx < new_lines.size() &&
             (old_idx >= old_lines.size() ||
              old_lines[old_idx] != new_lines[new_idx])) {
        current_hunk.lines.push_back({'+', new_lines[new_idx]});
        current_hunk.new_count++;
        new_idx++;
      }

      hunks.push_back(current_hunk);
      in_hunk = false;
    }
  }

  return hunks;
}

// Render a single diff hunk
static ftxui::Element renderDiffHunk(const DiffHunk &hunk,
                                     bool compact = false) {
  ftxui::Elements elements;
  const auto &theme = ThemeManager::instance().getCurrentTheme();

  int line_num = hunk.old_start;
  int max_lines = compact ? 15 : INT32_MAX;
  int shown = 0;

  for (const auto &[type, content] : hunk.lines) {
    if (compact && shown >= max_lines) {
      elements.push_back(ftxui::text("  …") | ftxui::color(theme.base.dim));
      break;
    }

    ftxui::Element line_el;
    std::string line_num_str = std::to_string(line_num);
    while (line_num_str.size() < 4)
      line_num_str = " " + line_num_str;

    switch (type) {
    case '-':
      line_el = ftxui::hbox(
          {ftxui::text(line_num_str + " ") | ftxui::color(theme.base.dim),
           ftxui::text("− ") | ftxui::color(theme.status_bar.error.normal.bg),
           ftxui::text(content) |
               ftxui::color(theme.status_bar.error.normal.bg)});
      line_num++;
      break;
    case '+':
      line_el = ftxui::hbox(
          {ftxui::text(line_num_str + " ") | ftxui::color(theme.base.dim),
           ftxui::text("+ ") | ftxui::color(theme.syntax.string),
           ftxui::text(content) | ftxui::color(theme.syntax.string)});
      break;
    case ' ':
      line_el = ftxui::hbox(
          {ftxui::text(line_num_str + " ") | ftxui::color(theme.base.dim),
           ftxui::text("  ") | ftxui::color(theme.base.dim),
           ftxui::text(content) |
               ftxui::color(theme.tool_blocks.specific.file_edit.fg)});
      line_num++;
      break;
    }

    elements.push_back(line_el);
    shown++;
  }

  return ftxui::vbox(elements);
}

ftxui::Component FileEditToolBlock(const std::shared_ptr<ToolCallView> &view) {
  auto opt = ftxui::ButtonOption::Simple();
  opt.transform = [](const ftxui::EntryState &s) {
    auto e = ftxui::text(s.label) | ftxui::dim;
    if (s.focused)
      e = e | ftxui::underlined;
    return e;
  };
  if (view) {
    opt.label = &view->toggle_label;
  } else {
    opt.label = "show";
  }
  opt.on_click = [view] {
    if (!view)
      return;
    view->show_result = !view->show_result;
  };

  auto toggle = ftxui::Button(opt);
  auto container = ftxui::Container::Horizontal({toggle});

  return ftxui::Renderer(container, [view, toggle] {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    if (!view)
      return ftxui::text("File edit call") | ftxui::color(theme.base.dim);

    // Parse args
    std::string path_arg;
    std::string old_string;
    std::string new_string;
    bool is_overwrite = false;
    if (!view->args.empty()) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        if (doc.HasMember("path") && doc["path"].IsString())
          path_arg = doc["path"].GetString();
        if (doc.HasMember("old_string") && doc["old_string"].IsString())
          old_string = doc["old_string"].GetString();
        if (doc.HasMember("new_string") && doc["new_string"].IsString())
          new_string = doc["new_string"].GetString();
        if (doc.HasMember("content") && doc["content"].IsString()) {
          is_overwrite = true;
          new_string = doc["content"].GetString();
        }
      }
    }

    // Get filename from path
    std::string filename = path_arg;
    auto pos = path_arg.find_last_of('/');
    if (pos != std::string::npos && pos + 1 < path_arg.size())
      filename = path_arg.substr(pos + 1);

    if (filename.size() > 50) {
      filename = "…" + filename.substr(filename.size() - 48);
    }

    // ── Preparing / Called ──
    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {
      std::string action = is_overwrite ? "Writing" : "Editing";

      using namespace firmius::shared;
      auto header = ftxui::hbox(
          {ftxui::text(" " + ICON_GEAR + " ") |
               ftxui::color(theme.tool_blocks.generic_icon),
           ftxui::text(action + " ") | ftxui::bold |
               ftxui::color(theme.tool_blocks.generic_title),
           ftxui::text(filename) |
               ftxui::color(theme.tool_blocks.specific.file_edit.fg)});

      // Show diff preview during Called if we have content
      if (view->phase == ToolPhase::Called &&
          (!old_string.empty() || !new_string.empty())) {
        auto hunks = parseDiff(path_arg, old_string, new_string);

        ftxui::Elements diff_elements;
        int total_added = 0, total_removed = 0;

        for (const auto &hunk : hunks) {
          diff_elements.push_back(renderDiffHunk(hunk, true));
          total_added += hunk.new_count;
          total_removed += hunk.old_count;
        }

        if (diff_elements.empty() && is_overwrite) {
          // For overwrite, show new content preview
          std::istringstream ss(new_string);
          std::string line;
          int line_num = 1;
          int shown = 0;
          while (std::getline(ss, line) && shown < 10) {
            std::string ln = std::to_string(line_num);
            while (ln.size() < 4)
              ln = " " + ln;
            diff_elements.push_back(ftxui::hbox(
                {ftxui::text(ln + " ") | ftxui::color(theme.base.dim),
                 ftxui::text("│ ") |
                     ftxui::color(theme.tool_blocks.generic_border),
                 ftxui::text(line) |
                     ftxui::color(theme.tool_blocks.specific.file_edit.fg)}));
            line_num++;
            shown++;
          }
          total_added = line_num - 1;
        }

        auto stats = ftxui::text(" ");
        if (total_added > 0 || total_removed > 0) {
          ftxui::Elements stats_parts;
          if (total_added > 0)
            stats_parts.push_back(
                ftxui::text("+" + std::to_string(total_added)) |
                ftxui::color(theme.syntax.string));
          if (total_removed > 0)
            stats_parts.push_back(
                ftxui::text("−" + std::to_string(total_removed)) |
                ftxui::color(theme.status_bar.error.normal.bg));
          stats = ftxui::hbox(stats_parts) | ftxui::color(theme.base.dim);
        }

        return ftxui::vbox(
                   {header,
                    ftxui::separatorLight() |
                        ftxui::color(theme.tool_blocks.generic_border),
                    ftxui::vbox(diff_elements) | ftxui::frame |
                        ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 12),
                    ftxui::hbox({stats, ftxui::filler(),
                                 ftxui::text(filename) |
                                     ftxui::color(theme.base.dim)})}) |
               ftxui::borderRounded |
               ftxui::color(theme.tool_blocks.generic_border) |
               ftxui::bgcolor(theme.tool_blocks.generic_bg);
      }

      return header;
    }

    // ── Finished + error ──
    if (!view->success) {
      std::string err_msg = view->result;
      if (err_msg.empty())
        err_msg = "unknown error";
      if (err_msg.size() > 60)
        err_msg = err_msg.substr(0, 57) + "…";

      using namespace firmius::shared;
      return ftxui::hbox({ftxui::text(" " + ICON_ERROR + " ") |
                              ftxui::color(theme.status_bar.error.normal.bg),
                          ftxui::text("Edit failed: " + err_msg) |
                              ftxui::color(theme.status_bar.error.normal.bg)}) |
             ftxui::borderRounded |
             ftxui::color(theme.status_bar.error.normal.bg) |
             ftxui::bgcolor(theme.tool_blocks.generic_bg);
    }

    // ── Finished + success ──
    view->toggle_label = view->show_result ? "hide" : "show diff";

    // Count lines for stats
    int removed = 0, added = 0;
    {
      std::istringstream ss(old_string);
      std::string line;
      while (std::getline(ss, line))
        removed++;
    }
    {
      std::istringstream ss(new_string);
      std::string line;
      while (std::getline(ss, line))
        added++;
    }

    ftxui::Elements rows;

    using namespace firmius::shared;
    // Header with stats
    rows.push_back(ftxui::hbox({
        ftxui::text(" " + ICON_FILE_EDIT + " ") |
            ftxui::color(theme.tool_blocks.generic_icon),
        ftxui::text(filename + " ") | ftxui::bold |
            ftxui::color(theme.tool_blocks.generic_title),
        ftxui::text(" [") | ftxui::color(theme.base.dim),
        toggle->Render(),
        ftxui::text("]") | ftxui::color(theme.base.dim),
        ftxui::filler(),
        removed > 0 ? (ftxui::text("−" + std::to_string(removed)) |
                       ftxui::color(theme.status_bar.error.normal.bg))
                    : ftxui::text(""),
        added > 0 ? (ftxui::text(" +" + std::to_string(added)) |
                     ftxui::color(theme.syntax.string))
                  : ftxui::text(""),
    }));

    if (view->show_result && (!old_string.empty() || !new_string.empty())) {
      auto hunks = parseDiff(path_arg, old_string, new_string);

      // Smart hunk optimization: show most relevant hunks when collapsed
      bool expanded = UIState::instance().diffsExpanded;
      size_t maxHunks =
          expanded ? hunks.size() : 3; // Show top 3 hunks when collapsed
      size_t maxLines =
          expanded ? SIZE_MAX
                   : static_cast<size_t>(UIState::instance().maxCollapsedLines);

      ftxui::Elements diff_elements;

      if (hunks.size() > maxHunks && !expanded) {
        // Show only most relevant hunks
        auto ranked = rankHunksByRelevance(hunks);
        size_t shownHunks = 0;
        size_t totalLines = 0;

        for (size_t ri = 0; ri < ranked.size() && shownHunks < maxHunks; ri++) {
          size_t hunkIdx = ranked[ri];
          const auto &hunk = hunks[hunkIdx];

          // Check if adding this hunk would exceed line limit
          size_t hunkLines = hunk.lines.size();
          if (totalLines + hunkLines > maxLines && shownHunks > 0) {
            break; // Don't show partial hunks
          }

          diff_elements.push_back(renderDiffHunk(hunk, !expanded));
          totalLines += hunkLines;
          shownHunks++;
        }

        // Add indicator for hidden hunks
        if (hunks.size() > shownHunks) {
          size_t hiddenCount = hunks.size() - shownHunks;
          diff_elements.insert(
              diff_elements.begin(),
              ftxui::text("  … " + std::to_string(hiddenCount) +
                          " hunks hidden (press Ctrl+G to expand)") |
                  ftxui::color(theme.base.dim));
        }
      } else {
        // Show all hunks
        for (const auto &hunk : hunks) {
          diff_elements.push_back(renderDiffHunk(hunk, !expanded));
        }
      }

      // For overwrite mode with no old content
      if (diff_elements.empty() && is_overwrite) {
        std::istringstream ss(new_string);
        std::string line;
        int line_num = 1;
        size_t shown = 0;
        size_t lineLimit =
            expanded
                ? SIZE_MAX
                : static_cast<size_t>(UIState::instance().maxCollapsedLines);

        while (std::getline(ss, line) && shown < lineLimit) {
          std::string ln = std::to_string(line_num);
          while (ln.size() < 4)
            ln = " " + ln;
          diff_elements.push_back(ftxui::hbox(
              {ftxui::text(ln + " ") | ftxui::color(theme.base.dim),
               ftxui::text("│ ") |
                   ftxui::color(theme.tool_blocks.generic_border),
               ftxui::text(line) |
                   ftxui::color(theme.tool_blocks.specific.file_edit.fg)}));
          line_num++;
          shown++;
        }

        if (!expanded &&
            line_num <= static_cast<int>(std::count(new_string.begin(),
                                                    new_string.end(), '\n')) +
                            1) {
          diff_elements.push_back(
              ftxui::text("  … Content truncated (press Ctrl+G to expand)") |
              ftxui::color(theme.base.dim));
        }
      }

      rows.push_back(ftxui::separatorLight() |
                     ftxui::color(theme.tool_blocks.generic_border));
      rows.push_back(ftxui::vbox(diff_elements) | ftxui::frame);
    }

    return ftxui::vbox(rows) | ftxui::borderRounded |
           ftxui::color(theme.tool_blocks.generic_border) |
           ftxui::bgcolor(theme.tool_blocks.generic_bg);
  });
}

} // namespace firmius::tui
