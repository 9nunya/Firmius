#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {

class AppState;

enum class ModalType {
    None,
    ModelSelector,
    ThreadSwitcher
};

class ModalSystem {
public:
    explicit ModalSystem(std::shared_ptr<AppState> state);
    ~ModalSystem() = default;

    ftxui::Element Render(ftxui::Element main_content) const;
    bool HandleEvent(ftxui::Event event);

    void show(ModalType type);
    void hide();
    bool isActive() const;

private:
    std::shared_ptr<AppState> state_;
    ModalType activeModal_ = ModalType::None;
    int selectedIndex_ = 0;

    ftxui::Element renderModelSelector() const;
    ftxui::Element renderThreadSwitcher() const;
    ftxui::Element wrapInDialog(ftxui::Element content, const std::string& title) const;
};

}
