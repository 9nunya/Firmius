#include "RewindOverlay.hpp"

#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>

namespace firmius::tui2 {

namespace {

// Concise relative-time formatter ("just now", "2m ago", "3h ago", "5d ago").
// Anything older than ~30 days falls back to "long ago" — production code
// would format an absolute date here, but the overlay only needs scannable
// hints for the user to pick the right turn.
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

// Truncate a string to a visible-width budget. Cheap byte-count truncation
// is fine here because we're only previewing user input — the overlay
// already trusted App to give us reasonable text.
std::string truncate(const std::string& text, int budget) {
  if (budget <= 0) return "";
  if (static_cast<int>(text.size()) <= budget) return text;
  if (budget <= 1) return text.substr(0, budget);
  return text.substr(0, budget - 1) + "…";
}

}  // namespace

void RewindOverlay::setEntries(std::vector<TurnEntry> entries) {
  entries_ = std::move(entries);
  cursorIdx_ = 0;
  scrollOffset_ = 0;
  preview_.reset();
  errorMessage_.clear();
  phase_ = Phase::PickingTurn;
  // Kick off the first preview fetch synchronously — App will populate
  // preview_ before the next frame.
  if (!entries_.empty() && onPreviewRequest_) {
    onPreviewRequest_(entries_[0].turnId);
  }
}

void RewindOverlay::setPreview(firmius::daemon::RewindPreviewResponse preview) {
  // Only accept the preview if it still matches the current cursor — this
  // is the late-landing-RPC guard.
  if (cursorIdx_ < 0 ||
      cursorIdx_ >= static_cast<int>(entries_.size())) {
    return;
  }
  if (preview.targetTurnId != entries_[cursorIdx_].turnId) {
    return;
  }
  preview_ = std::move(preview);
}

void RewindOverlay::setExecuting(bool executing) {
  if (executing) {
    phase_ = Phase::Executing;
    errorMessage_.clear();
  } else if (phase_ == Phase::Executing) {
    phase_ = Phase::PickingMode;  // back to picker on failure
  }
}

void RewindOverlay::showError(const std::string& message) {
  errorMessage_ = message;
}

void RewindOverlay::open() {
  isOpen_ = true;
  phase_ = Phase::PickingTurn;
  errorMessage_.clear();
}

void RewindOverlay::close() {
  isOpen_ = false;
  entries_.clear();
  cursorIdx_ = 0;
  scrollOffset_ = 0;
  modeCursor_ = 0;
  preview_.reset();
  errorMessage_.clear();
  phase_ = Phase::PickingTurn;
}

int RewindOverlay::height(int /*width*/) const {
  if (!isOpen_) return 0;
  // title + separator + (visible turns) + preview pane (~6 rows) + footer
  if (phase_ == Phase::PickingMode) return 12;
  if (phase_ == Phase::Executing)   return 6;
  const int rows = std::min<int>(entries_.size(), kMaxVisibleTurns);
  return 2 + rows + 6 + 2;  // title + sep + rows + preview + footer
}

std::string RewindOverlay::renderTurnRow(const TurnEntry& entry,
                                          bool selected,
                                          int width) const {
  const std::string marker = selected ? theme_ansi::accent("> ")
                                      : std::string("  ");
  const std::string time = formatRelative(entry.createdAtMs);
  const int budget = std::max(10, width - 4 - static_cast<int>(time.size()) - 2);
  std::string previewText = truncate(entry.preview, budget);
  std::string line = "  " + marker + theme_ansi::foreground(previewText);
  if (!time.empty()) {
    line += "  " + theme_ansi::dim(time);
  }
  std::string fitted = ansi::fitToWidth(line, width);
  return selected ? theme_ansi::selection(fitted) : fitted;
}

std::vector<std::string> RewindOverlay::renderPreviewPane(int width) const {
  std::vector<std::string> out;
  if (!preview_.has_value()) {
    out.push_back(ansi::fitToWidth(
        theme_ansi::dim("  Loading preview..."), width));
    return out;
  }
  const auto &p = *preview_;
  if (!p.errorMessage.empty()) {
    out.push_back(ansi::fitToWidth(
        "  " + theme_ansi::error("⚠ " + p.errorMessage), width));
    return out;
  }
  // Header line: "Will discard 3 turns. 2 files +25/-7."
  std::ostringstream header;
  header << "Will discard " << p.turnsToUndo
         << (p.turnsToUndo == 1 ? " turn" : " turns") << ".";
  if (!p.affectedEditBatches.empty()) {
    header << "  " << p.filesAffected.size()
           << (p.filesAffected.size() == 1 ? " file " : " files ")
           << theme_ansi::success("+" + std::to_string(p.totalAddedLines))
           << "/"
           << theme_ansi::error("-" + std::to_string(p.totalRemovedLines));
  } else {
    header << "  No code changes.";
  }
  out.push_back(ansi::fitToWidth("  " + header.str(), width));

  // First few affected files so the user has something to scan.
  const int filesToShow = std::min<int>(p.filesAffected.size(), 3);
  for (int i = 0; i < filesToShow; ++i) {
    out.push_back(ansi::fitToWidth(
        "    " + theme_ansi::dim("· " + p.filesAffected[i]), width));
  }
  if (static_cast<int>(p.filesAffected.size()) > filesToShow) {
    out.push_back(ansi::fitToWidth(
        "    " + theme_ansi::dim(
                     "· …and " +
                     std::to_string(p.filesAffected.size() - filesToShow) +
                     " more"),
        width));
  }
  if (!p.codeRestoreSafe) {
    out.push_back(ansi::fitToWidth(
        "  " + theme_ansi::warning("⚠ code restore blocked: " +
                                    p.codeRestoreBlockReason),
        width));
  }
  return out;
}

bool RewindOverlay::modeAvailable(firmius::daemon::RewindMode mode) const {
  using M = firmius::daemon::RewindMode;
  if (!preview_.has_value()) return false;
  // Restore code requires the preview to consider it safe and to actually
  // have batches to roll back.
  if (mode == M::RestoreCode || mode == M::RestoreCodeAndConversation) {
    if (!preview_->codeRestoreSafe) return false;
    if (preview_->affectedEditBatches.empty() && mode == M::RestoreCode) {
      return false;
    }
  }
  // Restore conversation requires there to be turns to discard. preview_
  // returns turnsToUndo > 0 for a valid target.
  if (mode == M::RestoreConversation || mode == M::RestoreCodeAndConversation) {
    if (preview_->turnsToUndo <= 0) return false;
  }
  return true;
}

std::vector<std::string> RewindOverlay::renderModePane(int width) const {
  std::vector<std::string> out;
  using M = firmius::daemon::RewindMode;
  struct Item { M mode; const char *label; };
  const Item items[] = {
      {M::RestoreCodeAndConversation, "Restore code and conversation"},
      {M::RestoreConversation,        "Restore conversation"},
      {M::RestoreCode,                "Restore code"},
  };
  out.push_back(ansi::fitToWidth(
      theme_ansi::accent(ansi::bold("  Choose what to restore")),
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
                  "⚠ Rewinding does not affect files edited manually or via bash."),
      width));
  return out;
}

std::vector<std::string> RewindOverlay::render(int width) const {
  std::vector<std::string> lines;
  if (!isOpen_) return lines;

  // Title bar.
  std::string title = " Rewind ";
  if (phase_ == Phase::PickingMode) title += "— mode ";
  else if (phase_ == Phase::Executing) title += "— applying ";
  lines.push_back(ansi::fitToWidth(
      theme_ansi::accent(ansi::bold(title)), width));
  lines.push_back(theme_ansi::divider(width));

  if (phase_ == Phase::Executing) {
    lines.push_back(ansi::fitToWidth(
        theme_ansi::accent("  ◌ ") +
            theme_ansi::foreground("Applying rewind..."),
        width));
    lines.push_back(ansi::fitToWidth("", width));
    lines.push_back(ansi::fitToWidth(
        theme_ansi::dim("  Esc to cancel after current file undo completes."),
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

  // Phase::PickingTurn.
  if (entries_.empty()) {
    lines.push_back(ansi::fitToWidth(
        theme_ansi::dim("  No user messages to rewind to."), width));
    lines.push_back(ansi::fitToWidth("", width));
    lines.push_back(ansi::fitToWidth(
        theme_ansi::dim(" Esc to close "), width));
    return lines;
  }

  const int total = static_cast<int>(entries_.size());
  const int visibleCount = std::min(total, kMaxVisibleTurns);
  for (int i = 0; i < visibleCount; ++i) {
    const int idx = scrollOffset_ + i;
    if (idx < 0 || idx >= total) break;
    lines.push_back(renderTurnRow(entries_[idx], idx == cursorIdx_, width));
  }
  // "↑ N more" hints when scrolled.
  if (total > kMaxVisibleTurns) {
    if (scrollOffset_ > 0) {
      lines.front() += theme_ansi::dim("  ↑ " + std::to_string(scrollOffset_) +
                                        " more");
    }
    const int below = total - (scrollOffset_ + visibleCount);
    if (below > 0 && !lines.empty()) {
      lines.back() += theme_ansi::dim("  ↓ " + std::to_string(below) + " more");
    }
  }

  lines.push_back(ansi::fitToWidth(theme_ansi::divider(width), width));
  auto preview = renderPreviewPane(width);
  for (auto& line : preview) lines.push_back(std::move(line));

  if (!errorMessage_.empty()) {
    lines.push_back(ansi::fitToWidth("", width));
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::error("⚠ " + errorMessage_), width));
  }

  lines.push_back(ansi::fitToWidth("", width));
  lines.push_back(ansi::fitToWidth(
      theme_ansi::dim(" ↑↓ navigate · Enter rewind here · Esc cancel "),
      width));

  return lines;
}

bool RewindOverlay::handleInput(const std::string& key) {
  if (!isOpen_) return false;

  // Always: ESC backs out one phase (or dismisses).
  if (key == "\x1b") {
    if (phase_ == Phase::PickingMode) {
      phase_ = Phase::PickingTurn;
      errorMessage_.clear();
      return true;
    }
    if (phase_ == Phase::Executing) {
      // Don't let users interrupt mid-undo; ignore Esc.
      return true;
    }
    if (onDismiss_) onDismiss_();
    return true;
  }

  if (phase_ == Phase::Executing) {
    return true;  // swallow all input
  }

  if (phase_ == Phase::PickingMode) {
    using M = firmius::daemon::RewindMode;
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
    // Number-key shortcut.
    if (key == "1") { modeCursor_ = 0; return true; }
    if (key == "2") { modeCursor_ = 1; return true; }
    if (key == "3") { modeCursor_ = 2; return true; }
    if (key == "\r" || key == "\n") {
      if (cursorIdx_ < 0 || cursorIdx_ >= static_cast<int>(entries_.size())) {
        return true;
      }
      const M chosen = modes[modeCursor_];
      if (!modeAvailable(chosen)) {
        errorMessage_ = "That mode is not available for this rewind point";
        return true;
      }
      if (onExecute_) {
        onExecute_(entries_[cursorIdx_].turnId, chosen);
      }
      return true;
    }
    return true;
  }

  // Phase::PickingTurn.
  if (entries_.empty()) return true;

  auto requestPreview = [&]() {
    // Reset BEFORE firing the callback. The callback may resolve
    // synchronously (daemon RPC happens inline in App::onRewindPreviewRequest),
    // and if we cleared after, we'd wipe the freshly-landed preview.
    preview_.reset();
    if (onPreviewRequest_ && cursorIdx_ >= 0 &&
        cursorIdx_ < static_cast<int>(entries_.size())) {
      onPreviewRequest_(entries_[cursorIdx_].turnId);
    }
  };

  if (key == "\x1b[A") {  // Up
    if (cursorIdx_ > 0) {
      --cursorIdx_;
      if (cursorIdx_ < scrollOffset_) scrollOffset_ = cursorIdx_;
      requestPreview();
    }
    return true;
  }
  if (key == "\x1b[B") {  // Down
    if (cursorIdx_ + 1 < static_cast<int>(entries_.size())) {
      ++cursorIdx_;
      if (cursorIdx_ >= scrollOffset_ + kMaxVisibleTurns) {
        scrollOffset_ = cursorIdx_ - kMaxVisibleTurns + 1;
      }
      requestPreview();
    }
    return true;
  }
  if (key == "\r" || key == "\n") {
    phase_ = Phase::PickingMode;
    modeCursor_ = 0;
    errorMessage_.clear();
    return true;
  }
  return true;
}

bool RewindOverlay::handleMouse(const MouseEvent&, int, int) {
  // No mouse interactions — would conflict with transcript drag-select.
  return true;
}

}  // namespace firmius::tui2
