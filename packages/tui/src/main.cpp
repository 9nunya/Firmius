#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <iostream>
#include <string>
 

using namespace ftxui;

int main() {
    auto &h = firmius::core::Harness::instance();
    h.init();

    auto threads = h.listThreads();
    if (threads.empty()) {
        std::cerr << "No threads found. Create a thread in another client first." << std::endl;
        return 1;
    }

    const auto &thread = threads.front();
    if (!h.switchThread(thread.threadId)) {
        std::cerr << "Failed to switch to thread: " << thread.threadId << std::endl;
        return 1;
    }

    const auto focused = h.focusedAgentId();
    if (focused.empty()) {
        std::cerr << "No focused agent for thread: " << thread.threadId << std::endl;
        return 1;
    }

    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(true);
    auto& state = firmius::tui::TuiState::instance();
    state.init(h, thread, focused);
    state.attachScreen(&screen);

    auto renderer = state.root();
    renderer = CatchEvent(renderer, [&](Event event) {
        if (event.is_character() && event.character() == std::string(1, '\x03')) {
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    screen.Loop(renderer);
    state.shutdown();

    return 0;
}
