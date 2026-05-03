#include "modals/AccountsModal.hpp"

#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "components/ScrollableBox.hpp"
#include "harness/Harness.hpp"
#include "modals/APIKeyWizardModal.hpp"
#include "modals/ModalLayout.hpp"
#include "modals/OAuthWizardModal.hpp"
#include "providers/BaseAPIKeyProvider.hpp"
#include "providers/BaseOAuthProvider.hpp"
#include "providers/ProviderRegistry.hpp"
#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {

AccountsModal::AccountsModal(std::string providerId)
    : providerId_(std::move(providerId)) {}

ftxui::Component AccountsModal::create(TuiState &state) {
  auto oauthAccounts =
      std::make_shared<std::vector<firmius::shared::OAuthAccount>>();
  auto selected = std::make_shared<int>(0);
  auto providerId = providerId_;
  auto rowBoxes = std::make_shared<std::vector<ftxui::Box>>();

  auto accountCount = [oauthAccounts]() {
    return static_cast<int>(oauthAccounts->size());
  };

  auto refreshAccounts = [oauthAccounts, providerId, selected,
                          accountCount, &state]() {
    (void)accountCount;
    (void)state;
    *oauthAccounts = firmius::core::Harness::instance().getAccounts(providerId);
    const int count = static_cast<int>(oauthAccounts->size());
    *selected = count <= 0 ? 0 : std::clamp(*selected, 0, count - 1);
  };

  refreshAccounts();

  auto listContent = ftxui::Renderer([oauthAccounts, selected, rowBoxes]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const int total = static_cast<int>(oauthAccounts->size());
    rowBoxes->assign(total, ftxui::Box{});

    if (total == 0) {
      return ftxui::vbox({
          ftxui::text("No accounts connected.") | ftxui::center |
              ftxui::color(theme.base.dim),
      });
    }

    ftxui::Elements rows;
    rows.reserve(total * 2);
    for (int i = 0; i < total; ++i) {
      const auto &acc = (*oauthAccounts)[i];
      std::string identifier = acc.identifier;
      if (auto emailIt = acc.metadata.find("email");
          emailIt != acc.metadata.end() && !emailIt->second.empty()) {
        identifier = emailIt->second;
      }
      std::string rightBadge = "oauth";

      if (auto keyPrefixIt = acc.metadata.find("keyPrefix");
          keyPrefixIt != acc.metadata.end()) {
        identifier += " (" + keyPrefixIt->second + "...)";
        rightBadge = "key";
      }

      auto row = ftxui::hbox({
                     ftxui::text("  "),
                     ftxui::text(identifier) | ftxui::bold |
                         ftxui::color(theme.modals.fg),
                     ftxui::filler(),
                     ftxui::text(rightBadge + "  ") |
                         ftxui::color(theme.base.dim),
                 }) |
                 ftxui::reflect(rowBoxes->at(i));

      if (i == *selected) {
        row = row | ftxui::bgcolor(theme.modals.highlight_bg) |
              ftxui::color(theme.modals.highlight_fg);
      }

      rows.push_back(row);
      rows.push_back(ftxui::text(""));
    }
    return ftxui::vbox(std::move(rows));
  });

  auto scrollable = ScrollableBox(listContent);

  auto component = ftxui::Renderer(scrollable, [oauthAccounts, selected,
                                                providerId, scrollable,
                                                accountCount]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const auto terminal = ftxui::Terminal::Size();
    const int panelWidth = std::clamp(std::max(0, terminal.dimx - 8), 58, 90);
    const int panelHeight = std::clamp(std::max(0, terminal.dimy - 6), 18, 24);
    const int total = accountCount();
    const int listHeight = std::max(6, panelHeight - 11);

    auto body = ftxui::vbox({
        ftxui::text("Select an account to manage:") |
            ftxui::color(theme.base.dim),
        ftxui::text(""),
        ftxui::hbox({
            ftxui::text(" Connected Accounts ") | ftxui::bold |
                ftxui::color(theme.modals.fg),
            ftxui::filler(),
            ftxui::text(" " + std::to_string(total) + " total ") |
                ftxui::bgcolor(theme.modals.highlight_bg) |
                ftxui::color(theme.modals.highlight_fg),
        }),
        ftxui::text(""),
        scrollable->Render() | ftxui::xflex |
            ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, listHeight),
        ftxui::text(""),
        ftxui::hbox(
            {ftxui::text(" [A] ") | ftxui::bold |
                 ftxui::color(theme.modals.highlight_fg),
             ftxui::text(" Add Account   ") | ftxui::color(theme.modals.fg),
             ftxui::text(" [D] ") | ftxui::bold |
                 ftxui::color(theme.status_bar.error.normal.fg),
             ftxui::text(" Delete Selected ") |
                 ftxui::color(theme.modals.fg),
             ftxui::filler(),
             ftxui::text(" [ESC] ") | ftxui::bold |
                 ftxui::color(theme.base.dim),
             ftxui::text(" Close ") | ftxui::color(theme.modals.fg)}) |
            ftxui::center,
        ftxui::separatorLight() | ftxui::color(theme.modals.border),
        ftxui::text("↑↓ navigate, wheel scroll, click select") |
            ftxui::center | ftxui::color(theme.base.dim),
    });

    return FlatModalPanel(theme, "Manage Accounts: " + providerId,
                          ModalSection(theme, std::move(body), theme.modals.bg),
                          panelWidth, panelHeight);
  });

  return ftxui::CatchEvent(component, [oauthAccounts, selected, providerId,
                                       refreshAccounts, rowBoxes, scrollable,
                                       accountCount, &state](ftxui::Event event) {
    const auto clampSelection = [&]() {
      const int count = accountCount();
      *selected = count <= 0 ? 0 : std::clamp(*selected, 0, count - 1);
    };

    if (event == ftxui::Event::Escape) {
      state.popModal();
      return true;
    }
    if (event == ftxui::Event::ArrowUp || event == ftxui::Event::PageUp ||
        event == ftxui::Event::Home) {
      if (event == ftxui::Event::Home) {
        *selected = 0;
      } else if (event == ftxui::Event::PageUp) {
        *selected = std::max(0, *selected - 5);
      } else if (*selected > 0) {
        --(*selected);
      }
      clampSelection();
      scrollable->OnEvent(event);
      return true;
    }
    if (event == ftxui::Event::ArrowDown || event == ftxui::Event::PageDown ||
        event == ftxui::Event::End) {
      const int count = accountCount();
      if (event == ftxui::Event::End && count > 0) {
        *selected = count - 1;
      } else if (event == ftxui::Event::PageDown && count > 0) {
        *selected = std::min(count - 1, *selected + 5);
      } else if (*selected < count - 1) {
        ++(*selected);
      }
      clampSelection();
      scrollable->OnEvent(event);
      return true;
    }
    if (event.is_mouse()) {
      const auto mouse = event.mouse();
      const bool isDragRelatedLeftMouse =
          mouse.button == ftxui::Mouse::Left ||
          mouse.motion == ftxui::Mouse::Moved ||
          mouse.motion == ftxui::Mouse::Released;
      if (isDragRelatedLeftMouse && scrollable->OnEvent(event)) {
        return true;
      }
      if (mouse.button == ftxui::Mouse::WheelUp && *selected > 0) {
        --(*selected);
      }
      if (mouse.button == ftxui::Mouse::WheelDown &&
          *selected < accountCount() - 1) {
        ++(*selected);
      }
      clampSelection();
      if (mouse.button == ftxui::Mouse::Left &&
          mouse.motion == ftxui::Mouse::Pressed) {
        for (int i = 0; i < static_cast<int>(rowBoxes->size()); ++i) {
          if (rowBoxes->at(i).Contain(mouse.x, mouse.y)) {
            *selected = i;
            return true;
          }
        }
      }
      if (isDragRelatedLeftMouse) {
        return false;
      }
      return scrollable->OnEvent(event);
    }
    if (event == ftxui::Event::Delete ||
        event == ftxui::Event::Character('d') ||
        event == ftxui::Event::Character('D')) {
      if (!oauthAccounts->empty() &&
          *selected < static_cast<int>(oauthAccounts->size())) {
        const std::string identifier = (*oauthAccounts)[*selected].identifier;
        firmius::core::Harness::instance().deleteAccount(providerId, identifier);
        refreshAccounts();
      }
      return true;
    }
    if (event == ftxui::Event::Character('a') ||
        event == ftxui::Event::Character('A')) {
      auto provider =
          firmius::provider::ProviderRegistry::instance().getProvider(
              providerId);
      if (provider) {
        switch (provider->getProviderType()) {
        case firmius::provider::ProviderType::APIKey: {
          auto apiKeyProvider = std::dynamic_pointer_cast<
              firmius::provider::BaseAPIKeyProvider>(provider);
          if (apiKeyProvider) {
            auto wizard = apiKeyProvider->beginConnectionWizard();
            if (wizard) {
              auto modalObj = std::make_shared<APIKeyWizardModal>(
                  std::move(wizard), providerId);
              state.deferUiMutation([&state, modalObj]() {
                state.popModalImmediate();
                state.openModalDirect(modalObj->create(state));
              });
              return true;
            }
          }
          return false;
        }
        case firmius::provider::ProviderType::OAuth: {
          auto oauthProvider = std::dynamic_pointer_cast<
              firmius::provider::BaseOAuthProvider>(provider);
          if (oauthProvider) {
            auto wizard = oauthProvider->beginConnectionWizard();
            if (wizard) {
              auto modalObj = std::make_shared<OAuthWizardModal>(
                  std::move(wizard), providerId);
              state.deferUiMutation([&state, modalObj]() {
                state.popModalImmediate();
                state.openModalDirect(modalObj->create(state));
              });
              return true;
            }
          }
          return false;
        }
        }
      }
      return false;
    }
    return false;
  });

}

} // namespace firmius::tui
