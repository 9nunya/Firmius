#include "components/ScrollableBox.hpp"

#include "components/Markdown.hpp"
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

    // Update viewport width for responsive layout
    int viewport_w = 0;
    if (box_.x_max >= box_.x_min) {
        viewport_w = box_.x_max - box_.x_min + 1;
    }
    viewport_width_ = viewport_w;

    if (viewport_w > 0) {
        firmius::tui::SetMarkdownWidth(std::max(10, viewport_w - 4));
    }

    auto background = child_->Render();

    // Constrain content width to viewport to prevent horizontal overflow
    if (viewport_w > 0)
        background = background | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, viewport_w - 2);

    // Spacer line to allow the final row to scroll fully into view.
    background = ftxui::vbox({background, ftxui::text("")});

    background->ComputeRequirement();
    size_ = background->requirement().min_y;

    int max_scroll = std::max(0, size_ + 30);

    if (at_bottom_) selected_ = max_scroll;
    selected_ = std::clamp(selected_, 0, max_scroll);

    auto frame = background
        | ftxui::focusPosition(0, selected_)
        | ftxui::frame
        | ftxui::vscroll_indicator
        | ftxui::yflex
        | ftxui::reflect(box_);

    return frame;
}

bool ScrollableBoxComponent::OnEvent(ftxui::Event event) {
    if (event.is_mouse() && box_.Contain(event.mouse().x, event.mouse().y)) {
        TakeFocus();
    }

    int previous = selected_;

    if (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelUp) {
        selected_ -= 5;
    }
    if (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelDown) {
        selected_ += 5;
    }
    if (event == ftxui::Event::PageDown) {
        selected_ += 5;
    }
    if (event == ftxui::Event::PageUp) {
        selected_ -= 5;
    }
    if (event == ftxui::Event::Home) {
        selected_ = 0;
    }
    if (event == ftxui::Event::End) {
        selected_ = size_;
        at_bottom_ = true;
    }

    int viewport_h = 0;
    if (box_.y_max >= box_.y_min) {
        viewport_h = box_.y_max - box_.y_min + 1;
    }
    int max_scroll = 0;
    if (viewport_h > 0) {
        max_scroll = std::max(0, size_ + 30);
    }
    selected_ = std::clamp(selected_, 0, max_scroll);
    if (previous != selected_ && event != ftxui::Event::End) {
        at_bottom_ = (selected_ >= max_scroll);
    }
    bool child_handled = ComponentBase::OnEvent(event);

    return previous != selected_ || child_handled || (previous != selected_);
}

bool ScrollableBoxComponent::Focusable() const {
    return true;
}

std::shared_ptr<ScrollableBoxComponent> ScrollableBox(ftxui::Component child) {
    return ftxui::Make<ScrollableBoxComponent>(std::move(child));
}

} // namespace firmius::tui
