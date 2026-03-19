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

ftxui::Element ScrollableBoxComponent::OnRender() {
    if (!child_) return ftxui::text("");

    int viewport_w = 0;
    if (box_.x_max >= box_.x_min) {
        viewport_w = box_.x_max - box_.x_min + 1;
    }
    viewport_width_ = viewport_w;

    if (viewport_w > 0) {
        firmius::tui::SetMarkdownWidth(std::max(10, viewport_w - 4));
    }

    auto background = child_->Render();

    if (viewport_w > 0)
        background = background | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, viewport_w - 2);

    // Use the ACTUAL laid-out height from previous frame.
    // ComputeRequirement() calculates height BEFORE text wrapping (assuming infinite width),
    // which gives an underestimated size_. Using last_background_ gives us the true
    // height after FTXUI has performed layout with proper width constraints.
    if (last_background_) {
        size_ = last_background_->requirement().min_y;
    } else {
        background->ComputeRequirement();
        size_ = background->requirement().min_y;
    }

    int viewport_h = 0;
    if (box_.y_max >= box_.y_min) {
        viewport_h = box_.y_max - box_.y_min + 1;
    }

    int max_scroll = std::max(0, size_ - viewport_h);

    if (at_bottom_) {
        selected_ = max_scroll;
    }
    selected_ = std::clamp(selected_, 0, max_scroll);

    int external_dimy = std::max(0, viewport_h - 1);
    int focus_y = selected_ + external_dimy / 2;

    auto frame = background
        | ftxui::focusPosition(0, focus_y)
        | ftxui::frame
        | ftxui::vscroll_indicator
        | ftxui::yflex
        | ftxui::reflect(box_);

    // Save background for next frame's size calculation
    last_background_ = background;

    return frame;
}

bool ScrollableBoxComponent::OnEvent(ftxui::Event event) {
    if (event.is_mouse() && box_.Contain(event.mouse().x, event.mouse().y)) {
        TakeFocus();
    }

    int previous = selected_;
    int viewport_h = 0;
    if (box_.y_max >= box_.y_min) {
        viewport_h = box_.y_max - box_.y_min + 1;
    }

    if (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelUp) {
        selected_ -= 5;
    }
    if (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelDown) {
        selected_ += 5;
    }
    if (event == ftxui::Event::PageDown) {
        selected_ += std::max(1, viewport_h - 2);
    }
    if (event == ftxui::Event::PageUp) {
        selected_ -= std::max(1, viewport_h - 2);
    }
    if (event == ftxui::Event::ArrowDown) {
        selected_ += 1;
    }
    if (event == ftxui::Event::ArrowUp) {
        selected_ -= 1;
    }
    if (event == ftxui::Event::Home) {
        selected_ = 0;
    }
    const bool is_keyboard_scroll =
        event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown ||
        event == ftxui::Event::PageUp || event == ftxui::Event::PageDown ||
        event == ftxui::Event::Home || event == ftxui::Event::End;

    // Use same max_scroll formula as OnRender
    int max_scroll = std::max(0, size_ - viewport_h);

    if (event == ftxui::Event::End) {
        selected_ = max_scroll;
        at_bottom_ = true;
    }

    selected_ = std::clamp(selected_, 0, max_scroll);
    if (previous != selected_ && event != ftxui::Event::End) {
        at_bottom_ = (selected_ >= max_scroll);
    }

    bool child_handled = ComponentBase::OnEvent(event);
    return previous != selected_ || child_handled || is_keyboard_scroll;
}

bool ScrollableBoxComponent::Focusable() const {
    return true;
}

std::shared_ptr<ScrollableBoxComponent> ScrollableBox(ftxui::Component child) {
    return ftxui::Make<ScrollableBoxComponent>(std::move(child));
}

} // namespace firmius::tui
