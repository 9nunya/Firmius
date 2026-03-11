#include "components/ScrollableBox.hpp"

#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <algorithm>

namespace firmius::tui {

ScrollableBoxComponent::ScrollableBoxComponent(ftxui::Component child)
    : child_(std::move(child)) {
    if (child_) Add(child_);
}

void ScrollableBoxComponent::RequestScrollToBottom() {
    at_bottom_ = true;
}

ftxui::Element ScrollableBoxComponent::Render() {
    if (!child_) return ftxui::text("");

    auto background = child_->Render();
    background->ComputeRequirement();
    // Calculate proper content size
    size_ = background->requirement().min_y + background->requirement().flex_grow_y;
    if (at_bottom_) {
        selected_ = size_;
    }
    auto frame = background
        | ftxui::focusPosition(0, selected_)
        | ftxui::frame
        | ftxui::vscroll_indicator
        | ftxui::yflex
        | ftxui::reflect(box_);
    // Update viewport width for responsive layout
    if (box_.x_max >= box_.x_min) {
        viewport_width_ = box_.x_max - box_.x_min + 1;
    }
    return frame;
}

bool ScrollableBoxComponent::OnEvent(ftxui::Event event) {
    if (event.is_mouse() && box_.Contain(event.mouse().x, event.mouse().y)) {
        TakeFocus();
    }

    int previous = selected_;
    bool scroll_handled = false;
    int wheel_step = std::max(1, (box_.y_max - box_.y_min) / 2);

    if (event == ftxui::Event::ArrowUp ||
        (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelUp)) {
        selected_ -= wheel_step;
        scroll_handled = true;
    }
    if (event == ftxui::Event::ArrowDown ||
        (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelDown)) {
            if (at_bottom_ && event.mouse().button == ftxui::Mouse::WheelDown) return scroll_handled;
        selected_ += wheel_step;
        scroll_handled = true;
    }
    if (event == ftxui::Event::PageDown) {
        selected_ += box_.y_max - box_.y_min;
        scroll_handled = true;
    }
    if (event == ftxui::Event::PageUp) {
        selected_ -= box_.y_max - box_.y_min;
        scroll_handled = true;
    }
    if (event == ftxui::Event::Home) {
        selected_ = 0;
        scroll_handled = true;
    }
    if (event == ftxui::Event::End) {
        selected_ = size_;
        scroll_handled = true;
        at_bottom_ = true;
    }

    selected_ = std::clamp(selected_, 0, std::max(0, size_ - 1));
    if (scroll_handled && event != ftxui::Event::End) {
        at_bottom_ = false;
    }
    bool child_handled = ComponentBase::OnEvent(event);
    return scroll_handled || child_handled || (previous != selected_);
}

bool ScrollableBoxComponent::Focusable() const {
    return true;
}

std::shared_ptr<ScrollableBoxComponent> ScrollableBox(ftxui::Component child) {
    return ftxui::Make<ScrollableBoxComponent>(std::move(child));
}

} // namespace firmius::tui
