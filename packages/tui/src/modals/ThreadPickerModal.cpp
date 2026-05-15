#include "modals/ThreadPickerModal.hpp"
#include "NotificationManager.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "TUIHotkeys.hpp"
#include "components/ScrollableBox.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include "utils/Clipboard.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <time.h>
#endif
namespace {

std::string normalizePath(const std::string &path) {
  if (path.empty())
    return path;
  std::error_code ec;
  std::filesystem::path p(path);
  if (!p.is_absolute()) {
    p = std::filesystem::absolute(p, ec);
  }
  if (!ec) {
    auto canon = std::filesystem::weakly_canonical(p, ec);
    if (!ec) {
      return canon.string();
    }
  }
  return p.string();
}

std::string toLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

std::string flattenText(std::string text) {
  for (auto &ch : text) {
    if (ch == '\n' || ch == '\r' || ch == '\t') {
      ch = ' ';
    } else if (ch == '"') {
      ch = '\'';
    }
  }
  std::string out;
  out.reserve(text.size());
  bool prev_space = false;
  for (char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch))) {
      if (!prev_space) {
        out.push_back(' ');
      }
      prev_space = true;
      continue;
    }
    prev_space = false;
    out.push_back(ch);
  }
  if (!out.empty() && out.front() == ' ')
    out.erase(out.begin());
  if (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

std::vector<std::string> splitTerms(const std::string &query) {
  std::vector<std::string> terms;
  std::string current;
  for (char ch : query) {
    if (std::isspace(static_cast<unsigned char>(ch))) {
      if (!current.empty()) {
        terms.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) {
    terms.push_back(current);
  }
  return terms;
}

int fuzzyScoreSingle(const std::string &text, const std::string &query) {
  if (query.empty()) {
    return 0;
  }

  const auto direct = text.find(query);
  if (direct != std::string::npos) {
    return static_cast<int>(query.size()) * 1000 - static_cast<int>(direct);
  }

  std::size_t textIndex = 0;
  int score = 0;
  int streak = 0;
  for (char queryCh : query) {
    bool matched = false;
    while (textIndex < text.size()) {
      if (text[textIndex] == queryCh) {
        matched = true;
        score += 10 + streak * 6;
        ++streak;
        ++textIndex;
        break;
      }
      streak = 0;
      ++textIndex;
    }
    if (!matched) {
      return -1;
    }
  }
  return score;
}

int fuzzyScoreAllTerms(const std::string &text, const std::string &query) {
  const auto terms = splitTerms(toLower(flattenText(query)));
  if (terms.empty()) {
    return 0;
  }

  int total = 0;
  for (const auto &term : terms) {
    const int score = fuzzyScoreSingle(text, term);
    if (score < 0) {
      return -1;
    }
    total += score;
  }
  return total;
}

std::string truncateText(const std::string &text, int max_len) {
  if (max_len <= 0)
    return "";
  if ((int)text.size() <= max_len)
    return text;
  if (max_len <= 3)
    return text.substr(0, max_len);
  return text.substr(0, max_len - 3) + "...";
}

std::string formatTime(uint64_t timestamp_ms) {
  if (timestamp_ms == 0)
    return "";
  std::time_t t = static_cast<std::time_t>(timestamp_ms / 1000);
  std::tm tm {};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[64];
  if (std::strftime(buf, sizeof(buf), "%I:%M %p", &tm) == 0)
    return "";
  std::string out(buf);
  if (!out.empty() && out.front() == '0') {
    out.erase(out.begin());
  }
  return out;
}

struct ThreadEntry {
  firmius::shared::ThreadMetadata meta;
  int agent_count = 0;
  std::string search_blob;
  bool locked_by_other = false;
  int locked_pid = -1;
};

} // namespace

namespace firmius::tui {

ftxui::Component ThreadPickerModal::create(TuiState &state) {
  [[maybe_unused]] auto &h = firmius::core::Harness::instance();
  auto all_threads_ptr = std::make_shared<std::vector<ThreadEntry>>();
  auto filtered_indices = std::make_shared<std::vector<int>>();
  auto filter_text = std::make_shared<std::string>("");
  auto selected = std::make_shared<int>(0);
  auto list_rows = std::make_shared<std::vector<std::string>>();
  auto list_width = std::make_shared<int>(80);
  auto row_boxes = std::make_shared<std::vector<ftxui::Box>>();
  auto threads_loading = std::make_shared<bool>(false);
  auto visible_start = std::make_shared<int>(0);
  auto visible_count = std::make_shared<int>(10);

  enum class SortMode { LastActive, Title, CreatedAt };
  auto sort_mode = std::make_shared<SortMode>(SortMode::LastActive);
  auto show_all = std::make_shared<bool>(false);

  std::error_code cwd_ec;
  auto cwd_path = std::filesystem::current_path(cwd_ec);
  std::string cwd = cwd_ec ? "" : normalizePath(cwd_path.string());

  auto rebuild_filtered =
      [all_threads_ptr, filtered_indices, filter_text, list_rows, selected]() {
        filtered_indices->clear();
        list_rows->clear();
        std::vector<std::pair<int, int>> matches;
        matches.reserve(all_threads_ptr->size());

        for (int index = 0; index < static_cast<int>(all_threads_ptr->size());
             ++index) {
          const auto &entry = (*all_threads_ptr)[index];
          const std::string title =
              toLower(flattenText(entry.meta.title.empty()
                                      ? "Untitled Thread"
                                      : entry.meta.title));
          const std::string searchText = title + " " + entry.search_blob;
          const int score =
              fuzzyScoreAllTerms(searchText, *filter_text);
          if (score < 0) {
            continue;
          }
          matches.emplace_back(-score, index);
        }

        std::sort(matches.begin(), matches.end());
        for (const auto &[negScore, index] : matches) {
          (void)negScore;
          filtered_indices->push_back(index);
          list_rows->push_back(std::to_string(index));
        }

        if (filtered_indices->empty()) {
          *selected = 0;
        } else {
          *selected = std::clamp(*selected, 0,
                                 static_cast<int>(filtered_indices->size() - 1));
        }
      };

  auto sync_from_snapshot =
      [all_threads_ptr, filtered_indices, selected, rebuild_filtered,
       threads_loading, &state]() {
        const auto snapshot = state.threadListSnapshot();
        std::vector<ThreadEntry> loaded;
        loaded.reserve(snapshot.entries.size());
        for (const auto &row : snapshot.entries) {
          ThreadEntry entry;
          entry.meta = row.metadata;
          entry.agent_count = row.agent_count;
          entry.locked_by_other = row.locked_by_other;
          entry.locked_pid = row.locked_pid;
          entry.search_blob = toLower(flattenText(
              row.metadata.threadId + " " +
              (row.metadata.title.empty() ? "" : row.metadata.title) + " " +
              row.metadata.cwd + " " + row.metadata.leadPersona));
          loaded.push_back(std::move(entry));
        }
        *all_threads_ptr = std::move(loaded);
        *threads_loading = snapshot.loading;
        rebuild_filtered();
        if (*selected >= static_cast<int>(filtered_indices->size())) {
          *selected = filtered_indices->empty()
                          ? 0
                          : static_cast<int>(filtered_indices->size()) - 1;
        }
      };

  sync_from_snapshot();

  auto start_refresh_threads = [cwd, show_all, sync_from_snapshot, &state]() {
        state.refreshThreadListSnapshot(
            cwd, *show_all,
            [sync_from_snapshot]() mutable { sync_from_snapshot(); });
      };

  start_refresh_threads();

  auto listContent =
      ftxui::Renderer([all_threads_ptr, filtered_indices, selected, list_width,
                       row_boxes, visible_start, visible_count]() {
        const auto &theme = ThemeManager::instance().getCurrentTheme();
        const int total = static_cast<int>(filtered_indices->size());
        const int start = std::clamp(*visible_start, 0, std::max(0, total - 1));
        const int count = std::max(1, *visible_count);
        const int end = std::min(total, start + count);
        row_boxes->assign(std::max(0, end - start), ftxui::Box{});

        if (filtered_indices->empty()) {
          return ftxui::vbox({
              ftxui::text("No matching threads") | ftxui::center |
                  ftxui::color(theme.base.dim),
              ftxui::text(""),
              ftxui::text("Search title, thread ID, cwd, or lead persona") |
                  ftxui::center | ftxui::color(theme.base.dim),
          });
        }

        ftxui::Elements rows;
        rows.reserve(static_cast<std::size_t>(std::max(0, end - start) * 2 + 2));
        if (start > 0) {
          rows.push_back(ftxui::text("... " + std::to_string(start) +
                                     " more above") |
                         ftxui::color(theme.base.dim));
          rows.push_back(ftxui::text(""));
        }
        for (int rowIndex = start; rowIndex < end; ++rowIndex) {
          const int visibleIndex = rowIndex - start;
          const auto &entry = (*all_threads_ptr)[(*filtered_indices)[rowIndex]];
          bool isSelected = rowIndex == *selected;

          std::string agent_label = "(" + std::to_string(entry.agent_count) +
                                    (entry.agent_count == 1 ? " agent)"
                                                             : " agents)");
          std::string benchmark_label;
          if (entry.meta.isBenchmarkRun) {
            benchmark_label = "BENCH";
            if (!entry.meta.benchmarkId.empty()) {
              benchmark_label += ":" + entry.meta.benchmarkId;
            }
            if (!entry.meta.benchmarkTaskId.empty()) {
              benchmark_label += "@" +
                                 truncateText(entry.meta.benchmarkTaskId, 24);
            }
          }
          std::string lock_label;
          if (entry.locked_by_other && entry.locked_pid > 0) {
            lock_label = "PID: " + std::to_string(entry.locked_pid);
          }

          int width = std::max(10, *list_width);
          int reserved =
              2 + static_cast<int>(agent_label.size()) +
              (benchmark_label.empty()
                   ? 0
                   : 3 + static_cast<int>(benchmark_label.size())) +
              (lock_label.empty() ? 0 : 2 + static_cast<int>(lock_label.size()));
          int max_title = std::max(8, width - reserved - 2);
          std::string title =
              truncateText(entry.meta.title.empty() ? "Untitled Thread"
                                                   : entry.meta.title,
                           max_title);

          ftxui::Color title_fg =
              entry.locked_by_other
                  ? theme.base.dim
                  : (isSelected ? theme.modals.highlight_fg : theme.modals.fg);
          ftxui::Color sub_fg = theme.base.dim;

          ftxui::Elements top_line_elements;
          top_line_elements.reserve(6);
          top_line_elements.push_back(ftxui::text(title) |
                                      ftxui::color(title_fg) | ftxui::bold);
          top_line_elements.push_back(ftxui::text(" ") | ftxui::color(title_fg));
          top_line_elements.push_back(ftxui::text(agent_label) | ftxui::color(sub_fg));
          if (!benchmark_label.empty()) {
            top_line_elements.push_back(ftxui::text(" ") | ftxui::color(sub_fg));
            top_line_elements.push_back(
                ftxui::text(" " + benchmark_label + " ") | ftxui::bold |
                ftxui::color(theme.modals.highlight_fg) |
                ftxui::bgcolor(theme.modals.highlight_bg));
          }
          auto top_line = ftxui::hbox(std::move(top_line_elements));

          if (!lock_label.empty()) {
            top_line =
                ftxui::hbox({top_line, ftxui::text("  ") | ftxui::color(sub_fg),
                             ftxui::text(lock_label) | ftxui::color(sub_fg)});
          }

          std::string time_label = formatTime(entry.meta.lastActiveAt);
          std::string user_text = !entry.meta.cwd.empty()
                                      ? flattenText(entry.meta.cwd)
                                      : (!entry.meta.leadPersona.empty()
                                             ? ("lead: " + entry.meta.leadPersona)
                                             : entry.meta.threadId);

          // Ensure the right-side time label always has space, even on narrow
          // terminals. Constrain the left preview element width explicitly.
          const int time_width = time_label.empty()
                                     ? 0
                                     : static_cast<int>(time_label.size()) + 1;
          const int min_preview = 14;
          const int gap = 2; // visual space before time
          const int prefix_suffix = 6; // -> "" + a trailing space
          const int preview_reserved = prefix_suffix + gap;
          int max_left_text_width =
              std::max(min_preview, width - time_width - preview_reserved);
          user_text = truncateText(user_text, max_left_text_width);

          auto msg_left =
              ftxui::text("-> \"" + user_text + "\"") | ftxui::color(sub_fg);
          ftxui::Element msg_line;
          if (time_label.empty()) {
            msg_line = msg_left;
          } else {
            msg_line = ftxui::hbox({
                msg_left |
                    ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN,
                               max_left_text_width + prefix_suffix) |
                    ftxui::xflex,
                ftxui::text(std::string(gap, ' ')) | ftxui::color(sub_fg),
                ftxui::text(time_label) | ftxui::color(sub_fg),
            });
          }

          auto id_line =
              ftxui::text("ID: " + entry.meta.threadId) | ftxui::color(sub_fg);
          auto row =
              ftxui::vbox({top_line, msg_line, id_line}) |
              ftxui::reflect(row_boxes->at(visibleIndex));
          if (isSelected) {
            row = row | ftxui::bgcolor(theme.modals.highlight_bg);
          }

          rows.push_back(row);
          rows.push_back(ftxui::text(""));
        }
        if (end < total) {
          rows.push_back(ftxui::text("... " + std::to_string(total - end) +
                                     " more below") |
                         ftxui::color(theme.base.dim));
        }

        return ftxui::vbox(std::move(rows));
      });

  auto scrollable = ScrollableBox(
      listContent, {.startAtBottom = false, .overlayScrollbar = false});
  scrollable->RequestScrollToTop();
  auto modal_renderer =
      ftxui::Renderer(scrollable, [scrollable, list_rows, filtered_indices,
                                   filter_text, all_threads_ptr, selected,
                                   list_width, sort_mode, show_all,
                                   threads_loading, visible_start,
                                   visible_count] {
        const auto &theme = ThemeManager::instance().getCurrentTheme();

        const auto sortLabel = [&]() -> std::string {
          switch (*sort_mode) {
          case SortMode::Title:
            return "Title";
          case SortMode::CreatedAt:
            return "Created";
          case SortMode::LastActive:
          default:
            return "Last Active";
          }
        }();
        const std::string scopeLabel = *show_all ? "All" : "Project";
        auto term = ftxui::Terminal::Size();
        const int term_w = std::max(0, term.dimx);
        const int term_h = std::max(0, term.dimy);

        // Width: scale up on wide terminals, keep margins, avoid overflow.
        int modal_width = std::clamp(term_w - 6, 32, 180);
        // Height: adapt to small-height terminals too.
        int modal_height = std::clamp(term_h - 6, 14, 28);

        int content_width = std::max(20, modal_width - 4);
        *list_width = content_width;

        const int list_max_h = std::max(6, modal_height - 10);
        const int windowRows = std::max(4, list_max_h / 3);
        *visible_count = windowRows;
        if (!filtered_indices->empty()) {
          const int maxStart =
              std::max(0, static_cast<int>(filtered_indices->size()) -
                              windowRows);
          *visible_start = std::clamp(*selected - windowRows / 2, 0, maxStart);
        } else {
          *visible_start = 0;
        }

        if (*threads_loading && all_threads_ptr->empty()) {
          return FlatModalPanel(
              theme, "Select Thread",
              ModalSection(theme,
                           ftxui::vbox({
                               ftxui::text("Loading threads...") |
                                   ftxui::color(theme.modals.fg) |
                                   ftxui::center,
                               ftxui::text(""),
                               ftxui::text("Thread details will appear as soon "
                                           "as the index is ready.") |
                                   ftxui::color(theme.base.dim) |
                                   ftxui::center,
                           }),
                           theme.modals.bg),
              modal_width, 16);
        }

        if (all_threads_ptr->empty()) {
          return FlatModalPanel(
              theme, "Select Thread",
              ModalSection(
                  theme,
                  ftxui::vbox(
                      {ftxui::text("No threads in this directory.") |
                           ftxui::color(theme.base.dim) | ftxui::center,
                       ftxui::text(""),
                       ftxui::text("Press ESC to cancel.") |
                           ftxui::color(theme.base.dim) | ftxui::center}),
                  theme.modals.bg),
              modal_width, 16);
        }

        std::string selected_id;
        if (!filtered_indices->empty() &&
            *selected < static_cast<int>(filtered_indices->size())) {
          selected_id = (*all_threads_ptr)[(*filtered_indices)[*selected]].meta.threadId;
        }

        return FlatModalPanel(
            theme, "Select Thread",
            ModalSection(
                theme,
                ftxui::vbox({
                    ftxui::hbox({
                        ftxui::text("Search: ") | ftxui::color(theme.modals.fg),
                        ftxui::text(*filter_text) | ftxui::underlined |
                            ftxui::color(theme.modals.fg),
                    }),
                    ftxui::text(""),
                    ftxui::hbox({
                        ftxui::text(" " + std::to_string(filtered_indices->size()) +
                                    " matches ") |
                            ftxui::color(theme.base.dim),
                        ftxui::text("  ") | ftxui::color(theme.base.dim),
                        ftxui::text("Sort: " + sortLabel) |
                            ftxui::color(theme.base.dim),
                        ftxui::text("  ") | ftxui::color(theme.base.dim),
                        ftxui::text("Scope: " + scopeLabel) |
                            ftxui::color(theme.base.dim),
                        *threads_loading
                            ? (ftxui::text("  reloading") |
                               ftxui::color(theme.base.dim))
                            : ftxui::text(""),
                    }),
                    ftxui::hbox({
                        ftxui::filler(),
                        ftxui::text(selected_id.empty()
                                        ? ""
                                        : "selected: " + truncateText(selected_id, 28)) |
                            ftxui::color(theme.base.dim),
                    }),
                    ftxui::separatorLight() | ftxui::color(theme.modals.border),
                    scrollable->Render() | ftxui::xflex |
                        ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, list_max_h),
                    ftxui::text(""),
                    ftxui::hbox({
                        ftxui::text("S:sort  A:all  ") |
                            ftxui::color(theme.base.dim),
                        ftxui::filler(),
                        ftxui::text("Enter:switch  Ctrl+Y:copy  ESC:cancel") |
                            ftxui::color(theme.base.dim),
                    }),
                }),
                theme.modals.bg),
            modal_width, modal_height);
      });

  return ftxui::CatchEvent(
      modal_renderer,
      [all_threads_ptr, filtered_indices, filter_text, selected, row_boxes,
       scrollable, rebuild_filtered, &state, start_refresh_threads, sort_mode,
       show_all, visible_start](ftxui::Event event) {
    const auto copySelectedThreadId = [&]() {
      rebuild_filtered();
      if (filtered_indices->empty() ||
          *selected >= static_cast<int>(filtered_indices->size())) {
        return true;
      }
      const auto &entry = (*all_threads_ptr)[(*filtered_indices)[*selected]];
      if (Clipboard::setText(entry.meta.threadId)) {
        NotificationManager::instance().notifySuccess(
            "Copied!", "Thread ID copied to clipboard.");
      } else {
        NotificationManager::instance().notifyError(
            "Copy Failed", "Could not copy thread ID to clipboard.", false);
      }
      return true;
    };

    if (IsPermissionCycleEvent(event)) {
      return copySelectedThreadId();
    }

    // Sorting / filtering toggles.
    // - 's' cycles sort: last-active -> title -> created-at -> last-active
    // - 'a' toggles show-all (across all workspace dirs) vs current dir only
    if (event == ftxui::Event::Character('s') ||
        event == ftxui::Event::Character('S')) {
      if (*sort_mode == SortMode::LastActive) {
        *sort_mode = SortMode::Title;
      } else if (*sort_mode == SortMode::Title) {
        *sort_mode = SortMode::CreatedAt;
      } else {
        *sort_mode = SortMode::LastActive;
      }
      *selected = 0;
      start_refresh_threads();
      scrollable->RequestScrollToTop();
      return true;
    }
    if (event == ftxui::Event::Character('a') ||
        event == ftxui::Event::Character('A')) {
      *show_all = !*show_all;
      *selected = 0;
      start_refresh_threads();
      scrollable->RequestScrollToTop();
      return true;
    }

    if (event == ftxui::Event::Return) {
      rebuild_filtered();
      if (*selected >= 0 && *selected < static_cast<int>(filtered_indices->size())) {
        const auto &entry = (*all_threads_ptr)[(*filtered_indices)[*selected]];
        if (entry.locked_by_other) {
          return true;
        }
        const std::string threadId = entry.meta.threadId;
        state.popModal();
        state.requestThreadOpen(threadId, false, "Opening thread...",
                                "Loading the selected thread from disk.");
        return true;
      }
      state.popModal();
      return true;
    }
    if (event == ftxui::Event::Escape) {
      state.popModal();
      return true;
    }
    if (event == ftxui::Event::ArrowUp || event == ftxui::Event::PageUp ||
        event == ftxui::Event::Home) {
      rebuild_filtered();
      if (!filtered_indices->empty()) {
        if (event == ftxui::Event::Home) {
          *selected = 0;
          scrollable->RequestScrollToTop();
        } else if (event == ftxui::Event::PageUp) {
          *selected = std::max(0, *selected - 6);
        } else {
          *selected = std::max(0, *selected - 1);
        }
      }
      scrollable->OnEvent(event);
      return true;
    }
    if (event == ftxui::Event::ArrowDown || event == ftxui::Event::PageDown ||
        event == ftxui::Event::End) {
      rebuild_filtered();
      if (!filtered_indices->empty()) {
        const int last = static_cast<int>(filtered_indices->size() - 1);
        if (event == ftxui::Event::End) {
          *selected = last;
        } else if (event == ftxui::Event::PageDown) {
          *selected = std::min(last, *selected + 6);
        } else {
          *selected = std::min(last, *selected + 1);
        }
      }
      scrollable->OnEvent(event);
      return true;
    }
    if (event == ftxui::Event::Backspace) {
      if (!filter_text->empty()) {
        filter_text->pop_back();
        *selected = 0;
        rebuild_filtered();
        scrollable->RequestScrollToTop();
      }
      return true;
    }
    if (event.is_character()) {
      *filter_text += event.character();
      *selected = 0;
      rebuild_filtered();
      scrollable->RequestScrollToTop();
      return true;
    }
    if (event.is_mouse()) {
      const auto mouse = event.mouse();
      const bool isDragRelatedLeftMouse =
          mouse.button == ftxui::Mouse::Left ||
          mouse.motion == ftxui::Mouse::Moved ||
          mouse.motion == ftxui::Mouse::Released;
      if (isDragRelatedLeftMouse && scrollable->OnEvent(event)) {
        return true;
      }
      if (mouse.button == ftxui::Mouse::WheelUp && *selected > 0) {
        --(*selected);
      }
      if (mouse.button == ftxui::Mouse::WheelDown && !filtered_indices->empty()) {
        *selected = std::min(
            *selected + 1, static_cast<int>(filtered_indices->size() - 1));
      }
      if (mouse.button == ftxui::Mouse::Left &&
          mouse.motion == ftxui::Mouse::Pressed) {
        for (int rowIndex = 0; rowIndex < static_cast<int>(row_boxes->size());
             ++rowIndex) {
          if (row_boxes->at(rowIndex).Contain(mouse.x, mouse.y)) {
            *selected = *visible_start + rowIndex;
            return true;
          }
        }
      }
      if (isDragRelatedLeftMouse) {
        return false;
      }
      return scrollable->OnEvent(event);
    }
    return false;
  });
}

} // namespace firmius::tui
