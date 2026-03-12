#include "modals/QuotasModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "components/ScrollableBox.hpp"
#include "harness/Harness.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace firmius::tui {

QuotasModal::QuotasModal(std::string providerId)
    : providerId_(std::move(providerId)) {}

ftxui::Component QuotasModal::create(TuiState &state) {
  auto allQuotas = std::make_shared<
      std::map<std::string, std::vector<firmius::shared::QuotaBucket>>>();
  auto isLoading = std::make_shared<bool>(true);

  std::thread([allQuotas, isLoading, providerId = providerId_, &state]() {
    *allQuotas = firmius::core::Harness::instance().getAllQuotas(providerId);
    *isLoading = false;
    state.postEvent(ftxui::Event::Custom);
  }).detach();

  auto content_renderer = ftxui::Renderer([allQuotas, isLoading,
                                           providerId = providerId_]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    if (*isLoading) {
      return ftxui::vbox(
          {ftxui::text("Fetching quotas...") | ftxui::center |
               ftxui::color(theme.modals.fg),
           ftxui::text("") | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 5)});
    }

    ftxui::Elements accounts_elements;
    if (allQuotas->empty()) {
      accounts_elements.push_back(
          ftxui::text("No accounts or quotas found for " + providerId) |
          ftxui::color(theme.base.dim) | ftxui::center);
    } else {
      for (const auto &[account, buckets] : *allQuotas) {
        ftxui::Elements bucket_elements;
        bucket_elements.push_back(
            ftxui::hbox({ftxui::text(" Account: ") | ftxui::bold |
                             ftxui::color(theme.modals.title),
                         ftxui::text(account) | ftxui::bold |
                             ftxui::color(theme.modals.highlight_fg)}));
        bucket_elements.push_back(ftxui::separatorLight() |
                                  ftxui::color(theme.modals.border));

        if (buckets.empty()) {
          bucket_elements.push_back(ftxui::text("  (No quota data available)") |
                                    ftxui::color(theme.base.dim));
        } else {
          for (const auto &bucket : buckets) {
            std::stringstream ss;
            ss << std::fixed << std::setprecision(1)
               << (bucket.remainingFraction * 100.0f) << "%";

            ftxui::Color color = theme.modals.highlight_fg;
            if (bucket.remainingFraction < 0.2f)
              color = theme.status_bar.error.normal.fg;
            else if (bucket.remainingFraction < 0.5f)
              color = theme.modals.title;

            ftxui::Element resetInfo = ftxui::filler();
            if (!bucket.resetTime.empty()) {
              resetInfo = ftxui::text(" (Resets: " + bucket.resetTime + ")") |
                          ftxui::color(theme.base.dim);
            }

            bucket_elements.push_back(ftxui::hbox(
                {ftxui::text("  " + bucket.name) |
                     ftxui::color(theme.modals.fg) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 17),
                 ftxui::gauge(bucket.remainingFraction) | ftxui::color(color) |
                     ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1) | ftxui::flex,
                 ftxui::text(" " + ss.str()) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 7) | ftxui::bold |
                     ftxui::color(color),
                 resetInfo}));
          }
        }
        accounts_elements.push_back(ftxui::vbox(std::move(bucket_elements)) |
                                    ftxui::borderRounded |
                                    ftxui::color(theme.modals.border));
        accounts_elements.push_back(ftxui::text("")); // Spacer
      }
    }
    return ftxui::vbox(std::move(accounts_elements));
  });

  auto scrollable_content = ScrollableBox(content_renderer);

  auto window_renderer =
      ftxui::Renderer(scrollable_content, [scrollable_content, isLoading,
                                           providerId = providerId_]() {
        const auto &theme = ThemeManager::instance().getCurrentTheme();
        auto window_title =
            ftxui::hbox({ftxui::text(" Provider Quotas: ") | ftxui::bold |
                             ftxui::color(theme.modals.title),
                         ftxui::text(providerId) | ftxui::bold |
                             ftxui::color(theme.modals.highlight_fg)});

        return ftxui::window(
                   window_title,
                   ftxui::vbox({
                       scrollable_content->Render() |
                           ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 22),
                       ftxui::text(""),
                       ftxui::hbox({ftxui::text(" ESC/Enter ") | ftxui::bold |
                                        ftxui::color(theme.base.dim),
                                    ftxui::text("close") |
                                        ftxui::color(theme.modals.fg)}) |
                           ftxui::center,
                   })) |
               ftxui::clear_under | ftxui::center |
               ftxui::bgcolor(theme.modals.bg) |
               ftxui::color(theme.modals.border);
      });

  return ftxui::CatchEvent(
      window_renderer,
      [&state, isLoading, scrollable_content](ftxui::Event event) {
        if (event == ftxui::Event::Escape || event == ftxui::Event::Return) {
          state.popModalImmediate();
          return true;
        }
        return scrollable_content->OnEvent(event);
      });
}

} // namespace firmius::tui
