#include "modals/QuotasModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "components/ScrollableBox.hpp"
#include "harness/Harness.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace firmius::tui {

namespace {

/**
 * @brief Parse ISO 8601 timestamp to time_t
 */
std::time_t parseIso8601(const std::string &isoTime) {
  std::tm tm = {};
  std::istringstream ss(isoTime);
  ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  if (ss.fail()) {
    return 0;
  }
  return std::mktime(&tm);
}

/**
 * @brief Convert a timestamp to a human-readable relative time string
 */
std::string humanizeResetTime(const std::string &isoTime) {
  if (isoTime.empty()) {
    return "";
  }

  std::time_t resetTime = parseIso8601(isoTime);
  if (resetTime == 0) {
    return isoTime; // Return as-is if parsing fails
  }

  auto now = std::chrono::system_clock::now();
  std::time_t nowTime = std::chrono::system_clock::to_time_t(now);

  int64_t diff = static_cast<int64_t>(resetTime) - static_cast<int64_t>(nowTime);

  if (diff <= 0) {
    return "resetting now";
  }

  int64_t days = diff / 86400;
  int64_t hours = (diff % 86400) / 3600;
  int64_t minutes = (diff % 3600) / 60;

  std::ostringstream result;

  if (days > 0) {
    result << "in " << days << " day";
    if (days > 1)
      result << "s";
    if (hours > 0) {
      result << ", " << hours << " hour";
      if (hours > 1)
        result << "s";
    }
    if (minutes > 0 && days == 0) {
      result << ", " << minutes << " min";
    }
  } else if (hours > 0) {
    result << "in " << hours << " hour";
    if (hours > 1)
      result << "s";
    if (minutes > 0) {
      result << ", " << minutes << " min";
    }
  } else {
    result << "in " << minutes << " min";
  }

  return result.str();
}

} // namespace

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
            ss << std::fixed << std::setprecision(0)
               << (bucket.remainingFraction * 100.0f);
            std::string percentStr = ss.str() + "%";

            ftxui::Color color = theme.modals.highlight_fg;
            if (bucket.remainingFraction < 0.2f)
              color = theme.status_bar.error.normal.fg;
            else if (bucket.remainingFraction < 0.5f)
              color = theme.modals.title;

            // Compact mode: combine name + percentage
            std::string compactLabel = bucket.name + " " + percentStr;

            ftxui::Element resetInfo = ftxui::filler();
            if (!bucket.resetTime.empty()) {
              std::string humanized = humanizeResetTime(bucket.resetTime);
              resetInfo = ftxui::text(" | " + humanized) |
                          ftxui::color(theme.base.dim);
            }

            // Compact mode: name + % on left, gauge, then reset time
            bucket_elements.push_back(ftxui::hbox(
                {ftxui::text("  " + compactLabel) |
                     ftxui::color(color) | ftxui::bold,
                 ftxui::gauge(bucket.remainingFraction) | ftxui::color(color) |
                     ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 20) | ftxui::flex,
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
                           ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 22) |
                           ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 60),
                       ftxui::text(""),
                       ftxui::hbox({ftxui::text(" ESC/Enter ") | ftxui::bold |
                                        ftxui::color(theme.base.dim),
                                    ftxui::text("close") |
                                        ftxui::color(theme.modals.fg)}) |
                           ftxui::center,
                   })) |
               ftxui::clear_under | ftxui::center |
               ftxui::bgcolor(theme.modals.bg) |
               ftxui::color(theme.modals.border) |
               ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 65);
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
