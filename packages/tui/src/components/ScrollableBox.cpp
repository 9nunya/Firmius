#include "components/ScrollableBox.hpp"

#include "components/Markdown.hpp"
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/animation.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <algorithm>

namespace firmius::tui {
void noteTuiRequestAnimationFrameFromScrollableBoxWidthChange() __attribute__((weak));
}

namespace {
inline void NoteScrollableRafIfAvailable() {
  if (firmius::tui::noteTuiRequestAnimationFrameFromScrollableBoxWidthChange) {
    firmius::tui::noteTuiRequestAnimationFrameFromScrollableBoxWidthChange();
  }
}
}

namespace firmius::tui {
namespace {
constexpr auto kScrollbarVisibleDwell = std::chrono::milliseconds(1800);

inline void TouchTransientScrollbar(bool &visible,
                                    std::chrono::steady_clock::time_point &until) {
  visible = true;
  until = std::chrono::steady_clock::now() + kScrollbarVisibleDwell;
  ftxui::animation::RequestAnimationFrame();
  NoteScrollableRafIfAvailable();
}
}


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

void ScrollableBoxComponent::RequestEnsureVisible(int line) {
    RequestEnsureVisible(line, line);
}

void ScrollableBoxComponent::RequestEnsureVisible(int first_line,
                                                  int last_line) {
    has_pending_ensure_visible_ = true;
    pending_visible_start_ = std::max(0, std::min(first_line, last_line));
    pending_visible_end_ = std::max(0, std::max(first_line, last_line));
}

void ScrollableBoxComponent::InvalidateLayout() {
    layout_dirty_ = true;
}

int ScrollableBoxComponent::ContentWidth() const {
    const int overlayGutter =
        options_.overlayScrollbar ? std::max(1, options_.overlayScrollbarGutter)
                                  : 2;
    return std::max(1, viewport_width_ - overlayGutter);
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
    if (viewport_w != last_rendered_viewport_w_) {
        last_rendered_viewport_w_ = viewport_w;
    }
    viewport_width_ = viewport_w;

    const int overlayGutter =
        options_.overlayScrollbar ? std::max(1, options_.overlayScrollbarGutter)
                                  : 2;
    const int contentWidth = std::max(1, viewport_w - overlayGutter);
    if (contentWidth > 0) {
        firmius::tui::SetMarkdownWidth(std::max(10, contentWidth - 2));
    }

    auto background = child_->Render();
    if (viewport_w > 0) {
        background =
            background | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth);
    }

    const std::size_t measurement_signature =
        options_.measurement_signature_getter
            ? options_.measurement_signature_getter()
            : 0;
    const int old_size = size_;
    if (layout_dirty_ || measured_viewport_width_ != viewport_w ||
        (options_.measurement_signature_getter &&
         measured_signature_ != measurement_signature)) {
        // Measure using FTXUI's iterative layout pass so wrapped paragraph/flexbox
        // content reports its true rendered height for the current width.
        const auto fitted = ftxui::Dimension::Fit(background, true);
        size_ = std::max(0, fitted.dimy);
        measured_viewport_width_ = viewport_w;
        measured_signature_ = measurement_signature;
        layout_dirty_ = false;
    }

    // Preserve scroll position proportionally when content height changes
    // (e.g. Ctrl+G toggling diff expansion/collapse).
    if (size_ != old_size && old_size > 0 && !at_bottom_ &&
        !has_pending_ensure_visible_) {
        const double ratio =
            static_cast<double>(selected_) / static_cast<double>(old_size);
        selected_ = static_cast<int>(ratio * size_);
    }

    int viewport_h = 0;
    if (box_.y_max >= box_.y_min) {
        viewport_h = box_.y_max - box_.y_min + 1;
    }

    const int max_scroll = maxScroll(viewport_h);
    if (has_pending_ensure_visible_) {
        // Defer applying ensure-visible until we know the viewport height.
        // During the first render pass (before `reflect(box_)` has populated
        // geometry), viewport_h may be 0.
        if (viewport_h > 0) {
            at_bottom_ = false;
            if (pending_visible_start_ < selected_) {
                selected_ = pending_visible_start_;
            } else if (pending_visible_end_ >= selected_ + viewport_h) {
                selected_ = pending_visible_end_ - viewport_h + 1;
            }
            has_pending_ensure_visible_ = false;
        }
    } else if (at_bottom_) {
        selected_ = max_scroll;
    }
    selected_ = std::clamp(selected_, 0, max_scroll);
    updateScrollbarGeometry(viewport_h);

    const int focus_y = at_bottom_ ? std::max(0, size_ - 1) : selected_;

    auto frame =
        background | ftxui::focusPosition(0, focus_y) | ftxui::frame |
        ftxui::yflex | ftxui::reflect(box_);

    if (!options_.showScrollbar) {
        return frame;
    }

    if (!options_.overlayScrollbar) {
        frame = frame | ftxui::vscroll_indicator;
        return frame;
    }

    const auto trackColor = scrollbar_hovered_
                                ? ftxui::Color::RGB(58, 64, 78)
                                : ftxui::Color::RGB(46, 51, 63);
    if (options_.overlayScrollbar) {
        const auto now = std::chrono::steady_clock::now();
        if (scrollbar_dragging_ || scrollbar_hovered_) {
            TouchTransientScrollbar(scrollbar_visible_, scrollbar_visible_until_);
        } else if (scrollbar_visible_ && now > scrollbar_visible_until_) {
            scrollbar_visible_ = false;
        } else if (scrollbar_visible_) {
            ftxui::animation::RequestAnimationFrame();
        }
    }

    const auto thumbColor =
        (scrollbar_dragging_ || scrollbar_hovered_)
            ? ftxui::Color::RGB(201, 158, 191)
            : ftxui::Color::RGB(112, 119, 138);
    ftxui::Elements scrollbarRows;
    if (!scrollbar_visible_ && !scrollbar_dragging_ && !scrollbar_hovered_) {
        return frame;
    }

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
    bool mouse_in_main = false;
    bool mouse_in_scrollbar = false;
    if (event.is_mouse()) {
        const auto mouse = event.mouse();
        mouse_in_main = box_.Contain(mouse.x, mouse.y);
        mouse_in_scrollbar =
            options_.overlayScrollbar && scrollbarBox_.Contain(mouse.x, mouse.y);
        scrollbar_hovered_ = mouse_in_scrollbar || scrollbar_dragging_;

        if (mouse_in_main || mouse_in_scrollbar) {
            TakeFocus();
        }

        if (!captured_mouse_ &&
            (mouse.button == ftxui::Mouse::WheelUp ||
             mouse.button == ftxui::Mouse::WheelDown) &&
            !mouse_in_main && !mouse_in_scrollbar) {
            return false;
        }

        if (options_.overlayScrollbar && captured_mouse_) {
            if (mouse.motion == ftxui::Mouse::Released) {
                captured_mouse_.reset();
                scrollbar_dragging_ = false;
                scrollbar_drag_offset_ = 0;
                scrollbar_hovered_ = scrollbarBox_.Contain(mouse.x, mouse.y);
                TouchTransientScrollbar(scrollbar_visible_, scrollbar_visible_until_);
                return true;
            }
            scrollFromScrollbarY(mouse.y);
            TouchTransientScrollbar(scrollbar_visible_, scrollbar_visible_until_);
            return true;
        }

        if (options_.overlayScrollbar && mouse.button == ftxui::Mouse::Left &&
            mouse.motion == ftxui::Mouse::Pressed && mouse_in_scrollbar &&
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
            TouchTransientScrollbar(scrollbar_visible_, scrollbar_visible_until_);
            return true;
        }
    }

    const int previous = selected_;

    if (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelUp) {
        selected_ -= 5;
        at_bottom_ = false;
        TouchTransientScrollbar(scrollbar_visible_, scrollbar_visible_until_);
    }
    if (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelDown) {
        selected_ += 5;
        at_bottom_ = false;
        TouchTransientScrollbar(scrollbar_visible_, scrollbar_visible_until_);
    }
    if (event == ftxui::Event::PageDown) {
        selected_ += std::max(1, viewport_height_ - 2);
        at_bottom_ = false;
        TouchTransientScrollbar(scrollbar_visible_, scrollbar_visible_until_);
    }
    if (event == ftxui::Event::PageUp) {
        selected_ -= std::max(1, viewport_height_ - 2);
        at_bottom_ = false;
        TouchTransientScrollbar(scrollbar_visible_, scrollbar_visible_until_);
    }
    if (event == ftxui::Event::ArrowDown) {
        selected_ += 1;
        at_bottom_ = false;
        TouchTransientScrollbar(scrollbar_visible_, scrollbar_visible_until_);
    }
    if (event == ftxui::Event::ArrowUp) {
        selected_ -= 1;
        at_bottom_ = false;
        TouchTransientScrollbar(scrollbar_visible_, scrollbar_visible_until_);
    }
    if (event == ftxui::Event::Home) {
        selected_ = 0;
        at_bottom_ = false;
        TouchTransientScrollbar(scrollbar_visible_, scrollbar_visible_until_);
    }

    const bool isKeyboardScroll =
        event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown ||
        event == ftxui::Event::PageUp || event == ftxui::Event::PageDown ||
        event == ftxui::Event::Home || event == ftxui::Event::End;

    if (event == ftxui::Event::End) {
        selected_ = maxScroll(viewport_height_);
        at_bottom_ = true;
        TouchTransientScrollbar(scrollbar_visible_, scrollbar_visible_until_);
    }

    selected_ = std::clamp(selected_, 0, maxScroll(viewport_height_));
    syncBottomFlag();
    updateScrollbarGeometry(viewport_height_);

    const bool childHandled = ComponentBase::OnEvent(event);
    if (childHandled) {
        InvalidateLayout();
    }
    return previous != selected_ || childHandled || isKeyboardScroll || scrollbar_dragging_;
}

bool ScrollableBoxComponent::Focusable() const {
    return true;
}

std::shared_ptr<ScrollableBoxComponent> ScrollableBox(ftxui::Component child,
                                                      ScrollableBoxOptions options) {
    return ftxui::Make<ScrollableBoxComponent>(std::move(child), options);
}

} // namespace firmius::tui
