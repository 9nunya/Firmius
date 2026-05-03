#ifndef FIRMIUS_COMPONENTS_INPUT_BAR_HPP
#define FIRMIUS_COMPONENTS_INPUT_BAR_HPP

#include <algorithm>
#include <cctype>
#include <ftxui/component/component_base.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {

struct PastedBlock {
  std::string type;      // "image" or "text"
  std::string id;        // unique identifier
  size_t line_count = 0; // for text pastes
  std::string content;   // actual pasted content (for text or image base64)
  std::string mime_type; // MIME type for images (e.g., "image/png")
  size_t start_pos = 0;  // position in buffer where placeholder starts
  size_t end_pos = 0;    // position in buffer where placeholder ends
};

struct InputBarModel {
  std::string *buffer = nullptr;
  int *cursor = nullptr;
  bool *help_opened_from_empty_query = nullptr;
  bool *command_palette_requested = nullptr;
  std::string placeholder;
  std::vector<PastedBlock> pasted_blocks; // blocks embedded in buffer
  bool is_focused = true;
  
  // Function to check if current model supports vision
  // Returns true if model supports image input, false otherwise
  bool compact_mode = false;
  std::function<bool()> check_vision_capable = nullptr;
  
  // Function to show notification
  std::function<void(const std::string& title, const std::string& message)> show_notification = nullptr;

  // Returns file-path completions for @file references.
  std::function<std::vector<std::string>(const std::string& query)>
      complete_file_references = nullptr;

  // Returns artifact completions for @artifact: references.
  std::function<std::vector<std::string>(const std::string& query)>
      complete_artifact_references = nullptr;
};

struct AtReferenceAutocompleteState {
  bool active = false;
  bool is_artifact = false;
  size_t token_start = 0;
  std::string query;
  std::string token_prefix;
};

inline AtReferenceAutocompleteState
DetectAtReferenceAutocompleteState(const std::string &buffer, int cursor) {
  AtReferenceAutocompleteState state;
  const int safeCursor =
      std::max(0, std::min(cursor, static_cast<int>(buffer.size())));
  size_t start = static_cast<size_t>(safeCursor);
  while (start > 0 &&
         !std::isspace(static_cast<unsigned char>(buffer[start - 1]))) {
    --start;
  }

  const std::string token =
      buffer.substr(start, safeCursor - static_cast<int>(start));
  if (token.rfind("@artifact:", 0) == 0) {
    state.active = true;
    state.is_artifact = true;
    state.token_start = start;
    state.token_prefix = "@artifact:";
    state.query = token.substr(std::char_traits<char>::length("@artifact:"));
    return state;
  }
  if (!token.empty() && token[0] == '@') {
    state.active = true;
    state.is_artifact = false;
    state.token_start = start;
    state.token_prefix = "@";
    state.query = token.substr(1);
  }
  return state;
}

inline bool IsShiftEnterInput(const std::string &raw) {
  return raw == "\x1b[13;2u" || raw == "\x1b\r" || raw == "\x1b\n" ||
         raw == "\x1b[27;2;13~";
}

ftxui::Component InputBar(
    const std::shared_ptr<InputBarModel> &model,
    std::function<void(const std::string &,
                       const std::vector<PastedBlock> &images)>
        on_submit,
    std::function<void()> on_escape = nullptr);

} // namespace firmius::tui

#endif
