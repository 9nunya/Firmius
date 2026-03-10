#include "components/InputBar.hpp"
#include "commands/CommandManager.hpp"
#include "providers/ProviderRegistry.hpp"
#include <algorithm>
#include <cctype>
#include <vector>
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

namespace {

constexpr size_t MAX_PROVIDER_SUGGESTIONS = 6;

std::string toLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool fuzzyMatchIgnoreCase(const std::string &text, const std::string &query) {
  if (query.empty())
    return true;
  auto comp = [](char lhs, char rhs) {
    return std::tolower(static_cast<unsigned char>(lhs)) ==
           std::tolower(static_cast<unsigned char>(rhs));
  };
  return std::search(text.begin(), text.end(), query.begin(), query.end(), comp) !=
         text.end();
}

std::string providerTypeLabel(firmius::provider::ProviderType type) {
  using Ptr = firmius::provider::ProviderType;
  switch (type) {
  case Ptr::OAuth:
    return "OAuth";
  case Ptr::APIKey:
    return "API key";
  }
  return "Provider";
}

struct ProviderSuggestion {
  std::string id;
  std::string type_label;
  bool is_prefix = false;
};

std::vector<ProviderSuggestion>
buildProviderSuggestions(ArgType argType, const std::string &filter) {
  std::vector<ProviderSuggestion> matches;
  auto providers =
      firmius::provider::ProviderRegistry::instance().listProviders();
  std::string lower_filter = toLowerAscii(filter);
  for (const auto &provider : providers) {
    if (!provider)
      continue;
    auto type = provider->getProviderType();
    if (argType == ArgType::OAuthProvider &&
        type != firmius::provider::ProviderType::OAuth) {
      continue;
    }
    const std::string id = provider->getId();
    if (!filter.empty() && !fuzzyMatchIgnoreCase(id, filter)) {
      continue;
    }
    std::string lower_id = toLowerAscii(id);
    bool prefix = !lower_filter.empty() && lower_id.rfind(lower_filter, 0) == 0;
    matches.push_back({id, providerTypeLabel(type), prefix});
  }

  std::sort(matches.begin(), matches.end(), [](const ProviderSuggestion &lhs,
                                               const ProviderSuggestion &rhs) {
    if (lhs.is_prefix != rhs.is_prefix)
      return lhs.is_prefix;
    return lhs.id < rhs.id;
  });

  if (matches.size() > MAX_PROVIDER_SUGGESTIONS)
    matches.resize(MAX_PROVIDER_SUGGESTIONS);
  return matches;
}

}

ftxui::Component InputBar(const std::shared_ptr<InputBarModel> &model,
                          std::function<void(const std::string &)> on_submit) {

  auto scroll_top = std::make_shared<int>(0);
  auto suggestion_index = std::make_shared<int>(0);

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
  auto with_keys = ftxui::CatchEvent(input, [model, on_submit, input, scroll_top,
                                             suggestion_index](ftxui::Event event) {
    if (!model || !model->buffer || !model->cursor)
      return false;

    auto ac = CommandManager::instance().getAutocomplete(*model->buffer);

    if (ac && ((ac->is_typing_command_name && !ac->command_matches.empty()) ||
               (!ac->is_typing_command_name && ac->current_arg &&
                (ac->current_arg->type == ArgType::Provider ||
                 ac->current_arg->type == ArgType::OAuthProvider)))) {

      size_t match_count = 0;
      if (ac->is_typing_command_name) {
        match_count = std::min(ac->command_matches.size(), (size_t)5);
      } else {
        auto query = ac->has_current_arg_value ? ac->current_arg_value : "";
        match_count = buildProviderSuggestions(ac->current_arg->type, query).size();
      }

      if (match_count > 0) {
        if (event == ftxui::Event::ArrowDown) {
          *suggestion_index = (*suggestion_index + 1) % match_count;
          return true;
        }
        if (event == ftxui::Event::ArrowUp) {
          *suggestion_index = (*suggestion_index + match_count - 1) % match_count;
          return true;
        }
      } else {
        *suggestion_index = 0;
      }
    } else {
      *suggestion_index = 0;
    }

    if (event == ftxui::Event::Tab || event == ftxui::Event::Return) {
      if (ac) {
        if (ac->is_typing_command_name && !ac->command_matches.empty()) {
          size_t idx = static_cast<size_t>(*suggestion_index);
          if (idx >= ac->command_matches.size()) idx = 0;
          
          if (event == ftxui::Event::Return && ac->command_matches[idx].is_exact) {
          } else {
            *model->buffer = "/" + ac->command_matches[idx].name + " ";
            *model->cursor = static_cast<int>(model->buffer->size());
            *suggestion_index = 0;
            return true;
          }
        } else if (!ac->is_typing_command_name && ac->current_arg && 
                   (ac->current_arg->type == ArgType::Provider || 
                    ac->current_arg->type == ArgType::OAuthProvider)) {
          auto query = ac->has_current_arg_value ? ac->current_arg_value : "";
          auto suggestions = buildProviderSuggestions(ac->current_arg->type, query);
          if (!suggestions.empty()) {
            size_t idx = static_cast<size_t>(*suggestion_index);
            if (idx >= suggestions.size()) idx = 0;

            std::string text = *model->buffer;
            size_t last_space = text.find_last_of(' ', *model->cursor - 1);
            if (last_space == std::string::npos) last_space = 0;
            else last_space++;

            text.replace(last_space, *model->cursor - last_space, suggestions[idx].id + " ");
            *model->buffer = text;
            *model->cursor = static_cast<int>(last_space + suggestions[idx].id.size() + 1);
            *suggestion_index = 0;
            return true;
          }
        }
      }
    }

    if (event == ftxui::Event::Return) {
      if (!model->buffer->empty()) {
        on_submit(*model->buffer);
        model->buffer->clear();
        *model->cursor = 0;
        *scroll_top = 0;
      }
      return true;
    }

    std::string raw = event.input();
    bool is_shift_enter = (raw == "\x1b[13;2u" || raw == "\x1b\r" || raw == "\x1b\n" || raw == "\x1b[27;2;13~");

    if (is_shift_enter) {
      insertText(*model->buffer, *model->cursor, "\n");
      int cline = cursorLine(*model->buffer, *model->cursor);
      if (cline >= *scroll_top + MAX_VISIBLE_LINES) {
        *scroll_top = cline - MAX_VISIBLE_LINES + 1;
      }
      return true;
    }

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

  return ftxui::Renderer(with_keys, [with_keys, model, scroll_top, suggestion_index] {
    auto prompt = ftxui::text("> ") | ftxui::bold |
                  ftxui::color(ftxui::Color::RGB(140, 120, 200));

    int total_lines = 1;
    if (model && model->buffer) {
      total_lines = countLines(*model->buffer);
    }

    auto ac = CommandManager::instance().getAutocomplete(
        model->buffer ? *model->buffer : "");
    ftxui::Element autocomplete_layer = ftxui::text("");

    if (ac && ac->is_typing_command_name && !ac->command_matches.empty()) {
      std::vector<ftxui::Element> match_elements;
      for (size_t i = 0; i < ac->command_matches.size() && i < 5; ++i) {
        const auto &match = ac->command_matches[i];

        auto match_text = ftxui::text("/" + match.name) | ftxui::bold |
                          ftxui::color(ftxui::Color::White);
        if (i == static_cast<size_t>(*suggestion_index))
          match_text = match_text | ftxui::inverted;

        match_elements.push_back(
            ftxui::hbox({match_text, ftxui::text("  "),
                         ftxui::text(match.description) | ftxui::dim}));
      }
      autocomplete_layer = ftxui::vbox(match_elements) |
                           ftxui::bgcolor(ftxui::Color::RGB(40, 40, 60));
    } else if (ac && !ac->is_typing_command_name && ac->current_arg) {
      const auto &arg = *ac->current_arg;
      std::string type_str;
      switch (arg.type) {
      case ArgType::String: type_str = "String"; break;
      case ArgType::Number: type_str = "Number"; break;
      case ArgType::AgentId: type_str = "AgentId"; break;
      case ArgType::ThreadId: type_str = "ThreadId"; break;
      case ArgType::Filepath: type_str = "Filepath"; break;
      case ArgType::Provider: type_str = "Provider"; break;
      case ArgType::OAuthProvider: type_str = "OAuth provider"; break;
      }

      auto header = ftxui::hbox({
                          ftxui::text(arg.name) | ftxui::bold |
                              ftxui::color(ftxui::Color::Cyan),
                          ftxui::text(" [" + type_str +
                                      (arg.optional ? ", opt" : "") + "] ") |
                              ftxui::dim,
                          ftxui::text(arg.description)});

      const bool wants_provider_suggestions =
          arg.type == ArgType::Provider || arg.type == ArgType::OAuthProvider;
      if (wants_provider_suggestions) {
        std::string query = ac->has_current_arg_value ?
                                ac->current_arg_value :
                                std::string();
        auto suggestions = buildProviderSuggestions(arg.type, query);
        ftxui::Element suggestion_body;
        if (!suggestions.empty()) {
          ftxui::Elements rows;
          for (size_t i = 0; i < suggestions.size(); ++i) {
            const auto &match = suggestions[i];
            auto label = ftxui::text(" " + match.id) | ftxui::bold;
            if (i == static_cast<size_t>(*suggestion_index))
              label = label | ftxui::inverted;
            auto meta = ftxui::text(" [" + match.type_label + "]") |
                        ftxui::dim;
            rows.push_back(ftxui::hbox({label, ftxui::filler(), meta}));
          }
          suggestion_body = ftxui::vbox(rows) | ftxui::yframe |
                            ftxui::color(ftxui::Color::GrayLight);
        } else {
          std::string message = query.empty()
                                    ? "No providers are registered yet."
                                    : "No providers match '" + query + "'";
          suggestion_body = ftxui::text(message) | ftxui::dim | ftxui::center;
        }

        autocomplete_layer = ftxui::vbox(
                                     {header, ftxui::separatorLight(),
                                      suggestion_body}) |
                             ftxui::bgcolor(ftxui::Color::RGB(40, 40, 60));
      } else {
        autocomplete_layer = header |
                             ftxui::bgcolor(ftxui::Color::RGB(40, 40, 60));
      }
    }

    if (total_lines <= MAX_VISIBLE_LINES) {
      *scroll_top = 0;
      return ftxui::vbox(
          {autocomplete_layer, ftxui::hbox({
                                   prompt,
                                   with_keys->Render() | ftxui::flex,
                                })});
    }

    auto all_lines = splitLines(model->buffer ? *model->buffer : "");
    int max_scroll = std::max(0, static_cast<int>(all_lines.size()) - MAX_VISIBLE_LINES);
    if (*scroll_top > max_scroll) *scroll_top = max_scroll;
    if (*scroll_top < 0) *scroll_top = 0;

    int cursor_line = cursorLine(model->buffer ? *model->buffer : "", model->cursor ? *model->cursor : 0);
    if (cursor_line < *scroll_top) {
      *scroll_top = cursor_line;
    } else if (cursor_line >= *scroll_top + MAX_VISIBLE_LINES) {
      *scroll_top = cursor_line - MAX_VISIBLE_LINES + 1;
    }

    int cursor_col = model->cursor ? *model->cursor : 0;
    for (int k = 0; k < cursor_line; ++k) {
      cursor_col -= static_cast<int>(all_lines[k].size()) + 1;
    }
    if (cursor_col < 0) cursor_col = 0;

    std::vector<ftxui::Element> visible_elements;
    int end = std::min(*scroll_top + MAX_VISIBLE_LINES, static_cast<int>(all_lines.size()));

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
      indicator = " [" + std::to_string(cursor_line + 1) + "/" + std::to_string(total_lines) + " \xe2\x86\x95]";
    } else if (*scroll_top > 0) {
      indicator = " [" + std::to_string(cursor_line + 1) + "/" + std::to_string(total_lines) + " \xe2\x86\x91]";
    } else if (end < static_cast<int>(all_lines.size())) {
      indicator = " [" + std::to_string(cursor_line + 1) + "/" + std::to_string(total_lines) + " \xe2\x86\x93]";
    }

    auto line_hint = ftxui::text(indicator) | ftxui::dim | ftxui::color(ftxui::Color::RGB(100, 100, 140));

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
