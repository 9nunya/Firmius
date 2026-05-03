#ifndef FIRMIUS_TUI_TRANSCRIPT_MODEL_HPP
#define FIRMIUS_TUI_TRANSCRIPT_MODEL_HPP

#include "Context.hpp"
#include <memory>
#include <vector>
#include <functional>

namespace firmius::tui {

struct EditableUserMessage {
  uint64_t timestamp = 0;
  std::string text;
  std::vector<firmius::shared::ImageContent> images;
};

class TranscriptModel {
public:
  static TranscriptModel& instance();

  std::shared_ptr<firmius::shared::AgentHistory> active_history;
  
  bool edit_mode_active = false;
  std::vector<EditableUserMessage> editable_user_messages;
  int selected_editable_message_index = -1;
  std::optional<EditableUserMessage> pending_edit_message;

  std::function<void()> on_history_changed;
  std::function<void()> on_edit_mode_changed;

private:
  TranscriptModel() = default;
};

} // namespace firmius::tui

#endif
