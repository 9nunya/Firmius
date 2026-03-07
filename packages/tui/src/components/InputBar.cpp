#include "components/InputBar.hpp"
#include "commands/CommandManager.hpp"
#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

static const int MAX_VISIBLE_LINES = 5;

static void insertText(std::string &text, int &cursor,
                       const std::string &insert) {
  if (cursor < 0)
    cursor = 0;
  if (cursor > static_cast<int>(text.size()))
    cursor = static_cast<int>(text.size());
  text.insert(text.begin() + cursor, insert.begin(), insert.end());
  cursor += static_cast<int>(insert.size());
}

// Return the 0-based line number the cursor is on.
static int cursorLine(const std::string &text, int cursor) {
  int line = 0;
  int pos = std::min(cursor, static_cast<int>(text.size()));
  for (int i = 0; i < pos; ++i) {
    if (text[i] == '\n')
      ++line;
  }
  return line;
}

static int countLines(const std::string &s) {
  if (s.empty())
    return 1;
  int n = 1;
  for (char c : s) {
    if (c == '\n')
      n++;
  }
  return n;
}

// Split text into lines.
static std::vector<std::string> splitLines(const std::string &s) {
  std::vector<std::string> lines;
  std::string cur;
  for (char c : s) {
    if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  lines.push_back(cur);
  return lines;
}

// Helper to extract one UTF-8 character safely
static std::string getUtf8Char(const std::string &str, size_t pos) {
  if (pos >= str.size())
    return " ";
  unsigned char c = str[pos];
  size_t len = 1;
  if ((c & 0xE0) == 0xC0)
    len = 2;
  else if ((c & 0xF0) == 0xE0)
    len = 3;
  else if ((c & 0xF8) == 0xF0)
    len = 4;
  return str.substr(pos, std::min(len, str.size() - pos));
}

ftxui::Component InputBar(const std::shared_ptr<InputBarModel> &model,
                          std::function<void(const std::string &)> on_submit) {

  // Shared scroll offset — which line is at the top of the visible window.
  auto scroll_top = std::make_shared<int>(0);

  auto opt = ftxui::InputOption::Default();
  opt.multiline = true;
  if (model) {
    opt.content = model->buffer;
    opt.cursor_position = model->cursor;
    opt.placeholder = model->placeholder;
  }

  opt.transform = [](ftxui::InputState state) {
    if (state.is_placeholder) {
      state.element |= ftxui::dim;
    }
    return state.element;
  };

  auto input = ftxui::Input(opt);
  auto with_keys = ftxui::CatchEvent(input, [model, on_submit, input,
                                             scroll_top](ftxui::Event event) {
    if (!model || !model->buffer || !model->cursor)
      return false;

    // Tab autocomplete
    if (event == ftxui::Event::Tab) {
      auto ac = CommandManager::instance().getAutocomplete(*model->buffer);
      if (ac && ac->is_typing_command_name && !ac->command_matches.empty()) {
        *model->buffer = "/" + ac->command_matches[0].name + " ";
        *model->cursor = static_cast<int>(model->buffer->size());
      }
      return true;
    }

    // Submit on plain Enter
    if (event == ftxui::Event::Return) {
      auto ac = CommandManager::instance().getAutocomplete(*model->buffer);
      if (ac && ac->is_typing_command_name && !ac->command_matches.empty() &&
          !ac->command_matches[0].is_exact) {
        // "when user presses enter + there is a match showing, autocomplete
        // command as the best matching command and directly execute"
        std::string cmd = "/" + ac->command_matches[0].name;
        on_submit(cmd);
        model->buffer->clear();
        *model->cursor = 0;
        *scroll_top = 0;
        return true;
      }

      if (!model->buffer->empty()) {
        on_submit(*model->buffer);
        model->buffer->clear();
        *model->cursor = 0;
        *scroll_top = 0;
      }
      return true;
    }

    // Newline insertion
    std::string raw = event.input();
    bool is_shift_enter = false;
    if (raw == "\x1b[13;2u")
      is_shift_enter = true;
    if (raw == "\x1b\r" || raw == "\x1b\n")
      is_shift_enter = true;
    if (raw == "\x1b[27;2;13~")
      is_shift_enter = true;

    if (is_shift_enter) {
      insertText(*model->buffer, *model->cursor, "\n");
      int cline = cursorLine(*model->buffer, *model->cursor);
      if (cline >= *scroll_top + MAX_VISIBLE_LINES) {
        *scroll_top = cline - MAX_VISIBLE_LINES + 1;
      }
      return true;
    }

    // ArrowUp/Down
    int lines = countLines(*model->buffer);
    if (lines > 1) {
      if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown) {
        input->OnEvent(event);
        int cline = cursorLine(*model->buffer, *model->cursor);
        if (cline < *scroll_top) {
          *scroll_top = cline;
        } else if (cline >= *scroll_top + MAX_VISIBLE_LINES) {
          *scroll_top = cline - MAX_VISIBLE_LINES + 1;
        }
        return true;
      }
    }

    return false;
  });

  return ftxui::Renderer(with_keys, [with_keys, model, scroll_top] {
    auto prompt = ftxui::text("> ") | ftxui::bold |
                  ftxui::color(ftxui::Color::RGB(140, 120, 200));

    int total_lines = 1;
    if (model && model->buffer) {
      total_lines = countLines(*model->buffer);
    }

    // Autocomplete View MUST be built during Render()
    auto ac = CommandManager::instance().getAutocomplete(
        model->buffer ? *model->buffer : "");
    ftxui::Element autocomplete_layer = ftxui::text("");

    if (ac && ac->is_typing_command_name && !ac->command_matches.empty()) {
      std::vector<ftxui::Element> match_elements;
      for (size_t i = 0; i < ac->command_matches.size() && i < 5; ++i) {
        const auto &match = ac->command_matches[i];

        auto match_text = ftxui::text("/" + match.name) | ftxui::bold |
                          ftxui::color(ftxui::Color::White);
        if (i == 0)
          match_text = match_text | ftxui::inverted;

        match_elements.push_back(
            ftxui::hbox({match_text, ftxui::text("  "),
                         ftxui::text(match.description) | ftxui::dim}));
      }
      autocomplete_layer = ftxui::vbox(match_elements) | ftxui::borderEmpty |
                           ftxui::bgcolor(ftxui::Color::RGB(40, 40, 60));
    } else if (ac && !ac->is_typing_command_name && ac->current_arg) {
      std::string type_str;
      switch (ac->current_arg->type) {
      case ArgType::String:
        type_str = "String";
        break;
      case ArgType::Number:
        type_str = "Number";
        break;
      case ArgType::AgentId:
        type_str = "AgentId";
        break;
      case ArgType::ThreadId:
        type_str = "ThreadId";
        break;
      case ArgType::Filepath:
        type_str = "Filepath";
        break;
      }

      autocomplete_layer =
          ftxui::hbox(
              {ftxui::text(ac->current_arg->name) | ftxui::bold |
                   ftxui::color(ftxui::Color::Cyan),
               ftxui::text(" [" + type_str +
                           (ac->current_arg->optional ? ", opt" : "") + "] ") |
                   ftxui::dim,
               ftxui::text(ac->current_arg->description)}) |
          ftxui::borderEmpty | ftxui::bgcolor(ftxui::Color::RGB(40, 40, 60));
    }

    // For short inputs, render normally and naturally let ftxui handle cursor
    if (total_lines <= MAX_VISIBLE_LINES) {
      *scroll_top = 0;
      return ftxui::vbox(
          {autocomplete_layer, ftxui::hbox({
                                   prompt,
                                   with_keys->Render() | ftxui::flex,
                               })});
    }

    auto all_lines = splitLines(model->buffer ? *model->buffer : "");
    int max_scroll =
        std::max(0, static_cast<int>(all_lines.size()) - MAX_VISIBLE_LINES);
    if (*scroll_top > max_scroll)
      *scroll_top = max_scroll;
    if (*scroll_top < 0)
      *scroll_top = 0;

    int cursor_line = cursorLine(model->buffer ? *model->buffer : "",
                                 model->cursor ? *model->cursor : 0);
    if (cursor_line < *scroll_top) {
      *scroll_top = cursor_line;
    } else if (cursor_line >= *scroll_top + MAX_VISIBLE_LINES) {
      *scroll_top = cursor_line - MAX_VISIBLE_LINES + 1;
    }

    int cursor_col = model->cursor ? *model->cursor : 0;
    for (int k = 0; k < cursor_line; ++k) {
      cursor_col -= static_cast<int>(all_lines[k].size()) + 1;
    }
    if (cursor_col < 0)
      cursor_col = 0;

    std::vector<ftxui::Element> visible_elements;
    int end = std::min(*scroll_top + MAX_VISIBLE_LINES,
                       static_cast<int>(all_lines.size()));

    for (int i = *scroll_top; i < end; ++i) {
      ftxui::Element line_el;
      if (i == cursor_line) {
        std::string pre = "";
        std::string cur_char = " ";
        std::string suf = "";
        int len = static_cast<int>(all_lines[i].size());

        if (cursor_col <= len) {
          pre = all_lines[i].substr(0, cursor_col);
          if (cursor_col < len) {
            cur_char = getUtf8Char(all_lines[i], cursor_col);
            suf = all_lines[i].substr(cursor_col + cur_char.size());
          }
        }

        line_el = ftxui::hbox({
                      ftxui::text(pre),
                      // The ftxui::focus decorator tells the screen engine to
                      // put the hardware cursor here!
                      ftxui::text(cur_char) | ftxui::inverted | ftxui::focus,
                      ftxui::text(suf),
                  }) |
                  ftxui::color(ftxui::Color::RGB(220, 220, 240));
      } else {
        line_el = ftxui::text(all_lines[i]) |
                  ftxui::color(ftxui::Color::RGB(180, 180, 200));
      }
      visible_elements.push_back(line_el);
    }

    std::string indicator;
    if (*scroll_top > 0 && end < static_cast<int>(all_lines.size())) {
      indicator = " [" + std::to_string(cursor_line + 1) + "/" +
                  std::to_string(total_lines) +
                  " \xe2\x86\x95]"; // Up-Down arrow
    } else if (*scroll_top > 0) {
      indicator = " [" + std::to_string(cursor_line + 1) + "/" +
                  std::to_string(total_lines) + " \xe2\x86\x91]"; // Up arrow
    } else if (end < static_cast<int>(all_lines.size())) {
      indicator = " [" + std::to_string(cursor_line + 1) + "/" +
                  std::to_string(total_lines) + " \xe2\x86\x93]"; // Down arrow
    }

    auto line_hint = ftxui::text(indicator) | ftxui::dim |
                     ftxui::color(ftxui::Color::RGB(100, 100, 140));

    // We no longer render the hidden true input to avoid it capturing focus.
    // The CatchEvent wrapper still receives events because it wraps the
    // returned hbox.
    return ftxui::vbox(
        {autocomplete_layer,
         ftxui::hbox({
             prompt,
             ftxui::vbox(std::move(visible_elements)) | ftxui::flex,
             line_hint,
         })});
  });
}

} // namespace firmius::tui
