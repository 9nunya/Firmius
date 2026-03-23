#include "components/ScrollableBox.hpp"

#include "components/Markdown.hpp"
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <algorithm>

namespace firmius::tui {

ScrollableBoxComponent::ScrollableBoxComponent(ftxui::Component child,
                                               ScrollableBoxOptions options)
    : options_(options), child_(std::move(child)),
      at_bottom_(options.startAtBottom) {
    if (child_) {
        Add(child_);
    }
}

void ScrollableBoxComponent::RequestScrollToBottom() {
    at_bottom_ = true;
}

void ScrollableBoxComponent::RequestScrollToTop() {
    at_bottom_ = false;
    selected_ = 0;
}

int ScrollableBoxComponent::maxScroll(int viewportHeight) const {
    return std::max(0, size_ - viewportHeight);
}

void ScrollableBoxComponent::syncBottomFlag() {
    at_bottom_ = (selected_ >= last_max_scroll_);
}

void ScrollableBoxComponent::updateScrollbarGeometry(int viewportHeight) {
    viewport_height_ = std::max(0, viewportHeight);
    last_max_scroll_ = maxScroll(viewport_height_);
    scrollbar_thumb_height_ = 0;
    scrollbar_thumb_top_ = 0;

    if (!options_.overlayScrollbar || viewport_height_ <= 0 ||
        last_max_scroll_ <= 0) {
        return;
    }

    scrollbar_thumb_height_ =
        std::clamp((viewport_height_ * viewport_height_) / std::max(1, size_),
                   1, viewport_height_);
    const int travel = std::max(0, viewport_height_ - scrollbar_thumb_height_);
    if (travel == 0 || last_max_scroll_ == 0) {
        scrollbar_thumb_top_ = 0;
        return;
    }

    scrollbar_thumb_top_ =
        std::clamp((selected_ * travel) / last_max_scroll_, 0, travel);
}

void ScrollableBoxComponent::scrollFromScrollbarY(int y) {
    if (!options_.overlayScrollbar || viewport_height_ <= 0 ||
        last_max_scroll_ <= 0) {
        return;
    }

    const int trackTop = scrollbarBox_.y_min;
    const int clampedY =
        std::clamp(y, trackTop, trackTop + std::max(0, viewport_height_ - 1));
    const int travel = std::max(0, viewport_height_ - scrollbar_thumb_height_);
    if (travel == 0) {
        selected_ = 0;
        syncBottomFlag();
        return;
    }

    const int thumbTop =
        std::clamp(clampedY - trackTop - scrollbar_drag_offset_, 0, travel);
    selected_ = (thumbTop * last_max_scroll_) / travel;
    selected_ = std::clamp(selected_, 0, last_max_scroll_);
    syncBottomFlag();
    updateScrollbarGeometry(viewport_height_);
}

ftxui::Element ScrollableBoxComponent::OnRender() {
    if (!child_) {
        return ftxui::text("");
    }

    int viewport_w = 0;
    if (box_.x_max >= box_.x_min) {
        viewport_w = box_.x_max - box_.x_min + 1;
    }
    viewport_width_ = viewport_w;

    const int contentWidth = std::max(
        1, viewport_w - (options_.overlayScrollbar ? 0 : 2));
    if (contentWidth > 0) {
        firmius::tui::SetMarkdownWidth(std::max(10, contentWidth - 2));
    }

    auto background = child_->Render();
    if (viewport_w > 0) {
        background =
            background | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth);
    }

    background->ComputeRequirement();
    size_ = background->requirement().min_y;

    int viewport_h = 0;
    if (box_.y_max >= box_.y_min) {
        viewport_h = box_.y_max - box_.y_min + 1;
    }

    if (at_bottom_) {
        selected_ = maxScroll(viewport_h);
    }
    selected_ = std::clamp(selected_, 0, maxScroll(viewport_h));
    updateScrollbarGeometry(viewport_h);

    const int external_dimy = std::max(0, viewport_h - 1);
    const int focus_y = selected_ + external_dimy / 2;

    auto frame =
        background | ftxui::focusPosition(0, focus_y) | ftxui::frame |
        ftxui::yflex | ftxui::reflect(box_);

    if (!options_.overlayScrollbar) {
        frame = frame | ftxui::vscroll_indicator;
        return frame;
    }

    const auto trackColor = scrollbar_hovered_
                                ? ftxui::Color::RGB(58, 64, 78)
                                : ftxui::Color::RGB(46, 51, 63);
    const auto thumbColor =
        (scrollbar_dragging_ || scrollbar_hovered_)
            ? ftxui::Color::RGB(201, 158, 191)
            : ftxui::Color::RGB(112, 119, 138);
    ftxui::Elements scrollbarRows;
    scrollbarRows.reserve(std::max(0, viewport_height_));
    for (int row = 0; row < viewport_height_; ++row) {
        const bool inThumb =
            row >= scrollbar_thumb_top_ &&
            row < scrollbar_thumb_top_ + scrollbar_thumb_height_;
        auto cell = ftxui::text(" ");
        if (inThumb) {
            cell = cell | ftxui::bgcolor(thumbColor);
        } else {
            cell = cell | ftxui::bgcolor(trackColor);
        }
        scrollbarRows.push_back(cell);
    }

    auto scrollbar =
        ftxui::vbox(std::move(scrollbarRows)) |
        ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 1) |
        ftxui::reflect(scrollbarBox_) | ftxui::align_right;

    return ftxui::dbox({frame, scrollbar});
}

bool ScrollableBoxComponent::OnEvent(ftxui::Event event) {
    if (event.is_mouse()) {
        const auto mouse = event.mouse();
        const bool inMainBox = box_.Contain(mouse.x, mouse.y);
        const bool inScrollbar =
            options_.overlayScrollbar && scrollbarBox_.Contain(mouse.x, mouse.y);
        scrollbar_hovered_ = inScrollbar || scrollbar_dragging_;

        if (inMainBox || inScrollbar) {
            TakeFocus();
        }

        if (options_.overlayScrollbar && captured_mouse_) {
            if (mouse.motion == ftxui::Mouse::Released) {
                captured_mouse_.reset();
                scrollbar_dragging_ = false;
                scrollbar_drag_offset_ = 0;
                scrollbar_hovered_ = scrollbarBox_.Contain(mouse.x, mouse.y);
                return true;
            }
            scrollFromScrollbarY(mouse.y);
            return true;
        }

        if (options_.overlayScrollbar && mouse.button == ftxui::Mouse::Left &&
            mouse.motion == ftxui::Mouse::Pressed && inScrollbar &&
            !captured_mouse_) {
            captured_mouse_ = CaptureMouse(event);
            if (!captured_mouse_) {
                return false;
            }
            scrollbar_dragging_ = true;
            const int thumbStart = scrollbarBox_.y_min + scrollbar_thumb_top_;
            const int thumbEnd = thumbStart + std::max(1, scrollbar_thumb_height_) - 1;
            if (mouse.y >= thumbStart && mouse.y <= thumbEnd) {
                scrollbar_drag_offset_ = mouse.y - thumbStart;
            } else {
                scrollbar_drag_offset_ = scrollbar_thumb_height_ / 2;
            }
            scrollFromScrollbarY(mouse.y);
            return true;
        }
    }

    const int previous = selected_;

    if (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelUp) {
        selected_ -= 5;
        at_bottom_ = false;
    }
    if (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelDown) {
        selected_ += 5;
        at_bottom_ = false;
    }
    if (event == ftxui::Event::PageDown) {
        selected_ += std::max(1, viewport_height_ - 2);
        at_bottom_ = false;
    }
    if (event == ftxui::Event::PageUp) {
        selected_ -= std::max(1, viewport_height_ - 2);
        at_bottom_ = false;
    }
    if (event == ftxui::Event::ArrowDown) {
        selected_ += 1;
        at_bottom_ = false;
    }
    if (event == ftxui::Event::ArrowUp) {
        selected_ -= 1;
        at_bottom_ = false;
    }
    if (event == ftxui::Event::Home) {
        selected_ = 0;
        at_bottom_ = false;
    }

    const bool isKeyboardScroll =
        event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown ||
        event == ftxui::Event::PageUp || event == ftxui::Event::PageDown ||
        event == ftxui::Event::Home || event == ftxui::Event::End;

    if (event == ftxui::Event::End) {
        selected_ = maxScroll(viewport_height_);
        at_bottom_ = true;
    }

    selected_ = std::clamp(selected_, 0, maxScroll(viewport_height_));
    syncBottomFlag();
    updateScrollbarGeometry(viewport_height_);

    const bool childHandled = ComponentBase::OnEvent(event);
    return previous != selected_ || childHandled || isKeyboardScroll ||
           scrollbar_dragging_;
}

bool ScrollableBoxComponent::Focusable() const {
    return true;
}

std::shared_ptr<ScrollableBoxComponent> ScrollableBox(ftxui::Component child,
                                                      ScrollableBoxOptions options) {
    return ftxui::Make<ScrollableBoxComponent>(std::move(child), options);
}

} // namespace firmius::tui
