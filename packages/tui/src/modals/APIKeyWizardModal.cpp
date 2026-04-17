#include "modals/APIKeyWizardModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "modals/ModalLayout.hpp"
#include "providers/ProviderRegistry.hpp"
#include "harness/Harness.hpp"
#include <chrono>
#include <cstdlib>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace firmius::tui {
namespace {

std::string detectFirstUrl(const std::string &text) {
  static const std::regex urlRegex(R"(https?://[^\s]+)");
  std::smatch match;
  if (std::regex_search(text, match, urlRegex)) {
    return match.str(0);
  }
  return "";
}

bool openUrlPlatformSpecific(const std::string &url) {
  if (url.empty()) {
    return false;
  }

#ifdef _WIN32
  std::string command = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
  std::string command = "open \"" + url + "\" >/dev/null 2>&1 &";
#else
  std::string command = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
#endif

  return std::system(command.c_str()) == 0;
}

bool HasChoices(const std::optional<firmius::WizardPrompt> &prompt) {
  return prompt.has_value() && !prompt->choices.empty();
}

std::string PlaceholderFor(const std::optional<firmius::WizardPrompt> &prompt) {
  if (!prompt.has_value()) {
    return "sk-...";
  }
  if (!prompt->placeholder.empty()) {
    return prompt->placeholder;
  }
  return prompt->isSecret ? "Enter secret..." : "Enter response...";
}

std::string SubmitLabelFor(const std::optional<firmius::WizardPrompt> &prompt,
                           bool has_url) {
  if (!prompt.has_value()) {
    return has_url ? "Open URL / Continue" : "Submit";
  }
  if (!prompt->submitLabel.empty()) {
    return prompt->submitLabel;
  }
  return has_url ? "Open URL / Continue" : "Submit";
}

std::vector<std::string>
ChoiceLabelsFor(const std::optional<firmius::WizardPrompt> &prompt) {
  std::vector<std::string> labels;
  if (!prompt.has_value()) {
    return labels;
  }
  labels.reserve(prompt->choices.size());
  for (const auto &choice : prompt->choices) {
    labels.push_back(choice.label);
  }
  return labels;
}

} // namespace

APIKeyWizardModal::APIKeyWizardModal(
    std::unique_ptr<firmius::provider::APIKeyWizard> wizard,
    std::string providerName)
    : wizard_(std::move(wizard)), providerName_(std::move(providerName)) {}

ftxui::Component APIKeyWizardModal::create(TuiState &state) {
  auto currentPrompt =
      std::make_shared<std::optional<firmius::WizardPrompt>>(std::nullopt);
  auto promptUrl = std::make_shared<std::string>("");
  auto inputContent = std::make_shared<std::string>("");
  auto choiceLabels = std::make_shared<std::vector<std::string>>();
  auto selectedChoice = std::make_shared<int>(0);
  auto errorMessage = std::make_shared<std::string>("");
  auto isDone = std::make_shared<std::atomic<bool>>(false);
  auto isError = std::make_shared<std::atomic<bool>>(false);
  auto isPolling = std::make_shared<std::atomic<bool>>(false);
  auto resultMessage = std::make_shared<std::string>("");
  auto wizard =
      std::shared_ptr<firmius::provider::APIKeyWizard>(std::move(wizard_));

  auto providerName = providerName_;

  // Bracketed paste handling
  auto pasteBuffer = std::make_shared<std::string>("");
  auto inPaste = std::make_shared<bool>(false);

  auto refreshPrompt = [wizard, currentPrompt, promptUrl, inputContent,
                        choiceLabels, selectedChoice]() {
    *inputContent = "";
    *selectedChoice = 0;
    *currentPrompt = wizard->nextPrompt();
    *promptUrl = currentPrompt->has_value()
                     ? detectFirstUrl(currentPrompt->value().message)
                     : std::string();
    *choiceLabels = ChoiceLabelsFor(*currentPrompt);
  };
  refreshPrompt();

  auto startFinalize = [wizard, isPolling, isDone, isError, errorMessage,
                        resultMessage, providerName, &state]() {
    if (*isPolling) {
      return;
    }
    *isPolling = true;
    std::thread([wizard, isPolling, isDone, isError, errorMessage,
                 resultMessage, providerName, &state]() {
      while (!wizard->isComplete()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }

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
          apiKeyProvider->addApiKey(apiKey);
          firmius::core::Harness::instance().invalidateModelCache();
        }

        *resultMessage = wizard->getFinalMessage();
        *isDone = true;
        *isError = false;
      } else {
        *errorMessage = err;
        *isDone = true;
        *isError = true;
      }
      *isPolling = false;
      state.postEvent(ftxui::Event::Custom);
    }).detach();
  };

  if (!currentPrompt->has_value()) {
    startFinalize();
  }

  auto inputOption = ftxui::InputOption::Default();
  inputOption.content = inputContent.get();
  inputOption.placeholder = PlaceholderFor(*currentPrompt);
  inputOption.password =
      currentPrompt->has_value() && currentPrompt->value().isSecret;
  auto input = ftxui::Input(inputOption);
  auto radiobox = ftxui::Radiobox(choiceLabels.get(), selectedChoice.get());
  auto container = ftxui::Container::Vertical({radiobox, input});

  auto syncFocus = [currentPrompt, input, radiobox, &state]() {
    if (HasChoices(*currentPrompt)) {
      radiobox->TakeFocus();
    } else if (currentPrompt->has_value() &&
               currentPrompt->value().allowFreeformInput) {
      input->TakeFocus();
    }
    state.postEvent(ftxui::Event::Custom);
  };

  syncFocus();

  auto renderer = ftxui::Renderer(
      container,
      [currentPrompt, promptUrl, inputContent, choiceLabels, isDone, isError,
       resultMessage, errorMessage, providerName, input, radiobox]() {
        ftxui::Elements content;
        const auto &theme = ThemeManager::instance().getCurrentTheme();

        auto titleColor = theme.modals.title;
        if (*isDone) {
          titleColor = *isError ? theme.status_bar.error.normal.fg
                                : theme.modals.highlight_fg;
        }

        if (*isDone) {
          if (*isError) {
            ftxui::Elements errorContent;
            errorContent.push_back(
                ftxui::text(" Connection Failed ") | ftxui::bold |
                ftxui::color(theme.status_bar.error.normal.fg) |
                ftxui::center);
            errorContent.push_back(ftxui::text(""));
            errorContent.push_back(ftxui::text(*errorMessage) |
                                   ftxui::center | ftxui::automerge |
                                   ftxui::color(theme.modals.fg));
            content.push_back(ModalSection(theme, ftxui::vbox(errorContent),
                                           theme.base.bg));
            content.push_back(ftxui::text(""));
            content.push_back(ftxui::text(" (Press Enter/Esc to close) ") |
                              ftxui::color(theme.base.dim) | ftxui::center);
          } else {
            ftxui::Elements successContent;
            successContent.push_back(ftxui::text(" API Key Added! ") |
                                     ftxui::bold |
                                     ftxui::color(theme.modals.highlight_fg) |
                                     ftxui::center);
            successContent.push_back(ftxui::text(""));
            successContent.push_back(ftxui::text(*resultMessage) |
                                     ftxui::center |
                                     ftxui::color(theme.modals.fg));
            content.push_back(ModalSection(theme, ftxui::vbox(successContent),
                                           theme.base.bg));
            content.push_back(ftxui::text(""));
            content.push_back(ftxui::text(" (Press Enter/Esc to close) ") |
                              ftxui::color(theme.base.dim) | ftxui::center);
          }
        } else if (currentPrompt->has_value()) {
          const auto &prompt = currentPrompt->value();
          ftxui::Elements promptContent;
          promptContent.push_back(ftxui::text(" Add Connection ") | ftxui::bold |
                                  ftxui::color(theme.modals.title) |
                                  ftxui::center);
          promptContent.push_back(ftxui::text(""));
          promptContent.push_back(ftxui::paragraph(prompt.message) |
                                  ftxui::color(theme.modals.fg));

          if (!promptUrl->empty()) {
            promptContent.push_back(ftxui::text(""));
            promptContent.push_back(
                ftxui::text("Detected URL:") | ftxui::bold |
                ftxui::color(theme.modals.highlight_fg));
            promptContent.push_back(ftxui::paragraph(*promptUrl) |
                                    ftxui::color(theme.modals.highlight_fg));
          }

          if (HasChoices(*currentPrompt)) {
            promptContent.push_back(ftxui::text(""));
            promptContent.push_back(radiobox->Render() | ftxui::center);
            promptContent.push_back(ftxui::text(""));
            promptContent.push_back(
                ftxui::text("Use ↑/↓ and Enter to choose") |
                ftxui::color(theme.base.dim) | ftxui::center);
          } else if (prompt.allowFreeformInput) {
            promptContent.push_back(ftxui::text(""));
            promptContent.push_back(input->Render() |
                                    ftxui::color(theme.modals.fg));
          } else {
            promptContent.push_back(ftxui::text(""));
            promptContent.push_back(
                ftxui::text("Press Enter to continue.") |
                ftxui::color(theme.base.dim) | ftxui::center);
          }

          if (providerName != "kilo") {
            promptContent.push_back(ftxui::text(""));
            promptContent.push_back(
                ftxui::text(
                    " The key will be stored securely in ~/.firmius/keys.json ") |
                ftxui::color(theme.base.dim) | ftxui::center);
          }

          content.push_back(
              ModalSection(theme, ftxui::vbox(promptContent), theme.base.bg));
          content.push_back(ftxui::text(""));
          ftxui::Elements buttonContent;
          buttonContent.push_back(ftxui::text(" [Enter] ") | ftxui::bold |
                                  ftxui::color(theme.modals.highlight_fg));
          buttonContent.push_back(
              ftxui::text(" " +
                          SubmitLabelFor(*currentPrompt, !promptUrl->empty()) +
                          "   ") |
              ftxui::color(theme.modals.fg));
          buttonContent.push_back(ftxui::text(" [ESC] ") | ftxui::bold |
                                  ftxui::color(theme.status_bar.error.normal.fg));
          buttonContent.push_back(ftxui::text(" Cancel ") |
                                  ftxui::color(theme.modals.fg));
          content.push_back(ftxui::hbox(buttonContent) | ftxui::center);
        } else {
          content.push_back(
              ModalSection(
                  theme,
                  ftxui::vbox({
                      ftxui::text(" Connection in Progress ") |
                          ftxui::bold | ftxui::color(theme.modals.title) |
                          ftxui::center,
                      ftxui::text(""),
                      ftxui::text("Waiting for the provider to finish the connection flow.") |
                          ftxui::center | ftxui::color(theme.modals.fg),
                      ftxui::text(" Polling for completion... ") |
                          ftxui::color(theme.base.dim) | ftxui::blink |
                          ftxui::center,
                  }),
                  theme.base.bg));
          content.push_back(ftxui::text(""));
          content.push_back(ftxui::text(" (Press Esc to cancel) ") |
                            ftxui::color(theme.base.dim) | ftxui::center);
        }

        return FlatModalPanel(theme, "API Key Setup: " + providerName,
                              ModalSection(theme, ftxui::vbox(content),
                                           theme.modals.bg),
                              72, 24, titleColor);
      });

  return ftxui::CatchEvent(
      renderer,
      [currentPrompt, promptUrl, inputContent, isDone, wizard, refreshPrompt,
       startFinalize, &state, pasteBuffer, inPaste, selectedChoice, syncFocus,
       container](ftxui::Event event) mutable {
        if (event == ftxui::Event::Escape) {
          state.popModal();
          return true;
        }

        if (*isDone) {
          if (event == ftxui::Event::Return) {
            state.popModal();
            return true;
          }
          return false;
        }

        std::string raw = event.input();
        if (raw == "\x1b[200~") {
          *inPaste = true;
          *pasteBuffer = "";
          return true;
        }
        if (raw == "\x1b[201~") {
          if (*inPaste && !pasteBuffer->empty()) {
            if (!pasteBuffer->empty() && pasteBuffer->back() == '\n') {
              pasteBuffer->pop_back();
            }
            *inputContent = *pasteBuffer;
            state.postEvent(ftxui::Event::Custom);
          }
          *inPaste = false;
          pasteBuffer->clear();
          return true;
        }
        if (*inPaste) {
          *pasteBuffer += raw;
          return true;
        }

        if (!currentPrompt->has_value()) {
          return false;
        }

        const auto &prompt = currentPrompt->value();
        if (event == ftxui::Event::Return) {
          std::string answer;
          if (HasChoices(*currentPrompt)) {
            if (*selectedChoice >= 0 &&
                static_cast<size_t>(*selectedChoice) < prompt.choices.size()) {
              answer = prompt.choices[static_cast<size_t>(*selectedChoice)].value;
            }
          } else if (prompt.allowFreeformInput) {
            if (inputContent->empty() && !prompt.allowEmptyInput) {
              return true;
            }
            answer = *inputContent;
          }

          if (!promptUrl->empty() && !prompt.allowFreeformInput) {
            openUrlPlatformSpecific(*promptUrl);
          }

          wizard->submitAnswer(answer);
          refreshPrompt();
          if (!currentPrompt->has_value()) {
            startFinalize();
          }
          syncFocus();
          return true;
        }

        if (HasChoices(*currentPrompt) || prompt.allowFreeformInput) {
          return container->OnEvent(event);
        }
        return false;
      });
}

} // namespace firmius::tui
