#include "modals/QuotasModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "components/ScrollableBox.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace firmius::tui {

namespace {

struct HeaderHit {
  std::string account;
  ftxui::Box box;
};

struct QuotaModalWidths {
  int window_w;
  int content_w;
  int inner_w;
};

QuotaModalWidths computeQuotaModalWidths() {
  auto terminal = ftxui::Terminal::Size();
  int terminal_w = std::max(0, terminal.dimx);
  int preferred_w = 80;
  int padded_w = std::max(0, terminal_w - 4);
  int window_w = std::min(preferred_w, padded_w);
  window_w = std::max(10, window_w);
  if (terminal_w > 0) {
    window_w = std::min(window_w, terminal_w);
  }
  // Leave a column for the scroll indicator so borders don't clip.
  int content_w = std::max(8, window_w - 3);
  int inner_w = std::max(6, content_w - 2);
  return {window_w, content_w, inner_w};
}

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
  auto collapsed =
      std::make_shared<std::unordered_map<std::string, bool>>();
  auto header_hits = std::make_shared<std::vector<HeaderHit>>();

  std::thread([allQuotas, isLoading, providerId = providerId_, &state]() {
    *allQuotas = firmius::core::Harness::instance().getAllQuotas(providerId);
    *isLoading = false;
    state.postEvent(ftxui::Event::Custom);
  }).detach();

  auto content_renderer = ftxui::Renderer([allQuotas, isLoading, collapsed,
                                           header_hits,
                                           providerId = providerId_]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const auto widths = computeQuotaModalWidths();
    if (*isLoading) {
      return ftxui::vbox(
          {ftxui::text("Fetching quotas...") | ftxui::center |
               ftxui::color(theme.modals.fg),
           ftxui::text("") | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 5)});
    }

    ftxui::Elements accounts_elements;
    header_hits->clear();
    header_hits->reserve(allQuotas->size());
    int max_reset_len = 0;
    for (const auto &[account, buckets] : *allQuotas) {
      (void)account;
      for (const auto &bucket : buckets) {
        if (!bucket.resetTime.empty()) {
          std::string humanized = humanizeResetTime(bucket.resetTime);
          int len = static_cast<int>(humanized.size());
          if (len > max_reset_len) {
            max_reset_len = len;
          }
        }
      }
    }
    int reset_reserve = std::clamp(max_reset_len + 1, 12, 28);

    if (allQuotas->empty()) {
      accounts_elements.push_back(
          ftxui::text("No accounts or quotas found for " + providerId) |
          ftxui::color(theme.base.dim) | ftxui::center);
    } else {
      for (const auto &[account, buckets] : *allQuotas) {
        ftxui::Elements bucket_elements;
        bool isCollapsed = false;
        auto it = collapsed->find(account);
        if (it != collapsed->end()) {
          isCollapsed = it->second;
        } else {
          collapsed->emplace(account, false);
        }

        HeaderHit hit;
        hit.account = account;
        header_hits->push_back(hit);
        auto &hit_box = header_hits->back().box;
        std::string arrow = isCollapsed ? ">" : "v";
        auto header_line =
            ftxui::hbox(
                {ftxui::text(" " + arrow + " ") | ftxui::bold |
                     ftxui::color(theme.modals.highlight_fg),
                 ftxui::text("Account: ") | ftxui::bold |
                     ftxui::color(theme.modals.title),
                 ftxui::text(account) | ftxui::bold |
                     ftxui::color(theme.modals.highlight_fg) | ftxui::flex}) |
            ftxui::size(ftxui::WIDTH, ftxui::EQUAL, widths.inner_w) |
            ftxui::bgcolor(theme.modals.highlight_bg) |
            ftxui::reflect(hit_box);
        bucket_elements.push_back(header_line);
        bucket_elements.push_back(ftxui::separatorLight() |
                                  ftxui::color(theme.modals.border));

        if (isCollapsed) {
          // Skip rendering buckets when collapsed.
        } else if (buckets.empty()) {
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

            ftxui::Element resetInfo = ftxui::text("");
            if (!bucket.resetTime.empty()) {
              std::string humanized = humanizeResetTime(bucket.resetTime);
              resetInfo = ftxui::text(" " + humanized) |
                          ftxui::color(theme.base.dim) |
                          ftxui::size(ftxui::WIDTH, ftxui::EQUAL,
                                      reset_reserve) |
                          ftxui::align_right;
            }

            int gaugeWidth = std::max(10, widths.inner_w - reset_reserve);
            auto gauge = ftxui::gauge(bucket.remainingFraction) |
                         ftxui::color(color) |
                         ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1) |
                         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, gaugeWidth);
            auto label =
                ftxui::text(" " + compactLabel) | ftxui::bold |
                ftxui::color(theme.modals.fg) | ftxui::bgcolor(color);
            ftxui::Element gauge_with_label =
                ftxui::dbox({gauge, label}) | ftxui::flex;

            // Compact mode: label on gauge, then reset time
            bucket_elements.push_back(
                ftxui::hbox({gauge_with_label, resetInfo}));
          }
        }
        accounts_elements.push_back(ftxui::vbox(std::move(bucket_elements)) |
                                    ftxui::size(ftxui::WIDTH, ftxui::EQUAL,
                                                widths.inner_w));
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
        const auto widths = computeQuotaModalWidths();
        return FlatModalPanel(
            theme, "Provider Quotas: " + providerId,
            ModalSection(
                theme,
                ftxui::vbox({
                    scrollable_content->Render() |
                        ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 22) |
                        ftxui::size(ftxui::WIDTH, ftxui::EQUAL,
                                    widths.content_w),
                    ftxui::text(""),
                    ftxui::hbox({ftxui::text(" ESC/Enter ") | ftxui::bold |
                                     ftxui::color(theme.base.dim),
                                 ftxui::text("close") |
                                     ftxui::color(theme.modals.fg)}) |
                        ftxui::center,
                }),
                theme.modals.bg),
            widths.window_w + 2, 28);
      });

  return ftxui::CatchEvent(
      window_renderer,
      [&state, isLoading, scrollable_content, collapsed,
       header_hits](ftxui::Event event) {
        if (event == ftxui::Event::Escape || event == ftxui::Event::Return) {
          state.popModal();
          return true;
        }
        if (event.is_mouse() &&
            event.mouse().button == ftxui::Mouse::Left) {
          for (const auto &hit : *header_hits) {
            if (hit.box.Contain(event.mouse().x, event.mouse().y)) {
              bool &isCollapsed = (*collapsed)[hit.account];
              isCollapsed = !isCollapsed;
              return true;
            }
          }
        }
        return scrollable_content->OnEvent(event);
      });
}

} // namespace firmius::tui
