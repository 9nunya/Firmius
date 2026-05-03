#ifndef FIRMIUS_TUI_CONTROLLERS_TRANSCRIPT_CONTROLLER_HPP
#define FIRMIUS_TUI_CONTROLLERS_TRANSCRIPT_CONTROLLER_HPP

#include "models/TranscriptModel.hpp"
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>

namespace firmius::tui {

class TranscriptController {
public:
  static TranscriptController& instance();
  void expandHistoryForTranscriptIfNeeded();
  void selectEditableMessageByTimestamp(uint64_t timestamp);
  void rebuildEditableUserMessages();
  void commitSelectedEditableMessageToInput();
  void toggleEditMode();
  void selectPreviousEditableMessage();
  void selectNextEditableMessage();
  std::vector<ftxui::Element> generateLiveRows();

private:
  TranscriptController() = default;

  // Holds Components produced inside `generateLiveRows()` for the duration of
  // a render cycle. ToolBlock components own a `Box box_` that the rendered
  // Element captures via `ftxui::reflect(box_)`. If the Component is destroyed
  // before ftxui's layout pass writes through that reference, we get a
  // heap-use-after-free in `Reflect::SetBox`. Keeping the Components alive in
  // this vector across renders extends their lifetime past Draw().
  std::vector<ftxui::Component> live_components_;

  // Memoized live rows for the focused transcript.
  std::string last_live_agent_id_;
  std::string last_live_thread_id_;
  uint64_t last_live_epoch_ = 0;
  std::vector<ftxui::Element> cached_live_rows_;
  std::vector<ftxui::Component> cached_live_components_;
};

} // namespace firmius::tui

#endif
