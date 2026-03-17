#include "modals/AccountsModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "harness/Harness.hpp"
#include "modals/APIKeyWizardModal.hpp"
#include "modals/ModalLayout.hpp"
#include "modals/OAuthWizardModal.hpp"
#include "providers/BaseAPIKeyProvider.hpp"
#include "providers/BaseOAuthProvider.hpp"
#include "providers/ProviderRegistry.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace firmius::tui {

AccountsModal::AccountsModal(std::string providerId)
    : providerId_(std::move(providerId)) {}

ftxui::Component AccountsModal::create(TuiState &state) {
  auto oauthAccounts =
      std::make_shared<std::vector<firmius::shared::OAuthAccount>>();
  auto apiKeyAccounts =
      std::make_shared<std::vector<firmius::provider::APIKeyAccount>>();
  auto selected = std::make_shared<int>(0);
  auto isLoading = std::make_shared<bool>(true);
  auto providerId = providerId_;
  auto isAPIKeyProvider = std::make_shared<bool>(false);

  auto refreshAccounts = [oauthAccounts, apiKeyAccounts, isLoading, providerId,
                          isAPIKeyProvider, &state]() {
    *isLoading = true;
    std::thread([oauthAccounts, apiKeyAccounts, isLoading, providerId,
                 isAPIKeyProvider, &state]() {
      auto provider =
          firmius::provider::ProviderRegistry::instance().getProvider(
              providerId);

      if (provider) {
        auto apiKeyProvider =
            std::dynamic_pointer_cast<firmius::provider::BaseAPIKeyProvider>(
                provider);
        if (apiKeyProvider) {
          *isAPIKeyProvider = true;
          *apiKeyAccounts = apiKeyProvider->getAccounts();
        } else {
          *isAPIKeyProvider = false;
          *oauthAccounts =
              firmius::core::Harness::instance().getAccounts(providerId);
        }
      }
      *isLoading = false;
      state.postEvent(ftxui::Event::Custom);
    }).detach();
  };

  refreshAccounts();

  auto component = ftxui::Renderer([oauthAccounts, apiKeyAccounts, selected,
                                    isLoading, providerId, isAPIKeyProvider]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();

    if (*isLoading) {
      return FlatModalPanel(
          theme, "Accounts: " + providerId,
          ModalSection(
              theme,
              ftxui::vbox({ftxui::text("Loading accounts...") | ftxui::center |
                               ftxui::color(theme.modals.fg),
                           ftxui::text("") |
                               ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 5)}),
              theme.modals.bg),
          70, 18);
    }

    ftxui::Elements rows;

    if (*isAPIKeyProvider) {
      // Display API key accounts with safe filtering
      for (int i = 0; i < (int)apiKeyAccounts->size(); ++i) {
        const auto &acc = (*apiKeyAccounts)[i];
        // Safe display: shows "Key #N (prefix...)"
        std::string display = acc.identifier + " (" + acc.keyPrefix + "...)";
        auto label = ftxui::text("  " + display + "  ");
        if (i == *selected) {
          label = label | ftxui::inverted | ftxui::bold |
                  ftxui::color(theme.modals.highlight_fg) |
                  ftxui::bgcolor(theme.modals.highlight_bg);
        } else {
          label = label | ftxui::color(theme.modals.fg);
        }
        rows.push_back(label);
      }
    } else {
      // Display OAuth accounts
      for (int i = 0; i < (int)oauthAccounts->size(); ++i) {
        auto label = ftxui::text("  " + (*oauthAccounts)[i].identifier + "  ");
        if (i == *selected) {
          label = label | ftxui::inverted | ftxui::bold |
                  ftxui::color(theme.modals.highlight_fg) |
                  ftxui::bgcolor(theme.modals.highlight_bg);
        } else {
          label = label | ftxui::color(theme.modals.fg);
        }
        rows.push_back(label);
      }
    }

    if (rows.empty()) {
      rows.push_back(ftxui::text("No accounts connected.") |
                     ftxui::color(theme.base.dim) | ftxui::center);
    }

    return FlatModalPanel(
        theme, "Manage Accounts: " + providerId,
        ModalSection(
            theme,
            ftxui::vbox({
                ftxui::text("Select an account to manage:") |
                    ftxui::color(theme.base.dim),
                ftxui::text(""),
                ModalSection(
                    theme,
                    ftxui::vbox(rows) | ftxui::vscroll_indicator |
                        ftxui::yframe |
                        ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 8),
                    theme.base.bg),
                ftxui::text(""),
                ftxui::hbox(
                    {ftxui::text(" [A] ") | ftxui::bold |
                         ftxui::color(theme.modals.highlight_fg),
                     ftxui::text(" Add Account   ") |
                         ftxui::color(theme.modals.fg),
                     ftxui::text(" [D] ") | ftxui::bold |
                         ftxui::color(theme.status_bar.error.normal.fg),
                     ftxui::text(" Delete Selected ") |
                         ftxui::color(theme.modals.fg),
                     ftxui::filler(),
                     ftxui::text(" [ESC] ") | ftxui::bold |
                         ftxui::color(theme.base.dim),
                     ftxui::text(" Close ") |
                         ftxui::color(theme.modals.fg)}) |
                    ftxui::center,
                ftxui::separatorLight() | ftxui::color(theme.modals.border),
                ftxui::hbox({ftxui::text(" ↑↓ ") | ftxui::bold |
                                 ftxui::color(theme.modals.title),
                             ftxui::text("navigate elements")}) |
                    ftxui::center | ftxui::color(theme.base.dim),
            }),
            theme.modals.bg),
        78, 24);
  });

  return ftxui::CatchEvent(component, [oauthAccounts, apiKeyAccounts, selected,
                                       providerId, isAPIKeyProvider,
                                       refreshAccounts,
                                       &state](ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
      state.popModalImmediate();
      return true;
    }
    if (event == ftxui::Event::ArrowUp) {
      if (*selected > 0)
        (*selected)--;
      return true;
    }
    if (event == ftxui::Event::ArrowDown) {
      if (*selected < (int)(*isAPIKeyProvider ? apiKeyAccounts->size()
                                              : oauthAccounts->size()) -
                          1)
        (*selected)++;
      return true;
    }
    if (event == ftxui::Event::Delete ||
        event == ftxui::Event::Character('d') ||
        event == ftxui::Event::Character('D')) {
      if (!(*isAPIKeyProvider ? apiKeyAccounts->empty()
                              : oauthAccounts->empty()) &&
          *selected < (int)(*isAPIKeyProvider ? apiKeyAccounts->size()
                                              : oauthAccounts->size())) {
        std::string identifier = *isAPIKeyProvider
                                     ? (*apiKeyAccounts)[*selected].identifier
                                     : (*oauthAccounts)[*selected].identifier;
        firmius::core::Harness::instance().deleteAccount(providerId,
                                                         identifier);
        refreshAccounts();
        if (*selected >= (int)(*isAPIKeyProvider ? apiKeyAccounts->size()
                                                 : oauthAccounts->size()) &&
            *selected > 0)
          (*selected)--;
      }
      return true;
    }
    if (event == ftxui::Event::Character('a') ||
        event == ftxui::Event::Character('A')) {
      auto provider =
          firmius::provider::ProviderRegistry::instance().getProvider(
              providerId);
      if (provider) {
        // Try API key provider first
        auto apiKeyProvider =
            std::dynamic_pointer_cast<firmius::provider::BaseAPIKeyProvider>(
                provider);
        if (apiKeyProvider) {
          auto wizard = apiKeyProvider->beginConnectionWizard();
          if (wizard) {
            state.popModalImmediate();
            auto modalObj = std::make_shared<APIKeyWizardModal>(
                std::move(wizard), providerId);
            state.openModalDirect(modalObj->create(state));
          }
          return true;
        }

        // Fall back to OAuth provider
        auto oauthProvider =
            std::dynamic_pointer_cast<firmius::provider::BaseOAuthProvider>(
                provider);
        if (oauthProvider) {
          auto wizard = oauthProvider->beginConnectionWizard();
          if (wizard) {
            state.popModalImmediate();
            auto modalObj = std::make_shared<OAuthWizardModal>(
                std::move(wizard), providerId);
            state.openModalDirect(modalObj->create(state));
          }
        }
      }
      return true;
    }
    return false;
  });
}

} // namespace firmius::tui
