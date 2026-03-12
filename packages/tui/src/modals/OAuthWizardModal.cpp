#include "modals/OAuthWizardModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include <chrono>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <regex>
#include <thread>

namespace firmius::tui {

OAuthWizardModal::OAuthWizardModal(std::unique_ptr<firmius::OAuthWizard> wizard,
                                   std::string providerName)
    : wizard_(std::move(wizard)), providerName_(std::move(providerName)) {}

ftxui::Component OAuthWizardModal::create(TuiState &state) {
  auto message = std::make_shared<std::string>("");
  auto url = std::make_shared<std::string>("");
  auto isPolling = std::make_shared<std::atomic<bool>>(false);
  auto isDone = std::make_shared<std::atomic<bool>>(false);
  auto isError = std::make_shared<std::atomic<bool>>(false);
  auto resultMessage = std::make_shared<std::string>("");
  auto wizard = std::shared_ptr<firmius::OAuthWizard>(std::move(wizard_));

  auto initPrompt = [wizard, message, url]() {
    if (auto p = wizard->nextPrompt()) {
      *message = p->message;
      std::regex url_regex(R"(https?://[^\s]+)");
      std::smatch match;
      if (std::regex_search(*message, match, url_regex)) {
        *url = match.str(0);
      }
    }
  };
  initPrompt();

  auto startPolling = [wizard, isPolling, isDone, isError, resultMessage,
                       &state]() {
    if (*isPolling)
      return;
    *isPolling = true;
    std::thread([wizard, isPolling, isDone, isError, resultMessage, &state]() {
      while (!wizard->isComplete()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // Simple timeout check or cancellation could go here
      }

      std::string err;
      if (!wizard->finalizeExchange(err)) {
        *isError = true;
        *resultMessage = err;
      } else {
        *resultMessage = wizard->getFinalMessage();
      }

      *isDone = true;
      *isPolling = false;
      state.postEvent(ftxui::Event::Custom);
    }).detach();
  };

  startPolling();

  auto tab_index = std::make_shared<int>(0);

  auto providerName = providerName_;

  auto renderer = ftxui::Renderer([message, url, isPolling, isDone, isError,
                                   resultMessage, tab_index, providerName]() {
    *tab_index = *isDone ? 1 : 0;
    const auto &theme = ThemeManager::instance().getCurrentTheme();

    ftxui::Elements content;

    auto title_color = theme.modals.title;
    if (*isDone) {
      if (*isError)
        title_color = theme.status_bar.error.normal.fg;
      else
        title_color = theme.modals.highlight_fg;
    }

    content.push_back(ftxui::text(*message) | ftxui::automerge | ftxui::bold |
                      ftxui::color(theme.modals.fg));
    content.push_back(ftxui::text(""));

    if (*isDone) {
      if (*isError) {
        content.push_back(
            ftxui::vbox({
                ftxui::text(" Connection Failed ") | ftxui::bold |
                    ftxui::color(theme.status_bar.error.normal.fg) |
                    ftxui::center,
                ftxui::text(""),
                ftxui::text(*resultMessage) | ftxui::center | ftxui::automerge |
                    ftxui::color(theme.modals.fg),
            }) |
            ftxui::borderRounded |
            ftxui::color(theme.status_bar.error.normal.fg));
        content.push_back(ftxui::text(""));
        content.push_back(ftxui::text(" (Press Enter/Esc to close) ") |
                          ftxui::color(theme.base.dim) | ftxui::center);
      } else {
        content.push_back(
            ftxui::vbox({
                ftxui::text(" Connection Successful! ") | ftxui::bold |
                    ftxui::color(theme.modals.highlight_fg) | ftxui::center,
                ftxui::text(""),
                ftxui::text(*resultMessage) | ftxui::center |
                    ftxui::color(theme.modals.fg),
            }) |
            ftxui::borderRounded | ftxui::color(theme.modals.highlight_fg));
        content.push_back(ftxui::text(""));
        content.push_back(ftxui::text(" (Press Enter/Esc to close) ") |
                          ftxui::color(theme.base.dim) | ftxui::center);
      }
    } else {
      content.push_back(
          ftxui::vbox({
              ftxui::text(" Authentication in Progress ") | ftxui::bold |
                  ftxui::color(theme.modals.title) | ftxui::center,
              ftxui::text(""),
              ftxui::text(
                  " Please check your browser to complete the login. ") |
                  ftxui::center | ftxui::color(theme.modals.fg),
              ftxui::text(" Waiting for callback... ") |
                  ftxui::color(theme.base.dim) | ftxui::blink | ftxui::center,
          }) |
          ftxui::borderRounded | ftxui::color(theme.modals.border));
      content.push_back(ftxui::text(""));
      content.push_back(ftxui::hbox({
                            ftxui::text(" [O]pen Browser ") | ftxui::bold |
                                ftxui::color(theme.modals.highlight_fg),
                            ftxui::text("   "),
                            ftxui::text(" [E]scape to Cancel ") | ftxui::bold |
                                ftxui::color(theme.status_bar.error.normal.fg),
                        }) |
                        ftxui::center);
    }

    auto window_title = ftxui::hbox(
        {ftxui::text(" OAuth Connection: ") | ftxui::bold |
             ftxui::color(theme.modals.title),
         ftxui::text(providerName) | ftxui::bold | ftxui::color(title_color)});

    return ftxui::window(window_title, ftxui::vbox(content)) |
           ftxui::clear_under | ftxui::center |
           ftxui::bgcolor(theme.modals.bg) | ftxui::color(theme.modals.border);
  });

  return ftxui::CatchEvent(renderer, [url, isDone, &state](ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
      state.popModalImmediate();
      return true;
    }
    if (*isDone) {
      if (event == ftxui::Event::Return) {
        state.popModalImmediate();
        return true;
      }
    } else {
      if (event == ftxui::Event::Character('o') ||
          event == ftxui::Event::Character('O') ||
          event == ftxui::Event::Return) {
        std::string cmd = "xdg-open \"" + *url + "\" &";
        system(cmd.c_str());
        return true;
      }
    }
    return false;
  });
}

} // namespace firmius::tui
