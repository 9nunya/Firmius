#ifndef FIRMIUS_TUI_CONTROLLERS_APP_CONTROLLER_HPP
#define FIRMIUS_TUI_CONTROLLERS_APP_CONTROLLER_HPP

#include "models/TUIStore.hpp"
#include "models/TranscriptModel.hpp"
#include "Events.hpp"
#include <memory>

namespace firmius::core { class Harness; }
namespace firmius::tui { class TuiState; }

namespace firmius::tui {

class AppController {
public:
  static AppController& instance();

  void dispatch(const firmius::shared::AppEvent& event, firmius::core::Harness* harness);
  const firmius::shared::AgentMetrics& getSessionMetrics() const { return session_metrics_; }

private:
  AppController() = default;

  friend class firmius::tui::TuiState;
  firmius::shared::AgentMetrics session_metrics_;
};

} // namespace firmius::tui
#endif
