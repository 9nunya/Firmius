#ifndef FIRMIUS_TUI_UTILS_UI_ACTIONS_HPP
#define FIRMIUS_TUI_UTILS_UI_ACTIONS_HPP

#include "Context.hpp"
#include "Events.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace firmius::tui {

struct UiCoreEventReceived {
  firmius::shared::AppEvent event;
};

struct UiPromptSubmitted {
  std::string text;
  std::vector<firmius::shared::ImageContent> images;
};

struct UiThreadOpenRequested {
  std::optional<std::string> thread_id;
  bool resume_last = false;
  bool preserve_live_state = true;
  std::string loading_message;
  std::string loading_detail;
};

struct UiThreadOpened {
  firmius::shared::ThreadMetadata metadata;
  std::string focused_agent_id;
  bool preserve_live_state = true;
};

struct UiThreadOpenFailed {
  std::string title = "Thread";
  std::string message;
  bool return_to_welcome = false;
};

struct UiDeferredMutation {
  std::function<void()> action;
};

struct UiTick {
  std::chrono::steady_clock::time_point now;
  std::chrono::milliseconds dt{0};
};

using UiAction = std::variant<UiCoreEventReceived, UiPromptSubmitted,
                              UiThreadOpenRequested, UiThreadOpened,
                              UiThreadOpenFailed, UiDeferredMutation, UiTick>;

} // namespace firmius::tui

#endif
