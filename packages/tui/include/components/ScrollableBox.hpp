#pragma once

#include <memory>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

namespace firmius::tui {

// Wraps `child` inside a scrollable frame that responds to wheel, page, home, and
// end events. Based on the proven `Scroller` example.
class ScrollableBoxComponent : public ftxui::ComponentBase {
public:
    explicit ScrollableBoxComponent(ftxui::Component child);

    void RequestScrollToBottom();

    ftxui::Element Render();
    bool OnEvent(ftxui::Event event) override;
    bool Focusable() const override;

private:
    int selected_ = 0;
    int size_ = 0;
    ftxui::Box box_;
    ftxui::Component child_;
    bool at_bottom_ = true;
    int viewport_width_ = 0;
};

std::shared_ptr<ScrollableBoxComponent> ScrollableBox(ftxui::Component child);

} // namespace firmius::tui
