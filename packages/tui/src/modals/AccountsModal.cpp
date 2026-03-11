#include "modals/AccountsModal.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include "modals/OAuthWizardModal.hpp"
#include "modals/APIKeyWizardModal.hpp"
#include "providers/ProviderRegistry.hpp"
#include "providers/BaseOAuthProvider.hpp"
#include "providers/BaseAPIKeyProvider.hpp"
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
          firmius::provider::ProviderRegistry::instance().getProvider(providerId);
      
      if (provider) {
        auto apiKeyProvider =
            std::dynamic_pointer_cast<firmius::provider::BaseAPIKeyProvider>(provider);
        if (apiKeyProvider) {
          *isAPIKeyProvider = true;
          *apiKeyAccounts = apiKeyProvider->getAccounts();
        } else {
          *isAPIKeyProvider = false;
          *oauthAccounts = firmius::core::Harness::instance().getAccounts(providerId);
        }
      }
      *isLoading = false;
      state.postEvent(ftxui::Event::Custom);
    }).detach();
  };

  refreshAccounts();

  auto component =
      ftxui::Renderer([oauthAccounts, apiKeyAccounts, selected, isLoading, 
                       providerId, isAPIKeyProvider]() {
        if (*isLoading) {
          return ftxui::window(
                     ftxui::text(" Accounts: " + providerId) | ftxui::bold |
                         ftxui::color(ftxui::Color::Cyan),
                     ftxui::vbox(
                         {ftxui::text("Loading accounts...") | ftxui::center,
                          ftxui::text("") |
                              ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 5)})) |
                 ftxui::clear_under | ftxui::center;
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
                      ftxui::color(ftxui::Color::Yellow);
            } else {
              label = label | ftxui::color(ftxui::Color::GrayDark);
            }
            rows.push_back(label);
          }
        } else {
          // Display OAuth accounts
          for (int i = 0; i < (int)oauthAccounts->size(); ++i) {
            auto label = ftxui::text("  " + (*oauthAccounts)[i].identifier + "  ");
            if (i == *selected) {
              label = label | ftxui::inverted | ftxui::bold |
                      ftxui::color(ftxui::Color::Yellow);
            } else {
              label = label | ftxui::color(ftxui::Color::GrayDark);
            }
            rows.push_back(label);
          }
        }

        if (rows.empty()) {
          rows.push_back(ftxui::text("No accounts connected.") | ftxui::dim |
                         ftxui::center);
        }

        auto window_title =
            ftxui::hbox({ftxui::text(" Manage Accounts: ") | ftxui::bold,
                         ftxui::text(providerId) | ftxui::bold |
                             ftxui::color(ftxui::Color::Cyan)});

        return ftxui::window(
                   window_title,
                   ftxui::vbox({
                       ftxui::text("Select an account to manage:") | ftxui::dim,
                       ftxui::text(""),
                       ftxui::vbox(rows) | ftxui::vscroll_indicator |
                           ftxui::yframe |
                           ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 8) |
                           ftxui::borderRounded |
                           ftxui::color(ftxui::Color::GrayLight),
                       ftxui::text(""),
                       ftxui::hbox({ftxui::text(" [A] ") | ftxui::bold |
                                        ftxui::color(ftxui::Color::Blue),
                                    ftxui::text(" Add Account   "),
                                    ftxui::text(" [D] ") | ftxui::bold |
                                        ftxui::color(ftxui::Color::Red),
                                    ftxui::text(" Delete Selected "),
                                    ftxui::filler(),
                                    ftxui::text(" [ESC] ") | ftxui::bold |
                                        ftxui::color(ftxui::Color::GrayLight),
                                    ftxui::text(" Close ")}) |
                           ftxui::center,
                       ftxui::separatorLight(),
                       ftxui::hbox({ftxui::text(" ↑↓ ") | ftxui::bold |
                                        ftxui::color(ftxui::Color::Cyan),
                                    ftxui::text("navigate elements")}) |
                           ftxui::center | ftxui::dim,
                   })) |
               ftxui::clear_under | ftxui::center;
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
      if (*selected < (int)(*isAPIKeyProvider ? apiKeyAccounts->size() : oauthAccounts->size()) - 1)
        (*selected)++;
      return true;
    }
    if (event == ftxui::Event::Delete ||
        event == ftxui::Event::Character('d') ||
        event == ftxui::Event::Character('D')) {
      if (!(*isAPIKeyProvider ? apiKeyAccounts->empty() : oauthAccounts->empty()) && 
          *selected < (int)(*isAPIKeyProvider ? apiKeyAccounts->size() : oauthAccounts->size())) {
        std::string identifier = *isAPIKeyProvider 
            ? (*apiKeyAccounts)[*selected].identifier 
            : (*oauthAccounts)[*selected].identifier;
        firmius::core::Harness::instance().deleteAccount(providerId, identifier);
        refreshAccounts();
        if (*selected >= (int)(*isAPIKeyProvider ? apiKeyAccounts->size() : oauthAccounts->size()) && *selected > 0)
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
