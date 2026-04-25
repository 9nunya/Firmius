#include "modals/QuotasModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "components/ScrollableBox.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <ftxui/component/component.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
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
  int preferred_w = 92;
  int padded_w = std::max(0, terminal_w - 6);
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
#if defined(_WIN32)
  return _mkgmtime(&tm);
#else
  return timegm(&tm);
#endif
}

std::time_t parseResetTime(const std::string &value) {
  if (value.empty()) {
    return 0;
  }
  if (std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch);
      })) {
    try {
      return static_cast<std::time_t>(std::stoll(value));
    } catch (...) {
      return 0;
    }
  }
  return parseIso8601(value);
}

std::string prettifyBucketName(const std::string &bucketName) {
  if (bucketName == "codex") {
    return "Codex limit";
  }
  std::string pretty = bucketName;
  std::replace(pretty.begin(), pretty.end(), '_', ' ');
  return pretty;
}

std::string repeatGlyph(const std::string &glyph, int count) {
  std::string out;
  for (int i = 0; i < std::max(0, count); ++i) {
    out += glyph;
  }
  return out;
}

/**
 * @brief Convert a timestamp to a human-readable relative time string
 */
std::string humanizeResetTime(const std::string &rawTime) {
  if (rawTime.empty()) {
    return "";
  }

  std::time_t resetTime = parseResetTime(rawTime);
  if (resetTime == 0) {
    return rawTime;
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
  (void)state;
  auto allQuotas = std::make_shared<
      std::map<std::string, std::vector<firmius::shared::QuotaBucket>>>();
  auto collapsed =
      std::make_shared<std::unordered_map<std::string, bool>>();
  auto header_hits = std::make_shared<std::vector<HeaderHit>>();

  *allQuotas = firmius::core::Harness::instance().getAllQuotas(providerId_);

  auto content_renderer = ftxui::Renderer([allQuotas, collapsed, header_hits,
                                           providerId = providerId_]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const auto widths = computeQuotaModalWidths();
    const int accountWidth = std::max(10, widths.inner_w - 4);

    ftxui::Elements accounts_elements;
    header_hits->clear();
    header_hits->reserve(allQuotas->size());

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
        std::string arrow = isCollapsed ? "▸" : "▾";
        auto header_line =
            ftxui::hbox(
                {ftxui::text(" " + arrow + " ") | ftxui::bold |
                     ftxui::color(theme.modals.highlight_fg),
                 ftxui::text(account) | ftxui::bold |
                     ftxui::color(theme.modals.highlight_fg) | ftxui::flex,
                 ftxui::text(" connected ") |
                     ftxui::color(theme.base.dim)}) |
            ftxui::size(ftxui::WIDTH, ftxui::EQUAL, accountWidth) |
            ftxui::bgcolor(theme.modals.highlight_bg) | ftxui::reflect(hit_box);
        bucket_elements.push_back(header_line);
        bucket_elements.push_back(ftxui::text(""));

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

            std::string humanized = humanizeResetTime(bucket.resetTime);
            auto percentBadge = ftxui::text(" " + percentStr + " ") |
                                ftxui::bold |
                                ftxui::color(theme.modals.bg) |
                                ftxui::bgcolor(color);
            ftxui::Element resetInfo = ftxui::text("");
            if (!humanized.empty()) {
              resetInfo = ftxui::text(" " + humanized + " ") |
                          ftxui::color(theme.base.dim) |
                          ftxui::bgcolor(theme.modals.highlight_bg);
            }

            int barUnits = std::clamp(accountWidth - 10, 18, 36);
            const int filledUnits =
                static_cast<int>(std::round(std::clamp(bucket.remainingFraction,
                                                       0.0f, 1.0f) *
                                            barUnits));
            const int emptyUnits = std::max(0, barUnits - filledUnits);
            auto meterRow =
                ftxui::hbox({
                    ftxui::text(repeatGlyph("█", filledUnits)) |
                        ftxui::color(color),
                    ftxui::text(repeatGlyph("░", emptyUnits)) |
                        ftxui::color(theme.modals.border) | ftxui::dim,
                    ftxui::filler(),
                    percentBadge,
                });

            ftxui::Elements bucketBody = {
                ftxui::hbox({
                    ftxui::text(" " + prettifyBucketName(bucket.name)) |
                        ftxui::bold | ftxui::color(theme.modals.fg),
                    ftxui::filler(),
                    resetInfo,
                }),
                meterRow,
            };

            if (!bucket.note.empty()) {
              bucketBody.push_back(
                  ftxui::paragraph(" " + bucket.note) |
                  ftxui::color(theme.base.dim));
            }
            bucketBody.push_back(ftxui::text(""));

            bucket_elements.push_back(
                ftxui::vbox(std::move(bucketBody)) |
                ftxui::size(ftxui::WIDTH, ftxui::EQUAL, accountWidth));
          }
        }
        accounts_elements.push_back(
            ftxui::hbox({
                ftxui::text("  "),
                ftxui::vbox(std::move(bucket_elements)) |
                    ftxui::size(ftxui::WIDTH, ftxui::EQUAL, accountWidth) |
                    ftxui::flex,
                ftxui::text("  "),
            }));
        accounts_elements.push_back(ftxui::text("")); // Spacer
      }
    }
    return ftxui::vbox(std::move(accounts_elements));
  });

  auto scrollable_content = ScrollableBox(content_renderer);

  auto window_renderer =
      ftxui::Renderer(scrollable_content, [scrollable_content,
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
      [&state, scrollable_content, collapsed, header_hits](ftxui::Event event) {
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
