#include "modals/QuotasModal.hpp"
#include "TUIState.hpp"
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

  auto component = ftxui::Renderer([allQuotas, isLoading,
                                    providerId = providerId_]() {
    if (*isLoading) {
      return ftxui::window(
                 ftxui::text(" Quotas: " + providerId) | ftxui::bold |
                     ftxui::color(ftxui::Color::Cyan),
                 ftxui::vbox(
                     {ftxui::text("Fetching quotas...") | ftxui::center,
                      ftxui::text("") |
                          ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 5)})) |
             ftxui::clear_under | ftxui::center;
    }

    ftxui::Elements accounts_elements;
    if (allQuotas->empty()) {
      accounts_elements.push_back(
          ftxui::text("No accounts or quotas found for " + providerId) |
          ftxui::dim | ftxui::center);
    } else {
      for (const auto &[account, buckets] : *allQuotas) {
        ftxui::Elements bucket_elements;
        bucket_elements.push_back(
            ftxui::hbox({ftxui::text(" Account: ") | ftxui::bold |
                             ftxui::color(ftxui::Color::Yellow),
                         ftxui::text(account) | ftxui::bold |
                             ftxui::color(ftxui::Color::YellowLight)}));
        bucket_elements.push_back(ftxui::separatorLight());

        if (buckets.empty()) {
          bucket_elements.push_back(ftxui::text("  (No quota data available)") |
                                    ftxui::dim);
        } else {
          for (const auto &bucket : buckets) {
            std::stringstream ss;
            ss << std::fixed << std::setprecision(1)
               << (bucket.remainingFraction * 100.0f) << "%";

            ftxui::Color color = ftxui::Color::Green;
            if (bucket.remainingFraction < 0.2f)
              color = ftxui::Color::Red;
            else if (bucket.remainingFraction < 0.5f)
              color = ftxui::Color::Yellow;

            ftxui::Element resetInfo = ftxui::filler();
            if (!bucket.resetTime.empty()) {
              resetInfo = ftxui::text(" (Resets: " + bucket.resetTime + ")") |
                          ftxui::dim;
            }

            bucket_elements.push_back(ftxui::hbox(
                {ftxui::text("  " + bucket.name) |
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
                                    ftxui::color(ftxui::Color::GrayDark));
        accounts_elements.push_back(ftxui::text("")); // Spacer
      }
    }

    auto window_title =
        ftxui::hbox({ftxui::text(" Provider Quotas: ") | ftxui::bold,
                     ftxui::text(providerId) | ftxui::bold |
                         ftxui::color(ftxui::Color::Cyan)});

    return ftxui::window(
               window_title,
               ftxui::vbox({
                   ftxui::vbox(std::move(accounts_elements)) |
                       ftxui::vscroll_indicator | ftxui::yframe |
                       ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 22),
                   ftxui::text(""),
                   ftxui::hbox({ftxui::text(" ESC/Enter ") | ftxui::bold |
                                    ftxui::color(ftxui::Color::GrayDark),
                                ftxui::text("close")}) |
                       ftxui::center | ftxui::dim,
               })) |
           ftxui::clear_under | ftxui::center;
  });

  return ftxui::CatchEvent(component, [&state, isLoading](ftxui::Event event) {
    if (event == ftxui::Event::Escape || event == ftxui::Event::Return) {
      state.popModalImmediate();
      return true;
    }
    return false;
  });
}

} // namespace firmius::tui
