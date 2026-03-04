#pragma once

#include <queue>
#include <mutex>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/event.hpp>
#include "HarnessEvents.hpp"
#include "AppState.hpp"

namespace firmius::tui {

class EventBridge {
public:
    EventBridge(AppState& state, ftxui::ScreenInteractive& screen);

    void push(const firmius::harness::HarnessEvent& event);

    void drain();

private:
    AppState& state_;
    ftxui::ScreenInteractive& screen_;
    std::queue<firmius::harness::HarnessEvent> queue_;
    mutable std::mutex queueMutex_;
};

}
