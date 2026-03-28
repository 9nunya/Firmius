#include "modals/ThreadPickerModal.hpp"
#include "NotificationManager.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "TUIHotkeys.hpp"
#include "components/ScrollableBox.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/Clipboard.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

const char *kFirmiusDir = ".firmius";
const char *kThreadsDir = "threads";
const char *kLockFile = ".lock";

std::filesystem::path getThreadsBase() {
  const char *home = std::getenv("HOME");
  if (!home || std::string(home).empty()) {
    home = "/tmp";
  }
  return std::filesystem::path(home) / kFirmiusDir / kThreadsDir;
}

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
  localtime_r(&t, &tm);
  char buf[64];
  if (std::strftime(buf, sizeof(buf), "%I:%M %p", &tm) == 0)
    return "";
  std::string out(buf);
  if (!out.empty() && out.front() == '0') {
    out.erase(out.begin());
  }
  return out;
}

struct LastUserMessage {
  std::string text;
  uint64_t timestamp = 0;
  bool found = false;
};

std::string extractMessageText(const firmius::shared::Message &msg,
                               bool includeThinking = true) {
  std::string out;
  for (const auto &part : msg.content) {
    if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
      if (!out.empty())
        out.push_back(' ');
      out += txt->text;
    } else if (includeThinking) {
      if (auto *thinking =
              std::get_if<firmius::shared::ThinkingContent>(&part)) {
        if (!out.empty())
          out.push_back(' ');
        out += thinking->thinking;
      }
    } else if (std::holds_alternative<firmius::shared::ImageContent>(part)) {
      if (!out.empty())
        out.push_back(' ');
      out += "[image]";
    }
  }
  return out;
}

struct ThreadSearchData {
  LastUserMessage lastUser;
  std::string searchBlob;
};

ThreadSearchData inspectThreadTranscript(firmius::core::ThreadManager &tm,
                                         const std::string &thread_id) {
  ThreadSearchData data;
  std::vector<std::string> searchChunks;
  searchChunks.push_back(thread_id);

  for (const auto &agentId : tm.listAgents(thread_id)) {
    try {
      auto history = tm.loadAgentHistory(thread_id, agentId);
      for (const auto &turn : history.turns) {
        for (const auto &msg : turn.messages) {
          const std::string text = extractMessageText(msg);
          if (!text.empty()) {
            searchChunks.push_back(text);
          }
          if (msg.role == firmius::shared::Role::User &&
              msg.timestamp >= data.lastUser.timestamp) {
            data.lastUser.timestamp = msg.timestamp;
            data.lastUser.text = text;
            data.lastUser.found = true;
          }
        }
      }
    } catch (...) {
    }
  }

  std::string flattened;
  for (const auto &chunk : searchChunks) {
    if (chunk.empty()) {
      continue;
    }
    if (!flattened.empty()) {
      flattened.push_back(' ');
    }
    flattened += chunk;
  }
  data.searchBlob = toLower(flattenText(flattened));
  return data;
}

int readLockPid(const std::filesystem::path &lock_path) {
  std::ifstream lf(lock_path);
  if (!lf.is_open())
    return -1;
  int pid = -1;
  lf >> pid;
  return pid;
}

std::optional<int> lockedPidByOther(const std::string &thread_id) {
  std::filesystem::path lock_path = getThreadsBase() / thread_id / kLockFile;
  std::error_code ec;
  if (!std::filesystem::exists(lock_path, ec))
    return std::nullopt;

  int fd = open(lock_path.c_str(), O_RDWR);
  if (fd < 0)
    return std::nullopt;

  if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
    flock(fd, LOCK_UN);
    close(fd);
    return std::nullopt;
  }

  if (errno != EWOULDBLOCK) {
    close(fd);
    return std::nullopt;
  }

  int pid = readLockPid(lock_path);
  close(fd);
  if (pid <= 0)
    return std::nullopt;
  return pid;
}

struct ThreadEntry {
  firmius::shared::ThreadMetadata meta;
  int agent_count = 0;
  std::string last_user_text;
  uint64_t last_user_timestamp = 0;
  std::string search_blob;
  bool locked_by_other = false;
  int locked_pid = -1;
};

} // namespace

namespace firmius::tui {

ftxui::Component ThreadPickerModal::create(TuiState &state) {
  auto &h = firmius::core::Harness::instance();
  auto all_threads_ptr = std::make_shared<std::vector<ThreadEntry>>();
  auto filtered_indices = std::make_shared<std::vector<int>>();
  auto filter_text = std::make_shared<std::string>("");
  auto selected = std::make_shared<int>(0);
  auto list_rows = std::make_shared<std::vector<std::string>>();
  auto list_width = std::make_shared<int>(80);
  auto row_boxes = std::make_shared<std::vector<ftxui::Box>>();

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

  auto refresh_threads =
      [all_threads_ptr, filtered_indices, selected, &h, cwd, rebuild_filtered] {
    auto all_threads = h.listThreads();
    all_threads_ptr->clear();
    firmius::core::ThreadManager tm(
        firmius::core::ThreadManager::defaultBasePath());

    for (const auto &t : all_threads) {
      if (!cwd.empty() && normalizePath(t.cwd) != cwd && !t.isBenchmarkRun) {
        continue;
      }

      ThreadEntry entry;
      entry.meta = t;
      entry.agent_count = static_cast<int>(h.listAgents(t.threadId).size());

      auto transcript = inspectThreadTranscript(tm, t.threadId);
      if (transcript.lastUser.found) {
        entry.last_user_text = transcript.lastUser.text;
        entry.last_user_timestamp = transcript.lastUser.timestamp;
      }
      entry.search_blob =
          toLower(flattenText(t.threadId + " " +
                              (t.title.empty() ? "" : t.title) + " " +
                              transcript.searchBlob));

      auto locked_pid = lockedPidByOther(t.threadId);
      if (locked_pid.has_value()) {
        entry.locked_by_other = true;
        entry.locked_pid = *locked_pid;
      }

      all_threads_ptr->push_back(entry);
    }

    rebuild_filtered();
    if (*selected >= static_cast<int>(filtered_indices->size())) {
      *selected = filtered_indices->empty()
                      ? 0
                      : static_cast<int>(filtered_indices->size()) - 1;
    }
  };

  // Initial load
  refresh_threads();

  auto listContent =
      ftxui::Renderer([all_threads_ptr, filtered_indices, selected, list_width,
                       row_boxes]() {
        const auto &theme = ThemeManager::instance().getCurrentTheme();
        row_boxes->assign(filtered_indices->size(), ftxui::Box{});

        if (filtered_indices->empty()) {
          return ftxui::vbox({
              ftxui::text("No matching threads") | ftxui::center |
                  ftxui::color(theme.base.dim),
              ftxui::text(""),
              ftxui::text("Search title, thread ID, user text, or agent output") |
                  ftxui::center | ftxui::color(theme.base.dim),
          });
        }

        ftxui::Elements rows;
        rows.reserve(filtered_indices->size() * 2);
        for (int rowIndex = 0;
             rowIndex < static_cast<int>(filtered_indices->size()); ++rowIndex) {
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

          ftxui::Elements top_line_elements = {
              ftxui::text(title) | ftxui::color(title_fg) | ftxui::bold,
              ftxui::text(" ") | ftxui::color(title_fg),
              ftxui::text(agent_label) | ftxui::color(sub_fg),
          };
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

          std::string time_label = formatTime(entry.last_user_timestamp);
          std::string user_text =
              entry.last_user_text.empty() ? "No user messages yet"
                                           : flattenText(entry.last_user_text);

          int time_width = time_label.empty() ? 0 : static_cast<int>(time_label.size()) + 1;
          int max_msg = std::max(8, width - time_width - 8);
          user_text = truncateText(user_text, max_msg);

          auto msg_left =
              ftxui::text("-> \"" + user_text + "\"") | ftxui::color(sub_fg);
          ftxui::Element msg_line;
          if (time_label.empty()) {
            msg_line = msg_left;
          } else {
            msg_line =
                ftxui::hbox({msg_left, ftxui::filler(),
                             ftxui::text(time_label) | ftxui::color(sub_fg)});
          }

          auto id_line =
              ftxui::text("ID: " + entry.meta.threadId) | ftxui::color(sub_fg);
          auto row =
              ftxui::vbox({top_line, msg_line, id_line}) |
              ftxui::reflect(row_boxes->at(rowIndex));
          if (isSelected) {
            row = row | ftxui::bgcolor(theme.modals.highlight_bg);
          }

          rows.push_back(row);
          rows.push_back(ftxui::text(""));
        }

        return ftxui::vbox(std::move(rows));
      });

  auto scrollable = ScrollableBox(
      listContent, {.startAtBottom = false, .overlayScrollbar = true});
  scrollable->RequestScrollToTop();

  auto modal_renderer =
      ftxui::Renderer(scrollable, [scrollable, list_rows, filtered_indices,
                                   filter_text, all_threads_ptr, selected,
                                   list_width] {
        const auto &theme = ThemeManager::instance().getCurrentTheme();
        auto term = ftxui::Terminal::Size();
        int modal_width = std::min(100, std::max(20, term.dimx - 6));
        if (modal_width > term.dimx) {
          modal_width = term.dimx;
        }
        int content_width = std::max(10, modal_width - 4);
        if (content_width > modal_width) {
          content_width = modal_width;
        }
        *list_width = content_width;

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
                ftxui::vbox(
                    {ftxui::hbox({
                         ftxui::text("Search: ") | ftxui::color(theme.modals.fg),
                         ftxui::text(*filter_text) | ftxui::underlined |
                             ftxui::color(theme.modals.fg),
                     }),
                     ftxui::text(""),
                     ftxui::hbox({
                         ftxui::text(" " + std::to_string(filtered_indices->size()) +
                                     " matches ") |
                             ftxui::color(theme.base.dim),
                         ftxui::filler(),
                         ftxui::text(selected_id.empty()
                                         ? ""
                                         : "selected: " + selected_id) |
                             ftxui::color(theme.base.dim),
                     }),
                     ftxui::separatorLight() |
                         ftxui::color(theme.modals.border),
                     scrollable->Render() | ftxui::xflex |
                         ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 18),
                     ftxui::text(""),
                     ftxui::text(
                         "Type to fuzzy-search. Enter switches. Ctrl+Y copies thread ID. ESC cancels.") |
                         ftxui::color(theme.base.dim)}),
                theme.modals.bg),
            modal_width, 24);
      });

  return ftxui::CatchEvent(
      modal_renderer,
      [all_threads_ptr, filtered_indices, filter_text, selected, row_boxes,
       scrollable, rebuild_filtered, &state,
       &h](ftxui::Event event) {
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

    if (event == ftxui::Event::Return) {
      rebuild_filtered();
      if (*selected >= 0 && *selected < static_cast<int>(filtered_indices->size())) {
        const auto &entry = (*all_threads_ptr)[(*filtered_indices)[*selected]];
        if (entry.locked_by_other) {
          return true;
        }
        if (h.switchThread(entry.meta.threadId)) {
          state.popModal();
        }
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
            *selected = rowIndex;
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
