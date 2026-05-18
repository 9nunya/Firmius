
#include "RedoOverlay.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace firmius::tui2 {

namespace {

// Same relative-time formatter as RewindOverlay — kept local rather than
// moved into a shared helper because the surface is small and the two
// overlays evolve together.
std::string formatRelative(std::uint64_t pastMs) {
  if (pastMs == 0) return "";
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  if (static_cast<std::uint64_t>(now) <= pastMs) return "just now";
  const auto deltaS = (static_cast<std::uint64_t>(now) - pastMs) / 1000;
  if (deltaS < 60)    return std::to_string(deltaS) + "s ago";
  if (deltaS < 3600)  return std::to_string(deltaS / 60) + "m ago";
  if (deltaS < 86400) return std::to_string(deltaS / 3600) + "h ago";
  if (deltaS < 86400ULL * 30) return std::to_string(deltaS / 86400) + "d ago";
  return "long ago";
}

std::string truncate(const std::string& text, int budget) {
  if (budget <= 0) return "";
  if (static_cast<int>(text.size()) <= budget) return text;
  if (budget <= 1) return text.substr(0, budget);
  return text.substr(0, budget - 1) + "…";
}

}  // namespace

void RedoOverlay::setActions(
    std::vector<firmius::daemon::RedoUndoActionSummary> actions) {
  actions_ = std::move(actions);
  cursorIdx_ = 0;
  scrollOffset_ = 0;
  errorMessage_.clear();
  phase_ = Phase::PickingAction;
}

void RedoOverlay::setExecuting(bool executing) {
  if (executing) {
    phase_ = Phase::Executing;
    errorMessage_.clear();
  } else if (phase_ == Phase::Executing) {
    phase_ = Phase::PickingMode;
  }
}

void RedoOverlay::showError(const std::string& message) {
  errorMessage_ = message;
}

void RedoOverlay::open() {
  isOpen_ = true;
  phase_ = Phase::PickingAction;
  errorMessage_.clear();
}

void RedoOverlay::close() {
  isOpen_ = false;
  actions_.clear();
  cursorIdx_ = 0;
  scrollOffset_ = 0;
  modeCursor_ = 0;
  errorMessage_.clear();
  phase_ = Phase::PickingAction;
}

int RedoOverlay::height(int /*width*/) const {
  if (!isOpen_) return 0;
  if (phase_ == Phase::PickingMode) return 11;
  if (phase_ == Phase::Executing)   return 6;
  const int rows = std::min<int>(actions_.size(), kMaxVisibleActions);
  return 2 + std::max(rows, 1) + 2;
}

std::string RedoOverlay::renderActionRow(
    const firmius::daemon::RedoUndoActionSummary& action,
    bool selected, int width) const {
  const std::string marker = selected ? theme_ansi::accent("> ")
                                      : std::string("  ");
  const std::string time = formatRelative(action.createdAt);
  std::ostringstream badge;
  if (action.turnsToRedo > 0) {
    badge << action.turnsToRedo << "t";
  }
  if (action.editBatchesToRedo > 0) {
    if (badge.tellp() > 0) badge << "/";
    badge << action.editBatchesToRedo << "b";
  }
  const std::string badgeText = badge.str();

  // budget: width - margin - marker - time - badge - separator
  const int reserved = 4 + static_cast<int>(time.size()) +
                       static_cast<int>(badgeText.size()) + 4;
  const int budget = std::max(10, width - reserved);
  std::string previewText = action.firstTurnPreview.empty()
                                ? std::string("(no preview)")
                                : truncate(action.firstTurnPreview, budget);
  std::string content = previewText;
  if (!action.redoAvailable) {
    content = theme_ansi::dim(content + "  (already redone)");
  } else {
    content = theme_ansi::foreground(content);
  }

  std::string line = "  " + marker + content;
  if (!badgeText.empty()) {
    line += "  " + theme_ansi::accent("[" + badgeText + "]");
  }
  if (!time.empty()) {
    line += "  " + theme_ansi::dim(time);
  }
  std::string fitted = ansi::fitToWidth(line, width);
  return selected ? theme_ansi::selection(fitted) : fitted;
}

bool RedoOverlay::modeAvailable(firmius::daemon::RedoMode mode) const {
  using M = firmius::daemon::RedoMode;
  if (cursorIdx_ < 0 ||
      cursorIdx_ >= static_cast<int>(actions_.size())) {
    return false;
  }
  const auto& a = actions_[cursorIdx_];
  if (!a.redoAvailable) return false;
  if (mode == M::RestoreCode || mode == M::RestoreCodeAndConversation) {
    if (a.editBatchesToRedo == 0 && mode == M::RestoreCode) {
      return false;
    }
  }
  if (mode == M::RestoreConversation || mode == M::RestoreCodeAndConversation) {
    if (a.turnsToRedo == 0 && mode == M::RestoreConversation) {
      return false;
    }
    if (a.turnsToRedo == 0 && a.editBatchesToRedo == 0 &&
        mode == M::RestoreCodeAndConversation) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> RedoOverlay::renderModePane(int width) const {
  std::vector<std::string> out;
  using M = firmius::daemon::RedoMode;
  struct Item { M mode; const char *label; };
  const Item items[] = {
      {M::RestoreCodeAndConversation, "Redo conversation and code"},
      {M::RestoreConversation,        "Redo conversation"},
      {M::RestoreCode,                "Redo code"},
  };
  out.push_back(ansi::fitToWidth(
      theme_ansi::accent(ansi::bold("  Choose what to redo")),
      width));
  out.push_back(ansi::fitToWidth("", width));

  for (int i = 0; i < 3; ++i) {
    const bool selected = (i == modeCursor_);
    const bool available = modeAvailable(items[i].mode);
    const std::string marker = selected ? theme_ansi::accent("> ") : "  ";
    std::string label = items[i].label;
    if (!available) {
      label = theme_ansi::dim(label + "  (unavailable)");
    } else {
      label = theme_ansi::foreground(label);
    }
    std::string line = "  " + marker +
                       theme_ansi::dim(std::to_string(i + 1) + ". ") +
                       label;
    std::string fitted = ansi::fitToWidth(line, width);
    out.push_back(selected ? theme_ansi::selection(fitted) : fitted);
  }
  out.push_back(ansi::fitToWidth("", width));
  out.push_back(ansi::fitToWidth(
      "  " + theme_ansi::dim(
                  "⚠ Redo replays edits onto current files. "
                  "If you've made manual changes since the undo, redo may conflict."),
      width));
  return out;
}

std::vector<std::string> RedoOverlay::render(int width) const {
  std::vector<std::string> lines;
  if (!isOpen_) return lines;

  std::string title = " Redo ";
  if (phase_ == Phase::PickingMode) title += "— mode ";
  else if (phase_ == Phase::Executing) title += "— applying ";
  lines.push_back(ansi::fitToWidth(
      theme_ansi::accent(ansi::bold(title)), width));
  lines.push_back(theme_ansi::divider(width));

  if (phase_ == Phase::Executing) {
    lines.push_back(ansi::fitToWidth(
        theme_ansi::accent("  ◌ ") +
            theme_ansi::foreground("Applying redo..."),
        width));
    lines.push_back(ansi::fitToWidth("", width));
    lines.push_back(ansi::fitToWidth(
        theme_ansi::dim("  Esc to cancel after current step completes."),
        width));
    return lines;
  }

  if (phase_ == Phase::PickingMode) {
    auto pane = renderModePane(width);
    for (auto& line : pane) lines.push_back(std::move(line));

    if (!errorMessage_.empty()) {
      lines.push_back(ansi::fitToWidth("", width));
      lines.push_back(ansi::fitToWidth(
          "  " + theme_ansi::error("⚠ " + errorMessage_), width));
    }

    lines.push_back(ansi::fitToWidth("", width));
    lines.push_back(ansi::fitToWidth(
        theme_ansi::dim(" 1/2/3 select  · Enter confirm · Esc back "),
        width));
    return lines;
  }

  // PickingAction.
  if (actions_.empty()) {
    lines.push_back(ansi::fitToWidth(
        theme_ansi::dim("  No undo actions available to redo."), width));
    lines.push_back(ansi::fitToWidth("", width));
    lines.push_back(ansi::fitToWidth(
        theme_ansi::dim(" Esc to close "), width));
    return lines;
  }

  const int total = static_cast<int>(actions_.size());
  const int visibleCount = std::min(total, kMaxVisibleActions);
  for (int i = 0; i < visibleCount; ++i) {
    const int idx = scrollOffset_ + i;
    if (idx < 0 || idx >= total) break;
    lines.push_back(renderActionRow(actions_[idx], idx == cursorIdx_, width));
  }
  if (total > kMaxVisibleActions) {
    if (scrollOffset_ > 0) {
      lines.front() += theme_ansi::dim("  ↑ " + std::to_string(scrollOffset_) +
                                        " more");
    }
    const int below = total - (scrollOffset_ + visibleCount);
    if (below > 0 && !lines.empty()) {
      lines.back() += theme_ansi::dim("  ↓ " + std::to_string(below) + " more");
    }
  }

  if (!errorMessage_.empty()) {
    lines.push_back(ansi::fitToWidth("", width));
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::error("⚠ " + errorMessage_), width));
  }

  lines.push_back(ansi::fitToWidth("", width));
  lines.push_back(ansi::fitToWidth(
      theme_ansi::dim(" ↑↓ navigate · Enter redo this · Esc cancel "),
      width));
  return lines;
}

bool RedoOverlay::handleInput(const std::string& key) {
  if (!isOpen_) return false;

  if (key == "\x1b") {
    if (phase_ == Phase::PickingMode) {
      phase_ = Phase::PickingAction;
      errorMessage_.clear();
      return true;
    }
    if (phase_ == Phase::Executing) {
      return true;  // ignore mid-redo
    }
    if (onDismiss_) onDismiss_();
    return true;
  }

  if (phase_ == Phase::Executing) return true;

  if (phase_ == Phase::PickingMode) {
    using M = firmius::daemon::RedoMode;
    const M modes[] = {M::RestoreCodeAndConversation,
                       M::RestoreConversation,
                       M::RestoreCode};
    if (key == "\x1b[A" && modeCursor_ > 0) {
      --modeCursor_;
      return true;
    }
    if (key == "\x1b[B" && modeCursor_ < 2) {
      ++modeCursor_;
      return true;
    }
    if (key == "1") { modeCursor_ = 0; return true; }
    if (key == "2") { modeCursor_ = 1; return true; }
    if (key == "3") { modeCursor_ = 2; return true; }
    if (key == "\r" || key == "\n") {
      if (cursorIdx_ < 0 || cursorIdx_ >= static_cast<int>(actions_.size())) {
        return true;
      }
      const M chosen = modes[modeCursor_];
      if (!modeAvailable(chosen)) {
        errorMessage_ = "That mode is not available for this redo target";
        return true;
      }
      if (onExecute_) {
        onExecute_(actions_[cursorIdx_].undoActionId, chosen);
      }
      return true;
    }
    return true;
  }

  // PickingAction.
  if (actions_.empty()) return true;

  if (key == "\x1b[A") {
    if (cursorIdx_ > 0) {
      --cursorIdx_;
      if (cursorIdx_ < scrollOffset_) scrollOffset_ = cursorIdx_;
    }
    return true;
  }
  if (key == "\x1b[B") {
    if (cursorIdx_ + 1 < static_cast<int>(actions_.size())) {
      ++cursorIdx_;
      if (cursorIdx_ >= scrollOffset_ + kMaxVisibleActions) {
        scrollOffset_ = cursorIdx_ - kMaxVisibleActions + 1;
      }
    }
    return true;
  }
  if (key == "\r" || key == "\n") {
    if (cursorIdx_ < 0 || cursorIdx_ >= static_cast<int>(actions_.size())) {
      return true;
    }
    if (!actions_[cursorIdx_].redoAvailable) {
      errorMessage_ = "This undo action is no longer redoable";
      return true;
    }
    phase_ = Phase::PickingMode;
    modeCursor_ = 0;
    errorMessage_.clear();
    return true;
  }
  return true;
}

bool RedoOverlay::handleMouse(const MouseEvent&, int, int) {
  return true;
}

}  // namespace firmius::tui2
