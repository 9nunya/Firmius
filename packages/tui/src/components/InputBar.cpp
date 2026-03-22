#include "components/InputBar.hpp"
#include "ThemeManager.hpp"
#include "commands/CommandManager.hpp"
#include "providers/ProviderRegistry.hpp"
#include "NotificationManager.hpp"
#include "utils/Clipboard.hpp"
#include <algorithm>
#include <cctype>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>

namespace firmius::tui {

static const int MAX_VISIBLE_LINES = 5;

// Generate unique ID for pasted blocks
static std::string generateBlockId() {
  static int counter = 0;
  return "block_" + std::to_string(++counter);
}

// Count lines in text
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

// Split text into lines
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

// Get cursor line number
static int cursorLine(const std::string &text, int cursor) {
  int line = 0;
  int pos = std::min(cursor, static_cast<int>(text.size()));
  for (int i = 0; i < pos; ++i) {
    if (text[i] == '\n')
      ++line;
  }
  return line;
}

// Insert text at cursor position
static void insertText(std::string &text, int &cursor,
                       const std::string &insert) {
  if (cursor < 0)
    cursor = 0;
  if (cursor > static_cast<int>(text.size()))
    cursor = static_cast<int>(text.size());
  text.insert(text.begin() + cursor, insert.begin(), insert.end());
  cursor += static_cast<int>(insert.size());
}

// Find which pasted block contains position (if any)
static int findBlockAtPos(const std::vector<PastedBlock> &blocks, size_t pos) {
  for (size_t i = 0; i < blocks.size(); ++i) {
    if (pos >= blocks[i].start_pos && pos < blocks[i].end_pos) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// Remove block and clean up buffer
static void removeBlock(std::string &buffer, int &cursor,
                        std::vector<PastedBlock> &blocks, int block_idx) {
  if (block_idx < 0 || block_idx >= static_cast<int>(blocks.size()))
    return;

  auto &block = blocks[block_idx];
  size_t len = block.end_pos - block.start_pos;

  // Remove from buffer
  buffer.erase(block.start_pos, len);

  // Adjust cursor
  if (static_cast<size_t>(cursor) > block.start_pos) {
    cursor -= static_cast<int>(len);
    if (cursor < 0)
      cursor = 0;
  }

  // Adjust other blocks' positions
  for (auto &b : blocks) {
    if (b.start_pos > block.start_pos) {
      b.start_pos -= len;
      b.end_pos -= len;
    }
  }

  // Remove block from list
  blocks.erase(blocks.begin() + block_idx);
}

// Create pasted text block placeholder
static std::string createTextBlockPlaceholder(size_t line_count) {
  return "[Pasted: " + std::to_string(line_count) + " lines]";
}

// Expand buffer content by replacing placeholders with actual pasted content
static std::string expandPastedContent(
    const std::string &buffer,
    const std::vector<PastedBlock> &pasted_blocks) {
  if (pasted_blocks.empty()) {
    return buffer;
  }

  std::string result;
  size_t pos = 0;

  // Sort blocks by start position to process in order
  std::vector<PastedBlock> sorted_blocks = pasted_blocks;
  std::sort(sorted_blocks.begin(), sorted_blocks.end(),
            [](const PastedBlock &a, const PastedBlock &b) {
              return a.start_pos < b.start_pos;
            });

  for (const auto &block : sorted_blocks) {
    if (block.type != "text")
      continue;

    // Add text before this block
    if (block.start_pos > pos && block.start_pos <= buffer.size()) {
      result += buffer.substr(pos, block.start_pos - pos);
    }

    // Add the actual content instead of placeholder
    result += block.content;

    pos = block.end_pos;
  }

  // Add remaining text after last block
  if (pos < buffer.size()) {
    result += buffer.substr(pos);
  }

  return result;
}

namespace {

constexpr size_t MAX_PROVIDER_SUGGESTIONS = 6;

std::string toLowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool fuzzyMatchIgnoreCase(const std::string &text, const std::string &query) {
  if (query.empty())
    return true;
  auto comp = [](char lhs, char rhs) {
    return std::tolower(static_cast<unsigned char>(lhs)) ==
           std::tolower(static_cast<unsigned char>(rhs));
  };
  return std::search(text.begin(), text.end(), query.begin(), query.end(),
                     comp) != text.end();
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
    // ProviderId shows all providers (both OAuth and APIKey)
    const std::string id = provider->getId();
    if (!filter.empty() && !fuzzyMatchIgnoreCase(id, filter)) {
      continue;
    }
    std::string lower_id = toLowerAscii(id);
    bool prefix = !lower_filter.empty() && lower_id.rfind(lower_filter, 0) == 0;
    matches.push_back({id, providerTypeLabel(type), prefix});
  }

  std::sort(matches.begin(), matches.end(),
            [](const ProviderSuggestion &lhs, const ProviderSuggestion &rhs) {
              if (lhs.is_prefix != rhs.is_prefix)
                return lhs.is_prefix;
              return lhs.id < rhs.id;
            });

  if (matches.size() > MAX_PROVIDER_SUGGESTIONS)
    matches.resize(MAX_PROVIDER_SUGGESTIONS);
  return matches;
}

std::vector<std::string>
buildAtReferenceSuggestions(const std::shared_ptr<InputBarModel> &model,
                            const AtReferenceAutocompleteState &state) {
  if (!model || !state.active) {
    return {};
  }
  if (state.is_artifact) {
    if (!model->complete_artifact_references) {
      return {};
    }
    return model->complete_artifact_references(state.query);
  }
  if (!model->complete_file_references) {
    return {};
  }
  return model->complete_file_references(state.query);
}

} // namespace

ftxui::Component InputBar(
    const std::shared_ptr<InputBarModel> &model,
    std::function<void(const std::string &, const std::vector<PastedBlock> &)>
        on_submit) {

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

  auto in_paste = std::make_shared<bool>(false);
  auto paste_buffer = std::make_shared<std::string>();
  auto just_submitted = std::make_shared<bool>(false);

  auto input = ftxui::Input(opt);
  auto with_keys = ftxui::CatchEvent(input, [model, on_submit, input,
                                             scroll_top, suggestion_index,
                                             in_paste, paste_buffer,
                                             just_submitted](
                                                ftxui::Event event) {
    if (!model || !model->buffer || !model->cursor)
      return false;

    // Skip processing if we just submitted (let input component catch up)
    if (*just_submitted) {
      *just_submitted = false;
      return false;
    }

    // Get raw input once for all handlers
    std::string raw = event.input();

    // Handle bracketed paste (Ctrl+V or terminal paste)
    // Bracketed paste starts with \x1b[200~ and ends with \x1b[201~
    if (raw == "\x1b[200~") {
      // Start of bracketed paste
      *in_paste = true;
      paste_buffer->clear();
      return true; // Consume the event
    }

    if (raw == "\x1b[201~") {
      // End of bracketed paste - process the content
      if (*in_paste && !paste_buffer->empty()) {
        // Remove trailing newline if present
        if (!paste_buffer->empty() && paste_buffer->back() == '\n') {
          paste_buffer->pop_back();
        }

        // Count lines
        int line_count = countLines(*paste_buffer);

        // Check if it looks like image data (very long single line,
        // base64-like)
        bool is_likely_image = (line_count == 1 && paste_buffer->size() > 1000);

        if (is_likely_image) {
          // Add as image tag above input
          PastedBlock img_block;
          img_block.type = "image";
          img_block.id = generateBlockId();
          model->image_tags.push_back(img_block);
        } else if (line_count >= 2) {
          // Multi-line text - create pasted block placeholder
          std::string placeholder = createTextBlockPlaceholder(line_count);

          // Insert placeholder at cursor
          insertText(*model->buffer, *model->cursor, placeholder);

          // Register the block
          PastedBlock text_block;
          text_block.type = "text";
          text_block.id = generateBlockId();
          text_block.line_count = line_count;
          text_block.content = *paste_buffer;
          text_block.start_pos = *model->cursor - placeholder.size();
          text_block.end_pos = *model->cursor;
          model->pasted_blocks.push_back(text_block);
        } else {
          // Single line - insert normally
          insertText(*model->buffer, *model->cursor, *paste_buffer);
        }

        paste_buffer->clear();
        *in_paste = false;
      }
      return true; // Consume the event
    }

    if (*in_paste) {
      // Accumulate paste content
      *paste_buffer += raw;
      return true; // Consume the event
    }

    // Handle Alt+Backspace (delete previous word)
    if (raw == "\x1b\x7f" || raw == "\x1b\b") {
      if (*model->cursor > 0) {
        int end = *model->cursor;
        int start = end;
        // Skip whitespace
        while (start > 0 && std::isspace((*model->buffer)[start - 1]))
          start--;
        // Skip word characters
        while (start > 0 && !std::isspace((*model->buffer)[start - 1]))
          start--;
        model->buffer->erase(start, end - start);
        *model->cursor = start;
      }
      return true;
    }

    // Handle Ctrl+Left Arrow (move to previous word)
    if (raw == "\x1b[1;5D" || raw == "\x1b\x1b[D" || raw == std::string("\x1b") + "b") {
      if (*model->cursor > 0) {
        // Skip whitespace
        while (*model->cursor > 0 &&
               std::isspace((*model->buffer)[*model->cursor - 1]))
          (*model->cursor)--;
        // Skip word characters
        while (*model->cursor > 0 &&
               !std::isspace((*model->buffer)[*model->cursor - 1]))
          (*model->cursor)--;
      }
      return true;
    }

    // Handle Ctrl+Right Arrow (move to next word)
    if (raw == "\x1b[1;5C" || raw == "\x1b\x1b[C" || raw == std::string("\x1b") + "f") {
      int len = static_cast<int>(model->buffer->size());
      if (*model->cursor < len) {
        // Skip current word
        while (*model->cursor < len &&
               !std::isspace((*model->buffer)[*model->cursor]))
          (*model->cursor)++;
        // Skip whitespace
        while (*model->cursor < len &&
               std::isspace((*model->buffer)[*model->cursor]))
          (*model->cursor)++;
      }
      return true;
    }

    // Handle Ctrl+V for image paste from clipboard
    if (event == ftxui::Event::Character("\x16")) {
      // Check if clipboard has image
      if (!Clipboard::hasImage()) {
        return false; // No image in clipboard, let terminal handle
      }

      // Check if model supports vision
      if (model->check_vision_capable && !model->check_vision_capable()) {
        if (model->show_notification) {
          model->show_notification("Image Paste Failed",
                                   "Model not visually capable");
        }
        return true; // Consume the event
      }

      // Get image from clipboard
      std::string mimeType;
      auto imageData = Clipboard::getImage(mimeType);
      if (imageData) {
        // Generate placeholder text
        int img_num = static_cast<int>(model->image_tags.size()) + 1;
        std::string placeholder = "[Image " + std::to_string(img_num) + "]";
        
        // Insert placeholder at cursor position
        insertText(*model->buffer, *model->cursor, placeholder);
        
        // Track in pasted_blocks with position
        PastedBlock img_block;
        img_block.type = "image";
        img_block.id = generateBlockId();
        img_block.content = *imageData;
        img_block.mime_type = mimeType;
        img_block.start_pos = static_cast<size_t>(*model->cursor) - placeholder.size();
        img_block.end_pos = static_cast<size_t>(*model->cursor);
        model->pasted_blocks.push_back(img_block);
        
        // Also track in image_tags
        model->image_tags.push_back(img_block);
        return true;
      }
      return false;
    }

    // Handle Ctrl+Shift+V (explicit text paste) - various encodings
    if (raw == "\x1b[24;5~" || raw == "\x1b[24~" ||
        (raw.size() >= 2 && raw[0] == '\x1b' && raw[1] == 'V')) {
      return false; // Let terminal handle
    }

    // Handle Delete key - check for pasted block deletion
    if (event == ftxui::Event::Delete || event == ftxui::Event::Backspace) {
      // Check if cursor is inside or at edge of a pasted block
      int block_idx = -1;

      // For Delete key (forward delete)
      if (event == ftxui::Event::Delete) {
        block_idx = findBlockAtPos(model->pasted_blocks, *model->cursor);
        // Also check if we're right before a block
        if (block_idx < 0) {
          for (size_t i = 0; i < model->pasted_blocks.size(); ++i) {
            if (static_cast<int>(model->pasted_blocks[i].start_pos) ==
                *model->cursor) {
              block_idx = static_cast<int>(i);
              break;
            }
          }
        }
      }
      // For Backspace key
      else {
        // Check if cursor is at end of a block
        for (size_t i = 0; i < model->pasted_blocks.size(); ++i) {
          if (static_cast<int>(model->pasted_blocks[i].end_pos) ==
              *model->cursor) {
            block_idx = static_cast<int>(i);
            break;
          }
        }
        // Also check if inside a block
        if (block_idx < 0) {
          block_idx = findBlockAtPos(model->pasted_blocks,
                                     std::max(0, *model->cursor - 1));
        }
      }

      if (block_idx >= 0) {
        // Delete entire block
        removeBlock(*model->buffer, *model->cursor, model->pasted_blocks,
                    block_idx);
        return true;
      }

      return false;
    }

    // Handle autocomplete navigation
    auto ac = CommandManager::instance().getAutocomplete(*model->buffer);
    auto at_state =
        DetectAtReferenceAutocompleteState(*model->buffer, *model->cursor);
    auto at_suggestions = buildAtReferenceSuggestions(model, at_state);

    if (ac && ((ac->is_typing_command_name && !ac->command_matches.empty()) ||
               (!ac->is_typing_command_name && ac->current_arg &&
                (ac->current_arg->type == ArgType::Provider ||
                 ac->current_arg->type == ArgType::OAuthProvider ||
                 ac->current_arg->type == ArgType::ProviderId)))) {

      size_t match_count = 0;
      if (ac->is_typing_command_name) {
        match_count = std::min(ac->command_matches.size(), (size_t)5);
      } else {
        auto query = ac->has_current_arg_value ? ac->current_arg_value : "";
        match_count =
            buildProviderSuggestions(ac->current_arg->type, query).size();
      }

      if (match_count > 0) {
        if (event == ftxui::Event::ArrowDown) {
          *suggestion_index = (*suggestion_index + 1) % match_count;
          return true;
        }
        if (event == ftxui::Event::ArrowUp) {
          *suggestion_index =
              (*suggestion_index + match_count - 1) % match_count;
          return true;
        }
      } else {
        *suggestion_index = 0;
      }
    } else if (at_state.active && !at_suggestions.empty()) {
      const size_t match_count = at_suggestions.size();
      if (event == ftxui::Event::ArrowDown) {
        *suggestion_index = (*suggestion_index + 1) % match_count;
        return true;
      }
      if (event == ftxui::Event::ArrowUp) {
        *suggestion_index =
            (*suggestion_index + match_count - 1) % match_count;
        return true;
      }
    } else {
      *suggestion_index = 0;
    }

    const bool is_shift_enter = IsShiftEnterInput(raw);

    if (event == ftxui::Event::Tab || (event == ftxui::Event::Return &&
                                       !is_shift_enter)) {
      if (ac) {
        if (ac->is_typing_command_name && !ac->command_matches.empty()) {
          size_t idx = static_cast<size_t>(*suggestion_index);
          if (idx >= ac->command_matches.size())
            idx = 0;

          if (event == ftxui::Event::Return &&
              ac->command_matches[idx].is_exact) {
          } else {
            *model->buffer = "/" + ac->command_matches[idx].name + " ";
            *model->cursor = static_cast<int>(model->buffer->size());
            *suggestion_index = 0;
            return true;
          }
        } else if (!ac->is_typing_command_name && ac->current_arg &&
                   (ac->current_arg->type == ArgType::Provider ||
                    ac->current_arg->type == ArgType::OAuthProvider ||
                    ac->current_arg->type == ArgType::ProviderId)) {
          auto query = ac->has_current_arg_value ? ac->current_arg_value : "";
          auto suggestions =
              buildProviderSuggestions(ac->current_arg->type, query);
          if (!suggestions.empty()) {
            size_t idx = static_cast<size_t>(*suggestion_index);
            if (idx >= suggestions.size())
              idx = 0;

            std::string text = *model->buffer;
            size_t last_space = text.find_last_of(' ', *model->cursor - 1);
            if (last_space == std::string::npos)
              last_space = 0;
            else
              last_space++;

            text.replace(last_space, *model->cursor - last_space,
                         suggestions[idx].id + " ");
            *model->buffer = text;
            *model->cursor =
                static_cast<int>(last_space + suggestions[idx].id.size() + 1);
            *suggestion_index = 0;
            return true;
          }
        }
      } else if (event == ftxui::Event::Tab && at_state.active &&
                 !at_suggestions.empty()) {
        size_t idx = static_cast<size_t>(*suggestion_index);
        if (idx >= at_suggestions.size()) {
          idx = 0;
        }
        const std::string replacement =
            at_state.token_prefix + at_suggestions[idx] + " ";
        model->buffer->replace(at_state.token_start,
                               static_cast<size_t>(*model->cursor) -
                                   at_state.token_start,
                               replacement);
        *model->cursor =
            static_cast<int>(at_state.token_start + replacement.size());
        *suggestion_index = 0;
        return true;
      }
    }

    // Handle Shift+Enter (newline)
    if (is_shift_enter) {
      insertText(*model->buffer, *model->cursor, "\n");
      int cline = cursorLine(*model->buffer, *model->cursor);
      if (cline >= *scroll_top + MAX_VISIBLE_LINES) {
        *scroll_top = cline - MAX_VISIBLE_LINES + 1;
      }
      return true;
    }

    // Handle Enter (submit)
    if (event == ftxui::Event::Return) {
      if (!model->buffer->empty()) {
        // Expand pasted block placeholders to actual content before submitting
        std::string expanded = expandPastedContent(*model->buffer, model->pasted_blocks);
        on_submit(expanded, model->image_tags);
        model->buffer->clear();
        model->pasted_blocks.clear();
        model->image_tags.clear();
        *model->cursor = 0;
        *scroll_top = 0;
        // Mark as just submitted to skip next input processing
        *just_submitted = true;
        // Consume the event - don't let input component insert newline
        return true;
      }
      return true;
    }

    // Handle arrow keys for multi-line navigation
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

  return ftxui::Renderer(with_keys, [model, scroll_top, suggestion_index] {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    auto prompt =
        ftxui::text("> ") | ftxui::bold | ftxui::color(theme.input.prompt);

    // Render image tags above input
    std::optional<ftxui::Element> image_tags_el;
    if (model && !model->image_tags.empty()) {
      ftxui::Elements tag_elements;
      for (size_t i = 0; i < model->image_tags.size(); ++i) {
        auto tag = ftxui::text(" [Image " + std::to_string(i + 1) + "] ") |
                   ftxui::color(theme.agent_strip.pills.tool_fg) |
                   ftxui::bgcolor(theme.agent_strip.pills.tool_bg);
        tag_elements.push_back(tag);
      }
      image_tags_el = ftxui::hbox(tag_elements);
    }

    int total_lines = 1;
    if (model && model->buffer) {
      total_lines = countLines(*model->buffer);
    }

    auto ac = CommandManager::instance().getAutocomplete(
        model->buffer ? *model->buffer : "");
    auto at_state = DetectAtReferenceAutocompleteState(
        model->buffer ? *model->buffer : "",
        model->cursor ? *model->cursor : 0);
    auto at_suggestions = buildAtReferenceSuggestions(model, at_state);
    std::optional<ftxui::Element> autocomplete_layer;

    if (ac && ac->is_typing_command_name && !ac->command_matches.empty()) {
      std::vector<ftxui::Element> match_elements;
      for (size_t i = 0; i < ac->command_matches.size() && i < 5; ++i) {
        const auto &match = ac->command_matches[i];
        bool is_selected = (i == static_cast<size_t>(*suggestion_index));

        auto match_text = ftxui::text("/" + match.name) | ftxui::bold |
                          ftxui::color(is_selected ? ftxui::Color::Black
                                                   : ftxui::Color::White);
        if (is_selected)
          match_text = match_text | ftxui::bgcolor(theme.modals.highlight_bg) |
                       ftxui::color(theme.modals.highlight_fg);

        match_elements.push_back(
            ftxui::hbox({match_text, ftxui::text("  "),
                         ftxui::text(match.description) | ftxui::dim}));
      }
      autocomplete_layer =
          ftxui::vbox(match_elements) | ftxui::bgcolor(theme.modals.bg);
    } else if (ac && !ac->is_typing_command_name && ac->current_arg) {
      const auto &arg = *ac->current_arg;
      std::string type_str;
      switch (arg.type) {
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
      case ArgType::Provider:
        type_str = "Provider";
        break;
      case ArgType::OAuthProvider:
        type_str = "OAuth provider";
        break;
      case ArgType::ProviderId:
        type_str = "Provider ID";
        break;
      }

      auto header = ftxui::hbox(
          {ftxui::text(arg.name) | ftxui::bold |
               ftxui::color(ftxui::Color::Cyan),
           ftxui::text(" [" + type_str + (arg.optional ? ", opt" : "") + "] ") |
               ftxui::dim,
           ftxui::text(arg.description)});

      const bool wants_provider_suggestions =
          arg.type == ArgType::Provider || arg.type == ArgType::OAuthProvider ||
          arg.type == ArgType::ProviderId;
      if (wants_provider_suggestions) {
        std::string query =
            ac->has_current_arg_value ? ac->current_arg_value : std::string();
        auto suggestions = buildProviderSuggestions(arg.type, query);
        ftxui::Element suggestion_body;
        if (!suggestions.empty()) {
          ftxui::Elements rows;
          for (size_t i = 0; i < suggestions.size(); ++i) {
            const auto &match = suggestions[i];
            bool is_selected = (i == static_cast<size_t>(*suggestion_index));
            auto label =
                ftxui::text(" " + match.id) | ftxui::bold |
                ftxui::color(is_selected ? theme.base.bg : theme.base.fg);
            if (is_selected)
              label = label | ftxui::bgcolor(theme.modals.highlight_bg);
            auto meta = ftxui::text(" [" + match.type_label + "]") | ftxui::dim;
            rows.push_back(ftxui::hbox({label, ftxui::filler(), meta}));
          }
          suggestion_body =
              ftxui::vbox(rows) | ftxui::yframe | ftxui::color(theme.base.fg);
        } else {
          std::string message = query.empty()
                                    ? "No providers are registered yet."
                                    : "No providers match '" + query + "'";
          suggestion_body = ftxui::text(message) | ftxui::dim | ftxui::center;
        }

        autocomplete_layer =
            ftxui::vbox({header, ftxui::separatorLight(), suggestion_body}) |
            ftxui::bgcolor(theme.modals.bg);
      } else {
        autocomplete_layer = header | ftxui::bgcolor(theme.modals.bg);
      }
    } else if (at_state.active) {
      ftxui::Elements rows;
      if (!at_suggestions.empty()) {
        for (size_t i = 0; i < at_suggestions.size(); ++i) {
          const bool is_selected = (i == static_cast<size_t>(*suggestion_index));
          const std::string prefix = at_state.is_artifact ? "@artifact:" : "@";
          auto label =
              ftxui::text(" " + prefix + at_suggestions[i]) | ftxui::bold |
              ftxui::color(is_selected ? theme.base.bg : theme.base.fg);
          if (is_selected) {
            label = label | ftxui::bgcolor(theme.modals.highlight_bg);
          }
          rows.push_back(label);
        }
      } else {
        const std::string emptyLabel = at_state.is_artifact
                                           ? "No artifact matches"
                                           : "No file matches";
        rows.push_back(ftxui::text(emptyLabel) | ftxui::dim | ftxui::center);
      }

      const std::string headerLabel =
          at_state.is_artifact ? "Artifact references" : "File references";
      autocomplete_layer =
          ftxui::vbox({ftxui::text(headerLabel) | ftxui::bold,
                       ftxui::separatorLight(), ftxui::vbox(rows)}) |
          ftxui::bgcolor(theme.modals.bg);
    }

    // Build the input display with proper wrapping and pasted block rendering
    ftxui::Element input_display;

    auto with_cursor = [&](ftxui::Element e) {
      if (model && model->is_focused) {
        return e | ftxui::bgcolor(theme.input.fg) |
               ftxui::color(theme.input.bg) | ftxui::focus;
      }
      return e | ftxui::focus;
    };

    // Helper to render buffer with pasted blocks highlighted
    auto renderBufferWithBlocks = [&]() -> ftxui::Element {
      if (model->pasted_blocks.empty()) {
        const std::string &content = *model->buffer;
        int cursor = *model->cursor;

        if (content.empty()) {
          std::string ph = model->placeholder;
          if (ph.empty())
            ph = " ";
          return ftxui::hbox(
              {with_cursor(ftxui::text(ph.substr(0, 1))) | ftxui::dim,
               ftxui::text(ph.substr(1)) | ftxui::dim});
        }

        std::vector<std::string> raw_lines = splitLines(content);
        ftxui::Elements line_elements;

        int char_pos = 0;
        for (const auto &raw_line : raw_lines) {
          const size_t WRAP_WIDTH =
              std::max(20, ftxui::Terminal::Size().dimx - 10);
          size_t line_start = 0;

          if (raw_line.empty()) {
            if (cursor == char_pos) {
              line_elements.push_back(
                  ftxui::hbox({with_cursor(ftxui::text(" "))}));
            } else {
              line_elements.push_back(ftxui::text(""));
            }
          } else {
            while (line_start < raw_line.size()) {
              size_t line_len =
                  std::min(WRAP_WIDTH, raw_line.size() - line_start);
              std::string segment = raw_line.substr(line_start, line_len);

              int seg_start = char_pos;
              int seg_end = char_pos + segment.size();

              line_start += line_len;
              bool is_end_of_line = (line_start >= raw_line.size());

              bool cursor_here = false;
              if (is_end_of_line) {
                cursor_here = (cursor >= seg_start && cursor <= seg_end);
              } else {
                cursor_here = (cursor >= seg_start && cursor < seg_end);
              }

              ftxui::Element seg_elem;
              if (cursor_here) {
                int local_cursor = cursor - seg_start;
                if (local_cursor < static_cast<int>(segment.size())) {
                  seg_elem = ftxui::hbox(
                      {ftxui::text(segment.substr(0, local_cursor)),
                       with_cursor(
                           ftxui::text(segment.substr(local_cursor, 1))),
                       ftxui::text(segment.substr(local_cursor + 1))});
                } else {
                  seg_elem = ftxui::hbox(
                      {ftxui::text(segment), with_cursor(ftxui::text(" "))});
                }
              } else {
                seg_elem = ftxui::text(segment);
              }

              line_elements.push_back(seg_elem);
              char_pos += line_len;
            }
          }
          char_pos++; // Account for newline char
        }

        return ftxui::vbox(std::move(line_elements));
      }

      // Custom rendering to show pasted blocks with special styling
      const std::string &buf = *model->buffer;
      int cursor = *model->cursor;

      ftxui::Elements line_elements;
      std::vector<std::string> lines = splitLines(buf);

      size_t buf_pos = 0;
      for (size_t line_idx = 0; line_idx < lines.size(); ++line_idx) {
        const std::string &line = lines[line_idx];
        ftxui::Elements line_parts;

        size_t line_start = buf_pos;
        size_t line_end = buf_pos + line.size();

        // Check for pasted blocks on this line
        bool has_block = false;
        for (const auto &block : model->pasted_blocks) {
          if (block.type == "text" && block.start_pos >= line_start &&
              block.start_pos <= line_end) {
            // Render text before block
            if (block.start_pos > line_start) {
              std::string pre =
                  buf.substr(line_start, block.start_pos - line_start);
              bool cursor_here = (cursor >= static_cast<int>(line_start) &&
                                  cursor < static_cast<int>(block.start_pos));
              if (cursor_here) {
                int local_cursor = cursor - line_start;
                line_parts.push_back(ftxui::text(pre.substr(0, local_cursor)));
                line_parts.push_back(
                    with_cursor(ftxui::text(pre.substr(local_cursor, 1))));
                line_parts.push_back(ftxui::text(pre.substr(local_cursor + 1)));
              } else {
                line_parts.push_back(ftxui::text(pre));
              }
            }

            // Render block placeholder with special styling
            std::string placeholder =
                createTextBlockPlaceholder(block.line_count);
            bool cursor_in_block =
                (cursor >= static_cast<int>(block.start_pos) &&
                 cursor <= static_cast<int>(block.end_pos));
            auto block_el = ftxui::text(" " + placeholder + " ") |
                            ftxui::bgcolor(theme.agent_strip.pills.purpose_bg) |
                            ftxui::color(theme.agent_strip.pills.purpose_fg);
            if (cursor_in_block) {
              block_el = block_el | ftxui::underlined;
            }
            line_parts.push_back(block_el);

            has_block = true;
            buf_pos = block.end_pos;
            break;
          }
        }

        // Render rest of line if no block or after block
        if (!has_block) {
          bool cursor_here = (cursor >= static_cast<int>(line_start) &&
                              cursor <= static_cast<int>(line_end));
          if (cursor_here) {
            int local_cursor = cursor - line_start;
            // Fix: cursor position should be correct, not ahead
            if (local_cursor <= static_cast<int>(line.size())) {
              line_parts.push_back(ftxui::text(line.substr(0, local_cursor)));
              if (local_cursor < static_cast<int>(line.size())) {
                line_parts.push_back(
                    with_cursor(ftxui::text(line.substr(local_cursor, 1))));
                line_parts.push_back(
                    ftxui::text(line.substr(local_cursor + 1)));
              } else {
                // Cursor at end of line
                line_parts.push_back(with_cursor(ftxui::text(" ")));
              }
            } else {
              line_parts.push_back(ftxui::text(line));
            }
          } else {
            line_parts.push_back(ftxui::text(line));
          }
        } else {
          // Render text after block
          if (buf_pos < line_end) {
            std::string post = buf.substr(buf_pos, line_end - buf_pos);
            line_parts.push_back(ftxui::text(post));
          }
        }

        line_elements.push_back(ftxui::hbox(line_parts));
        buf_pos = line_end + 1; // +1 for newline
      }

      // Handle empty last line with cursor
      if (lines.empty() || (buf.size() > 0 && buf.back() == '\n')) {
        if (cursor == static_cast<int>(buf.size())) {
          line_elements.push_back(ftxui::hbox({with_cursor(ftxui::text(" "))}));
        }
      }

      return ftxui::vbox(std::move(line_elements));
    };

    if (total_lines <= MAX_VISIBLE_LINES) {
      *scroll_top = 0;
      input_display = renderBufferWithBlocks();
    } else {
      // Multi-line scrolling view
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

          // Fix: cursor should be at the correct position, not ahead by one
          if (cursor_col < len) {
            // Cursor is on a character
            pre = all_lines[i].substr(0, cursor_col);
            cur_char = all_lines[i][cursor_col];
            suf = all_lines[i].substr(cursor_col + 1);
          } else if (cursor_col == len) {
            // Cursor is at end of line - show space cursor
            pre = all_lines[i];
            cur_char = " ";
            suf = "";
          } else {
            // Cursor is beyond line - shouldn't happen but handle gracefully
            pre = all_lines[i];
            cur_char = " ";
            suf = "";
          }

          line_el = ftxui::hbox({
                        ftxui::text(pre),
                        with_cursor(ftxui::text(cur_char)),
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
                    std::to_string(total_lines) + " \xe2\x86\x95]";
      } else if (*scroll_top > 0) {
        indicator = " [" + std::to_string(cursor_line + 1) + "/" +
                    std::to_string(total_lines) + " \xe2\x86\x91]";
      } else if (end < static_cast<int>(all_lines.size())) {
        indicator = " [" + std::to_string(cursor_line + 1) + "/" +
                    std::to_string(total_lines) + " \xe2\x86\x93]";
      }

      auto line_hint = ftxui::text(indicator) | ftxui::dim |
                       ftxui::color(ftxui::Color::RGB(100, 100, 140));

      auto wrapped_content = ftxui::vbox(std::move(visible_elements));
      ftxui::Elements input_parts;
      input_parts.push_back(wrapped_content);
      input_parts.push_back(line_hint);
      input_display = ftxui::hbox(std::move(input_parts));
    }

    auto input_area = ftxui::hbox({prompt, input_display});

    ftxui::Elements root_elements;
    if (autocomplete_layer) {
      root_elements.push_back(*autocomplete_layer);
    }
    if (image_tags_el) {
      root_elements.push_back(*image_tags_el);
    }
    root_elements.push_back(input_area);

    return ftxui::vbox(std::move(root_elements));
  });
}

} // namespace firmius::tui
