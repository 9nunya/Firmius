#include "modals/APIKeyWizardModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "providers/ProviderRegistry.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
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
  auto wizard =
      std::shared_ptr<firmius::provider::APIKeyWizard>(std::move(wizard_));

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
    const auto &theme = ThemeManager::instance().getCurrentTheme();

    auto title_color = theme.modals.title;
    if (*isDone) {
      if (*isError)
        title_color = theme.status_bar.error.normal.fg;
      else
        title_color = theme.modals.highlight_fg;
    }

    if (*isDone) {
      if (*isError) {
        ftxui::Elements errorContent;
        errorContent.push_back(
            ftxui::text(" Connection Failed ") | ftxui::bold |
            ftxui::color(theme.status_bar.error.normal.fg) | ftxui::center);
        errorContent.push_back(ftxui::text(""));
        errorContent.push_back(ftxui::text(*errorMessage) | ftxui::center |
                               ftxui::automerge |
                               ftxui::color(theme.modals.fg));
        content.push_back(ftxui::vbox(errorContent) | ftxui::borderRounded |
                          ftxui::color(theme.status_bar.error.normal.fg));
        content.push_back(ftxui::text(""));
        content.push_back(ftxui::text(" (Press Enter/Esc to close) ") |
                          ftxui::color(theme.base.dim) | ftxui::center);
      } else {
        ftxui::Elements successContent;
        successContent.push_back(ftxui::text(" API Key Added! ") | ftxui::bold |
                                 ftxui::color(theme.modals.highlight_fg) |
                                 ftxui::center);
        successContent.push_back(ftxui::text(""));
        successContent.push_back(ftxui::text(*resultMessage) | ftxui::center |
                                 ftxui::color(theme.modals.fg));
        content.push_back(ftxui::vbox(successContent) | ftxui::borderRounded |
                          ftxui::color(theme.modals.border));
        content.push_back(ftxui::text(""));
        content.push_back(ftxui::text(" (Press Enter/Esc to close) ") |
                          ftxui::color(theme.base.dim) | ftxui::center);
      }
    } else {
      ftxui::Elements inputContent;
      inputContent.push_back(ftxui::text(" Add API Key ") | ftxui::bold |
                             ftxui::color(theme.modals.title) | ftxui::center);
      inputContent.push_back(ftxui::text(""));
      inputContent.push_back(
          ftxui::text(" Enter your API key for " + providerName + ": ") |
          ftxui::center | ftxui::color(theme.modals.fg));
      inputContent.push_back(ftxui::text(""));
      inputContent.push_back(input->Render() | ftxui::color(theme.modals.fg));
      inputContent.push_back(ftxui::text(""));
      inputContent.push_back(
          ftxui::text(
              " The key will be stored securely in ~/.firmius/keys.json ") |
          ftxui::color(theme.base.dim) | ftxui::center);
      content.push_back(ftxui::vbox(inputContent) | ftxui::borderRounded |
                        ftxui::color(theme.modals.border));
      content.push_back(ftxui::text(""));
      ftxui::Elements buttonContent;
      buttonContent.push_back(ftxui::text(" [Enter] ") | ftxui::bold |
                              ftxui::color(theme.modals.highlight_fg));
      buttonContent.push_back(ftxui::text(" Save Key   ") |
                              ftxui::color(theme.modals.fg));
      buttonContent.push_back(ftxui::text(" [ESC] ") | ftxui::bold |
                              ftxui::color(theme.status_bar.error.normal.fg));
      buttonContent.push_back(ftxui::text(" Cancel ") |
                              ftxui::color(theme.modals.fg));
      content.push_back(ftxui::hbox(buttonContent) | ftxui::center);
    }

    auto window_title = ftxui::hbox(
        {ftxui::text(" API Key Setup: ") | ftxui::bold |
             ftxui::color(theme.modals.title),
         ftxui::text(providerName) | ftxui::bold | ftxui::color(title_color)});

    return ftxui::window(window_title, ftxui::vbox(content)) |
           ftxui::clear_under | ftxui::center |
           ftxui::bgcolor(theme.modals.bg) | ftxui::color(theme.modals.border);
  });

  // Single event handler that handles paste, global keys, and delegates to
  // input
  return ftxui::CatchEvent(renderer, [input_content, isDone, isError,
                                      errorMessage, resultMessage, wizard,
                                      providerName, &state, paste_buffer,
                                      in_paste, input](ftxui::Event event) {
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
        return true;
      }

      wizard->submitAnswer(*input_content);
      std::string apiKey;
      std::string err;
      if (wizard->finalizeExchange(apiKey, err)) {
        auto provider =
            firmius::provider::ProviderRegistry::instance().getProvider(
                providerName);
        auto apiKeyProvider =
            std::dynamic_pointer_cast<firmius::provider::BaseAPIKeyProvider>(
                provider);
        if (apiKeyProvider && !apiKey.empty()) {
          firmius::provider::APIKeyAccount acc;
          acc.apiKey = apiKey;
          acc.keyPrefix =
              firmius::provider::BaseAPIKeyProvider::extractKeyPrefix(apiKey);
          acc.identifier = apiKeyProvider->generateIdentifier();
          apiKeyProvider->addAccount(acc);
        }

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
