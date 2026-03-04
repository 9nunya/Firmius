#include "EventBridge.hpp"

namespace firmius::tui {

EventBridge::EventBridge(AppState& state, ftxui::ScreenInteractive& screen)
    : state_(state), screen_(screen) {}

void EventBridge::push(const firmius::harness::HarnessEvent& event) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.push(event);
    }
    screen_.PostEvent(ftxui::Event::Custom);
}

void EventBridge::drain() {
    std::queue<firmius::harness::HarnessEvent> localQueue;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (queue_.empty()) {
            return;
        }
        std::swap(localQueue, queue_);
    }

    while (!localQueue.empty()) {
        state_.applyEvent(localQueue.front());
        localQueue.pop();
    }
}

}
