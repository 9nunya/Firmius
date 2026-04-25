#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

namespace firmius::tui {

struct ScrollableBoxOptions {
    bool startAtBottom = false;
    bool overlayScrollbar = false;
    int overlayScrollbarGutter = 2;
    bool showScrollbar = true;
    std::function<std::size_t()> measurement_signature_getter = nullptr;
    std::function<int(int width)> custom_size_getter = nullptr;
};

// Wraps `child` inside a scrollable frame that responds to wheel, page, home, and
// end events. Based on the proven `Scroller` example.
class ScrollableBoxComponent : public ftxui::ComponentBase {
public:
    explicit ScrollableBoxComponent(ftxui::Component child,
                                    ScrollableBoxOptions options = {});

    void RequestScrollToBottom();
    void RequestScrollToTop();
    void RequestEnsureVisible(int line);
    void RequestEnsureVisible(int first_line, int last_line);
    void InvalidateLayout();
    int ContentWidth() const;

    int ScrollOffset() const { return selected_; }
    int ViewportHeight() const { return viewport_height_; }
    bool IsAtBottom() const { return at_bottom_; }
    ftxui::Element OnRender() override;
    bool OnEvent(ftxui::Event event) override;
    bool Focusable() const override;

private:
    int maxScroll(int viewportHeight) const;
    void syncBottomFlag();
    void updateScrollbarGeometry(int viewportHeight);
    void scrollFromScrollbarY(int y);

    ScrollableBoxOptions options_;
    int selected_ = 0;
    int size_ = 0;
    ftxui::Box box_;
    ftxui::Box scrollbarBox_;
    ftxui::Component child_;
    bool at_bottom_ = false;
    int viewport_width_ = 0;
    int last_rendered_viewport_w_ = -1;
    int viewport_height_ = 0;
    int last_max_scroll_ = 0;
    int scrollbar_thumb_top_ = 0;
    int scrollbar_thumb_height_ = 0;
    bool scrollbar_hovered_ = false;
    bool scrollbar_dragging_ = false;
    int scrollbar_drag_offset_ = 0;
    bool has_pending_ensure_visible_ = false;
    int pending_visible_start_ = 0;
    int pending_visible_end_ = 0;
    bool layout_dirty_ = true;
    int measured_viewport_width_ = -1;
    std::size_t measured_signature_ = 0;
    ftxui::CapturedMouse captured_mouse_;
    bool scrollbar_visible_ = false;
    std::chrono::steady_clock::time_point scrollbar_visible_until_{};
};

std::shared_ptr<ScrollableBoxComponent> ScrollableBox(
    ftxui::Component child, ScrollableBoxOptions options = {});

} // namespace firmius::tui
