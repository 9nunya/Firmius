#pragma once

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
};

// Wraps `child` inside a scrollable frame that responds to wheel, page, home, and
// end events. Based on the proven `Scroller` example.
class ScrollableBoxComponent : public ftxui::ComponentBase {
public:
    explicit ScrollableBoxComponent(ftxui::Component child,
                                    ScrollableBoxOptions options = {});

    void RequestScrollToBottom();
    void RequestScrollToTop();

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
    int viewport_height_ = 0;
    int last_max_scroll_ = 0;
    int scrollbar_thumb_top_ = 0;
    int scrollbar_thumb_height_ = 0;
    bool scrollbar_hovered_ = false;
    bool scrollbar_dragging_ = false;
    int scrollbar_drag_offset_ = 0;
    ftxui::CapturedMouse captured_mouse_;
};

std::shared_ptr<ScrollableBoxComponent> ScrollableBox(
    ftxui::Component child, ScrollableBoxOptions options = {});

} // namespace firmius::tui
