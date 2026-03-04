#include "WelcomeScreen.hpp"
#include "AppState.hpp"
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

using namespace ftxui;

WelcomeScreen::WelcomeScreen(std::shared_ptr<AppState> state) : state_(std::move(state)) {}

ftxui::Element WelcomeScreen::Render() const {
    if (!initialized_) {
        if (!state_->getMessages().empty()) {
            isReload_ = true;
        }
        initialized_ = true;
    }

    if (isReload_ || !state_->getMessages().empty()) {
        return ftxui::emptyElement();
    }

    auto logo = vbox({
        text(R"(  _____ _                 _             )") | bold | color(Color::Cyan),
        text(R"( |  ___(_)               (_)            )") | bold | color(Color::Cyan),
        text(R"( | |_   _ _ __ _ __ ___  _ _   _ ___    )") | bold | color(Color::Cyan),
        text(R"( |  _| | | '__| '_ ` _ \ | | | | / __|   )") | bold | color(Color::Cyan),
        text(R"( | |   | | |  | | | | | | | |_| \__ \   )") | bold | color(Color::Cyan),
        text(R"( \_|   |_|_|  |_| |_| |_|_|\__,_|___/   )") | bold | color(Color::Cyan),
    });

    return vbox({
        filler(),
        logo | center,
        separator() | dim,
        text("Welcome to Firmius Engine.") | center | dim,
        text("Type /help for available commands.") | center | dim,
        filler(),
    }) | flex;
}

}
