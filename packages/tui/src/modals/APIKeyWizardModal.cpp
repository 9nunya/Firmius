#include "modals/APIKeyWizardModal.hpp"
#include "TUIState.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component_options.hpp>
#include <string>

namespace firmius::tui {

APIKeyWizardModal::APIKeyWizardModal(
    std::unique_ptr<firmius::provider::APIKeyWizard> wizard,
    std::string providerName)
    : wizard_(std::move(wizard)), providerName_(std::move(providerName)) {}

ftxui::Component APIKeyWizardModal::create(TuiState &state) {
  auto input_content = std::make_shared<std::string>("");
  auto errorMessage = std::make_shared<std::string>("");
  auto isDone = std::make_shared<bool>(false);
  auto isError = std::make_shared<bool>(false);
  auto resultMessage = std::make_shared<std::string>("");
  auto wizard = std::shared_ptr<firmius::provider::APIKeyWizard>(
      std::move(wizard_));

  auto providerName = providerName_;

  // Bracketed paste handling
  auto paste_buffer = std::make_shared<std::string>("");
  auto in_paste = std::make_shared<bool>(false);

  auto opt = ftxui::InputOption::Default();
  opt.content = input_content.get();
  opt.placeholder = "sk-...";

  auto input = ftxui::Input(opt);

  auto renderer = ftxui::Renderer([input_content, errorMessage, isDone, isError,
                                   resultMessage, providerName, input]() {
    ftxui::Elements content;

    auto title_color = ftxui::Color::Cyan;
    if (*isDone) {
      if (*isError)
        title_color = ftxui::Color::Red;
      else
        title_color = ftxui::Color::Green;
    }

    if (*isDone) {
      if (*isError) {
        ftxui::Elements errorContent;
        errorContent.push_back(ftxui::text(" Connection Failed ") | ftxui::bold |
                    ftxui::color(ftxui::Color::Red) | ftxui::center);
        errorContent.push_back(ftxui::text(""));
        errorContent.push_back(ftxui::text(*errorMessage) | ftxui::center | ftxui::automerge);
        content.push_back(
            ftxui::vbox(errorContent) |
            ftxui::borderRounded | ftxui::color(ftxui::Color::Red));
        content.push_back(ftxui::text(""));
        content.push_back(ftxui::text(" (Press Enter/Esc to close) ") |
                          ftxui::dim | ftxui::center);
      } else {
        ftxui::Elements successContent;
        successContent.push_back(ftxui::text(" API Key Added! ") | ftxui::bold |
                    ftxui::color(ftxui::Color::Green) | ftxui::center);
        successContent.push_back(ftxui::text(""));
        successContent.push_back(ftxui::text(*resultMessage) | ftxui::center);
        content.push_back(
            ftxui::vbox(successContent) |
            ftxui::borderRounded | ftxui::color(ftxui::Color::Green));
        content.push_back(ftxui::text(""));
        content.push_back(ftxui::text(" (Press Enter/Esc to close) ") |
                          ftxui::dim | ftxui::center);
      }
    } else {
      ftxui::Elements inputContent;
      inputContent.push_back(ftxui::text(" Add API Key ") | ftxui::bold |
                  ftxui::color(ftxui::Color::Yellow) | ftxui::center);
      inputContent.push_back(ftxui::text(""));
      inputContent.push_back(ftxui::text(" Enter your API key for " + providerName + ": ") |
                  ftxui::center);
      inputContent.push_back(ftxui::text(""));
      inputContent.push_back(input->Render());
      inputContent.push_back(ftxui::text(""));
      inputContent.push_back(ftxui::text(" The key will be stored securely in ~/.firmius/keys.json ") |
                  ftxui::dim | ftxui::center);
      content.push_back(
          ftxui::vbox(inputContent) |
          ftxui::borderRounded | ftxui::color(ftxui::Color::Yellow));
      content.push_back(ftxui::text(""));
      ftxui::Elements buttonContent;
      buttonContent.push_back(ftxui::text(" [Enter] ") | ftxui::bold |
                  ftxui::color(ftxui::Color::Green));
      buttonContent.push_back(ftxui::text(" Save Key   "));
      buttonContent.push_back(ftxui::text(" [ESC] ") | ftxui::bold |
                  ftxui::color(ftxui::Color::Red));
      buttonContent.push_back(ftxui::text(" Cancel "));
      content.push_back(
          ftxui::hbox(buttonContent) |
          ftxui::center);
    }

    auto window_title = ftxui::hbox(
        {ftxui::text(" API Key Setup: ") | ftxui::bold,
         ftxui::text(providerName) | ftxui::bold | ftxui::color(title_color)});

    return ftxui::window(window_title, ftxui::vbox(content)) |
           ftxui::clear_under | ftxui::center;
  });

  // Single event handler that handles paste, global keys, and delegates to input
  return ftxui::CatchEvent(renderer, [input_content, isDone, isError, errorMessage,
                                      resultMessage, wizard, &state,
                                      paste_buffer, in_paste, input](ftxui::Event event) {
    // Handle Escape globally
    if (event == ftxui::Event::Escape) {
      state.popModalImmediate();
      return true;
    }

    // Handle completion state
    if (*isDone) {
      if (event == ftxui::Event::Return) {
        state.popModalImmediate();
        return true;
      }
      return false;
    }

    // Handle bracketed paste
    std::string raw = event.input();
    if (raw == "\x1b[200~") {
      *in_paste = true;
      *paste_buffer = "";
      return true;
    }
    if (raw == "\x1b[201~") {
      if (*in_paste && !paste_buffer->empty()) {
        if (!paste_buffer->empty() && paste_buffer->back() == '\n') {
          paste_buffer->pop_back();
        }
        *input_content = *paste_buffer;
        state.postEvent(ftxui::Event::Custom);
      }
      *in_paste = false;
      paste_buffer->clear();
      return true;
    }
    if (*in_paste) {
      *paste_buffer += raw;
      return true;
    }

    // Handle Enter for submission
    if (event == ftxui::Event::Return) {
      if (input_content->empty()) {
        *isError = true;
        *errorMessage = "API key cannot be empty.";
        *isDone = true;
        state.postEvent(ftxui::Event::Custom);
        return true;
      }

      std::string apiKey = *input_content;
      std::string err;
      if (wizard->finalizeExchange(apiKey, err)) {
        *resultMessage = wizard->getFinalMessage();
        *isDone = true;
        *isError = false;
      } else {
        *errorMessage = err;
        *isDone = true;
        *isError = true;
      }
      state.postEvent(ftxui::Event::Custom);
      return true;
    }

    // Let input handle everything else (character input, navigation, etc.)
    return input->OnEvent(event);
  });
}

} // namespace firmius::tui
