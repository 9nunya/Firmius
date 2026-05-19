#include "AccountsOverlay.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>

namespace firmius::tui {

static std::string humanizeResetTime(const std::string& resetTime) {
  if (resetTime.empty()) return "";

  int year, month, day, hour, min, sec;
  if (std::sscanf(resetTime.c_str(), "%d-%d-%dT%d:%d:%dZ",
                  &year, &month, &day, &hour, &min, &sec) != 6) {
    return resetTime;
  }

  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon  = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min  = min;
  tm.tm_sec  = sec;
  tm.tm_isdst = 0;

  std::time_t resetEpoch = timegm(&tm);
  if (resetEpoch == static_cast<std::time_t>(-1)) return resetTime;

  auto nowEpoch = std::chrono::system_clock::to_time_t(
      std::chrono::system_clock::now());
  long diffSecs = static_cast<long>(resetEpoch - nowEpoch);

  if (diffSecs <= 0) return "now";

  long diffMins  = diffSecs / 60;
  long diffHours = diffMins  / 60;
  long diffDays  = diffHours / 24;

  if (diffDays >= 1) {
    long remHours = diffHours % 24;
    return "in " + std::to_string(diffDays) +
           " day" + (diffDays != 1 ? "s" : "") + ", " +
           std::to_string(remHours) +
           " hour" + (remHours != 1 ? "s" : "");
  }
  if (diffHours >= 1) {
    long remMins = diffMins % 60;
    return "in " + std::to_string(diffHours) +
           " hour" + (diffHours != 1 ? "s" : "") + ", " +
           std::to_string(remMins) + " min";
  }
  if (diffMins >= 1) {
    return "in " + std::to_string(diffMins) + " min";
  }
  return "in " + std::to_string(diffSecs) + "s";
}

static std::string displayIdentifier(const firmius::daemon::AccountSnapshot& acct) {
  auto emailIt = acct.metadata.find("email");
  if (emailIt != acct.metadata.end() && !emailIt->second.empty()) {
    return emailIt->second;
  }
  if (!acct.identifier.empty()) {
    return acct.identifier;
  }
  auto subIt = acct.metadata.find("sub");
  if (subIt != acct.metadata.end() && !subIt->second.empty()) {
    return subIt->second;
  }
  return "(unnamed)";
}

void AccountsOverlay::load(std::string providerId,
                           std::vector<firmius::daemon::AccountSnapshot> accounts,
                           firmius::daemon::QuotaSnapshot quotas,
                           int termWidth) {
  providerId_ = std::move(providerId);
  termWidth_ = termWidth;
  entries_.clear();

  auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();

  for (const auto& acct : accounts) {
    AccountEntry e;
    e.displayId = displayIdentifier(acct);
    e.rawId = acct.identifier;
    e.rateLimited = acct.rateLimited;

    auto tierIt = acct.metadata.find("plan_tier");
    if (tierIt != acct.metadata.end()) {
      e.planTier = tierIt->second;
    }

    if (acct.backoffUntil > 0) {
      auto remaining = acct.backoffUntil - nowMs;
      if (remaining > 0) {
        e.backoffSecsRemaining = static_cast<int>(remaining / 1000);
      }
    }

    auto qit = quotas.buckets.find(acct.identifier);
    if (qit == quotas.buckets.end()) {
      qit = quotas.buckets.find(e.displayId);
    }
    if (qit != quotas.buckets.end()) {
      e.quotaBuckets = qit->second;
    }

    e.expanded = false;
    entries_.push_back(std::move(e));
  }

  cursorIdx_ = 0;
  mouseArmedIdx_ = -1;
  scrollOffset_ = 0;
}

void AccountsOverlay::open() {
  isOpen_ = true;
  cursorIdx_ = 0;
  mouseArmedIdx_ = -1;
  scrollOffset_ = 0;
}

void AccountsOverlay::close() {
  isOpen_ = false;
}

int AccountsOverlay::rowsForEntry(int idx) const {
  if (idx < 0 || idx >= static_cast<int>(entries_.size())) return 0;
  const auto& e = entries_[idx];
  int rows = 1; // header
  if (e.expanded) {
    if (!e.planTier.empty()) ++rows;
    if (e.backoffSecsRemaining > 0) ++rows;
    for (const auto& b : e.quotaBuckets) {
      rows += 2; // name+reset row + bar row
      if (!b.note.empty()) ++rows;
    }
    ++rows; // blank separator
  }
  return rows;
}

int AccountsOverlay::visibleRowCount() const {
  int total = 0;
  for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
    total += rowsForEntry(i);
  }
  return total;
}

void AccountsOverlay::clampScroll() {
  int totalRows = visibleRowCount();
  int maxOffset = std::max(0, totalRows - maxVisible_);
  scrollOffset_ = std::max(0, std::min(scrollOffset_, maxOffset));
}

int AccountsOverlay::entryAtRow(int row) const {
  int cur = 0;
  for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
    int r = rowsForEntry(i);
    if (row >= cur && row < cur + r) return i;
    cur += r;
  }
  return -1;
}

int AccountsOverlay::height(int /*width*/) const {
  if (!isOpen_) return 0;
  int rows = std::min(visibleRowCount(), maxVisible_);
  return 1 + 1 + rows + 1; // title + sep + content + hints
}

std::string AccountsOverlay::renderAccountHeader(const AccountEntry& e,
                                                  bool focused,
                                                  int width) const {
  std::string focusPrefix = focused ? theme_ansi::success("> ") : "  ";
  std::string arrow = e.expanded
      ? theme_ansi::accent("\xe2\x96\xbc ")   // ▼
      : theme_ansi::accent("\xe2\x96\xb6 ");  // ▶

  std::string label = e.displayId;
  std::string statusStr = e.rateLimited ? "rate-limited" : "connected";
  std::string statusColored = e.rateLimited
      ? theme_ansi::warning(statusStr)
      : theme_ansi::success(statusStr);

  int labelVisible = static_cast<int>(focusPrefix.size()) + 2 +
                     static_cast<int>(label.size());
  int statusVisible = static_cast<int>(statusStr.size());
  int pad = std::max(1, width - labelVisible - statusVisible - 1);
  std::string line = focusPrefix + arrow + ansi::bold(label) +
                     std::string(pad, ' ') + statusColored;

  if (focused) {
    line = theme_ansi::selection(ansi::fitToWidth(line, width));
  } else {
    line = ansi::fitToWidth(line, width);
  }
  return line;
}

std::vector<std::string> AccountsOverlay::renderAccountBody(const AccountEntry& e,
                                                             int width) const {
  std::vector<std::string> lines;
  int barWidth = std::min(40, width - 12);

  if (!e.planTier.empty()) {
    lines.push_back(ansi::fitToWidth(
        "  Plan: " + theme_ansi::success(e.planTier), width));
  }
  if (e.backoffSecsRemaining > 0) {
    lines.push_back(ansi::fitToWidth(
        "  Backoff: " + theme_ansi::warning(
            std::to_string(e.backoffSecsRemaining) + "s remaining"), width));
  }

  for (const auto& bucket : e.quotaBuckets) {
    int pct = static_cast<int>(bucket.remainingFraction * 100.0f);
    std::string pctBadge = std::to_string(pct) + "%";

    // Name + humanized reset time right-aligned
    std::string humanReset = humanizeResetTime(bucket.resetTime);
    std::string nameRow = "  " + ansi::bold(bucket.name);
    if (!humanReset.empty()) {
      int nameVisible = 2 + static_cast<int>(bucket.name.size());
      int resetVisible = static_cast<int>(humanReset.size());
      int p = std::max(1, width - nameVisible - resetVisible - 2);
      nameRow += std::string(p, ' ') + ansi::dim(humanReset);
    }
    lines.push_back(ansi::fitToWidth(nameRow, width));

    // Bar + % badge right-aligned
    int filled = static_cast<int>(bucket.remainingFraction * barWidth);
    filled = std::max(0, std::min(barWidth, filled));
    std::string bar;
    for (int j = 0; j < barWidth; ++j) {
      if (j < filled) {
        if (bucket.remainingFraction > 0.5f) {
          bar += theme_ansi::accent("\xe2\x96\x88");
        } else if (bucket.remainingFraction > 0.2f) {
          bar += theme_ansi::warning("\xe2\x96\x88");
        } else {
          bar += theme_ansi::error("\xe2\x96\x88");
        }
      } else {
        bar += theme_ansi::dim("\xe2\x96\x92");
      }
    }
    int barPad = std::max(1, width - barWidth - static_cast<int>(pctBadge.size()) - 5);
    std::string badge = (pct <= 20)
        ? theme_ansi::errorBg(" " + pctBadge + " ")
        : theme_ansi::panel(" " + pctBadge + " ");
    lines.push_back(ansi::fitToWidth("  " + bar + std::string(barPad, ' ') + badge, width));

    if (!bucket.note.empty()) {
      lines.push_back(ansi::fitToWidth("  Plan: " + ansi::dim(bucket.note), width));
    }
  }

  lines.push_back(""); // blank separator
  return lines;
}

std::vector<std::string> AccountsOverlay::render(int width) const {
  std::vector<std::string> lines;
  if (!isOpen_) return lines;

  // Title
  lines.push_back(theme_ansi::accent(
      ansi::bold(ansi::fitToWidth(" Accounts: " + providerId_, width))));
  // Separator
  lines.push_back(theme_ansi::divider(width));

  // Collect all renderable rows, skip by scrollOffset_
  int totalRows = visibleRowCount();
  int rowIdx = 0;
  int rendered = 0;

  for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
    const auto& e = entries_[i];
    bool focused = (i == cursorIdx_);

    // Header row
    if (rowIdx >= scrollOffset_ && rendered < maxVisible_) {
      lines.push_back(renderAccountHeader(e, focused, width));
      ++rendered;
    }
    ++rowIdx;

    // Body rows (only if expanded)
    if (e.expanded) {
      auto bodyLines = renderAccountBody(e, width);
      for (const auto& bl : bodyLines) {
        if (rowIdx >= scrollOffset_ && rendered < maxVisible_) {
          lines.push_back(bl);
          ++rendered;
        }
        ++rowIdx;
      }
    }

    if (rendered >= maxVisible_) break;
  }

  // Hints
  int pct = totalRows > 0
      ? std::min(100, (scrollOffset_ + maxVisible_) * 100 / totalRows)
      : 100;
  std::string hints;
  if (totalRows > maxVisible_) {
    hints = ansi::dim(" " + std::to_string(scrollOffset_ + 1) + "-" +
                      std::to_string(std::min(scrollOffset_ + maxVisible_, totalRows)) +
                      " of " + std::to_string(totalRows) +
                      " (" + std::to_string(pct) + "%)  ");
  }
  hints += ansi::dim("\xe2\x86\x91\xe2\x86\x93 navigate") +
           ansi::dim(" \xe2\x94\x82 ") +
           ansi::dim("Enter expand") +
           ansi::dim(" \xe2\x94\x82 ") +
           ansi::dim("Esc close");
  lines.push_back(ansi::fitToWidth(hints, width));

  return lines;
}

bool AccountsOverlay::handleInput(const std::string& key) {
  if (!isOpen_) return false;

  if (key == "\x1b") {
    if (onDismiss_) onDismiss_();
    return true;
  }

  if (key == "\r" || key == "\n" || key == " ") {
    if (cursorIdx_ >= 0 && cursorIdx_ < static_cast<int>(entries_.size())) {
      entries_[cursorIdx_].expanded = !entries_[cursorIdx_].expanded;
      clampScroll();
    }
    return true;
  }

  if (key == "\x1b[A") { // Up
    if (cursorIdx_ > 0) {
      --cursorIdx_;
      // Scroll up enough to show new cursor's header
      int rowOfCursor = 0;
      for (int i = 0; i < cursorIdx_; ++i) rowOfCursor += rowsForEntry(i);
      if (rowOfCursor < scrollOffset_) scrollOffset_ = rowOfCursor;
    }
    return true;
  }

  if (key == "\x1b[B") { // Down
    if (cursorIdx_ < static_cast<int>(entries_.size()) - 1) {
      ++cursorIdx_;
      // Scroll down enough to show new cursor's header
      int rowOfCursor = 0;
      for (int i = 0; i < cursorIdx_; ++i) rowOfCursor += rowsForEntry(i);
      if (rowOfCursor >= scrollOffset_ + maxVisible_) {
        scrollOffset_ = rowOfCursor - maxVisible_ + 1;
      }
    }
    return true;
  }

  if (key == "\x1b[5~") { // PgUp
    scrollOffset_ = std::max(0, scrollOffset_ - maxVisible_);
    return true;
  }
  if (key == "\x1b[6~") { // PgDn
    clampScroll();
    scrollOffset_ = std::min(scrollOffset_ + maxVisible_,
                             std::max(0, visibleRowCount() - maxVisible_));
    clampScroll();
    return true;
  }

  return false;
}

bool AccountsOverlay::handleMouse(const MouseEvent& event,
                                   int screenRow,
                                   int /*screenCol*/) {
  if (!isOpen_) return false;

  if (event.type == MouseEvent::Type::Scroll) {
    if (event.button == MouseEvent::Button::ScrollUp) {
      scrollOffset_ = std::max(0, scrollOffset_ - 1);
    } else if (event.button == MouseEvent::Button::ScrollDown) {
      scrollOffset_ = std::min(scrollOffset_ + 1,
                               std::max(0, visibleRowCount() - maxVisible_));
      clampScroll();
    }
    return true;
  }

  if (event.type != MouseEvent::Type::Press ||
      event.button != MouseEvent::Button::Left) {
    return false;
  }

  const int firstContentRow = screenRow + 2;
  const int relativeRow = event.row - firstContentRow;
  if (relativeRow < 0 || relativeRow >= maxVisible_) {
    return false;
  }

  const int entryIdx = entryAtRow(scrollOffset_ + relativeRow);
  if (entryIdx < 0 || entryIdx >= static_cast<int>(entries_.size())) {
    return false;
  }

  if (cursorIdx_ == entryIdx) {
    if (mouseArmedIdx_ == entryIdx) {
      entries_[entryIdx].expanded = !entries_[entryIdx].expanded;
      clampScroll();
    } else {
      mouseArmedIdx_ = entryIdx;
    }
  } else {
    cursorIdx_ = entryIdx;
    mouseArmedIdx_ = entryIdx;
  }
  return true;
}

} // namespace firmius::tui
