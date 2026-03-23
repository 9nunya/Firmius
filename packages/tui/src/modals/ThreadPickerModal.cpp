#include "modals/ThreadPickerModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include <Serialization.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <rapidjson/document.h>
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
#include <variant>

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

std::string extractMessageText(const firmius::shared::Message &msg) {
  std::string out;
  for (const auto &part : msg.content) {
    if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
      if (!out.empty())
        out.push_back(' ');
      out += txt->text;
    } else if (std::holds_alternative<firmius::shared::ImageContent>(part)) {
      if (!out.empty())
        out.push_back(' ');
      out += "[image]";
    }
  }
  return out;
}

LastUserMessage findLastUserMessage(const std::string &thread_id) {
  LastUserMessage best;
  std::filesystem::path thread_dir = getThreadsBase() / thread_id;
  std::error_code ec;
  if (!std::filesystem::exists(thread_dir, ec))
    return best;

  for (const auto &entry : std::filesystem::directory_iterator(thread_dir)) {
    if (!entry.is_regular_file())
      continue;
    if (entry.path().extension() != ".jsonl")
      continue;

    std::ifstream file(entry.path());
    if (!file.is_open())
      continue;

    std::string line;
    while (std::getline(file, line)) {
      if (line.empty())
        continue;
      rapidjson::Document doc;
      doc.Parse(line.c_str());
      if (doc.HasParseError() || !doc.IsObject())
        continue;
      try {
        auto turn = firmius::shared::agentTurnFromJsonValue(doc);
        for (const auto &msg : turn.messages) {
          if (msg.role != firmius::shared::Role::User)
            continue;
          if (msg.timestamp >= best.timestamp) {
            best.timestamp = msg.timestamp;
            best.text = extractMessageText(msg);
            best.found = true;
          }
        }
      } catch (...) {
        continue;
      }
    }
  }

  return best;
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
  bool locked_by_other = false;
  int locked_pid = -1;
};

} // namespace

namespace firmius::tui {

ftxui::Component ThreadPickerModal::create(TuiState &state) {
  auto &h = firmius::core::Harness::instance();
  auto selected = std::make_shared<int>(0);
  auto entries = std::make_shared<std::vector<std::string>>();
  auto threads_ptr = std::make_shared<std::vector<ThreadEntry>>();
  auto list_width = std::make_shared<int>(80);

  std::error_code cwd_ec;
  auto cwd_path = std::filesystem::current_path(cwd_ec);
  std::string cwd = cwd_ec ? "" : normalizePath(cwd_path.string());

  auto refresh_threads = [entries, threads_ptr, selected, &h, cwd] {
    auto all_threads = h.listThreads();
    threads_ptr->clear();
    entries->clear();

    int idx = 0;
    for (const auto &t : all_threads) {
      if (!cwd.empty() && normalizePath(t.cwd) != cwd && !t.isBenchmarkRun) {
        continue;
      }

      ThreadEntry entry;
      entry.meta = t;
      entry.agent_count = static_cast<int>(h.listAgents(t.threadId).size());

      auto last_user = findLastUserMessage(t.threadId);
      if (last_user.found) {
        entry.last_user_text = last_user.text;
        entry.last_user_timestamp = last_user.timestamp;
      }

      auto locked_pid = lockedPidByOther(t.threadId);
      if (locked_pid.has_value()) {
        entry.locked_by_other = true;
        entry.locked_pid = *locked_pid;
      }

      threads_ptr->push_back(entry);
      entries->push_back(std::to_string(idx++));
    }

    if (*selected >= static_cast<int>(entries->size())) {
      *selected = entries->empty() ? 0 : static_cast<int>(entries->size()) - 1;
    }
  };

  // Initial load
  refresh_threads();

  auto option = ftxui::MenuOption::Vertical();
  option.entries_option.transform =
      [threads_ptr, list_width](ftxui::EntryState state) {
        const auto &theme = ThemeManager::instance().getCurrentTheme();
        int idx = 0;
        try {
          idx = std::stoi(state.label);
        } catch (...) {
          return ftxui::text("");
        }

        if (idx < 0 || idx >= static_cast<int>(threads_ptr->size())) {
          return ftxui::text("");
        }

        const auto &entry = (*threads_ptr)[idx];
        bool selected = state.active;

        std::string agent_label = "(" + std::to_string(entry.agent_count) +
                                  (entry.agent_count == 1 ? " agent)" :
                                                           " agents)");
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
        int reserved = 2 + static_cast<int>(agent_label.size()) +
                       (benchmark_label.empty()
                            ? 0
                            : 3 + static_cast<int>(benchmark_label.size())) +
                       (lock_label.empty() ? 0 : 2 + (int)lock_label.size());
        int max_title =
            std::max(8, width - reserved - 2 /*padding*/);
        std::string title =
            truncateText(entry.meta.title.empty() ? "Untitled Thread"
                                                 : entry.meta.title,
                         max_title);

        ftxui::Color title_fg =
            entry.locked_by_other
                ? theme.base.dim
                : (selected ? theme.modals.highlight_fg : theme.modals.fg);
        ftxui::Color sub_fg = entry.locked_by_other ? theme.base.dim
                                                    : theme.base.dim;

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
                           ftxui::text(lock_label) |
                               ftxui::color(sub_fg)});
        }

        std::string time_label = formatTime(entry.last_user_timestamp);
        std::string user_text =
            entry.last_user_text.empty()
                ? "No user messages yet"
                : flattenText(entry.last_user_text);

        int time_width = time_label.empty() ? 0 : (int)time_label.size() + 1;
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

        auto row = ftxui::vbox({top_line, msg_line});
        if (selected) {
          row = row | ftxui::bgcolor(theme.modals.highlight_bg);
        }
        return row;
      };

  auto menu = ftxui::Menu(entries.get(), selected.get(), option);

  auto modal_renderer =
      ftxui::Renderer(menu, [menu, entries, list_width] {
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

        if (entries->empty()) {
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

        return FlatModalPanel(
            theme, "Select Thread",
            ModalSection(
                theme,
                ftxui::vbox(
                    {menu->Render() | ftxui::vscroll_indicator |
                         ftxui::yframe |
                         ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 18),
                     ftxui::text(""),
                     ftxui::text("Press Enter to switch, ESC to cancel.") |
                         ftxui::color(theme.base.dim)}),
                theme.modals.bg),
            modal_width, 24);
      });

  return ftxui::CatchEvent(
      modal_renderer,
      [threads_ptr, selected, &state, &h](ftxui::Event event) {
    if (event == ftxui::Event::Return) {
      if (*selected >= 0 && *selected < (int)threads_ptr->size()) {
        const auto &entry = (*threads_ptr)[*selected];
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
    return false;
  });
}

} // namespace firmius::tui
