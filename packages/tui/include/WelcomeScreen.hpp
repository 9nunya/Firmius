#pragma once

#include <ftxui/component/component.hpp>
#include <memory>
#include <vector>
#include <string>

namespace firmius::tui {

class AppState;

class WelcomeScreen {
public:
    explicit WelcomeScreen(std::shared_ptr<AppState> state);
    ~WelcomeScreen() = default;

    ftxui::Element Render() const;

private:
    std::shared_ptr<AppState> state_;
    mutable bool isReload_ = false;
    mutable bool initialized_ = false;
};

}
