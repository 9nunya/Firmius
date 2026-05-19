#ifndef FIRMIUS_TUI_REWINDOVERLAY_HPP
#define FIRMIUS_TUI_REWINDOVERLAY_HPP

#include "Overlay.hpp"
#include "daemon/Protocol.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace firmius::tui {

/// Claude-Code-style /undo rewind overlay.
///
/// Two sub-views joined by ↑/↓ + Enter:
///
///   1. Turn picker (default).
///        - List of user-message turns, newest at the top.
///        - Each row shows: 1-line preview, relative time, optional
///          "+A/-B across N files since" badge if edits exist.
///        - As selection moves, App fetches a fresh RewindPreviewResponse
///          and pushes it via setPreview() so the bottom pane updates.
///   2. Mode picker (after Enter on a row).
///        - 3 options:
///            (1) Restore code and conversation
///            (2) Restore conversation
///            (3) Restore code
///        - Greyed/disabled rows (e.g. "Restore code" when codeRestoreSafe
///          is false) cannot be activated.
///        - Enter executes; Esc returns to the turn picker.
///
/// The overlay is "dumb" — App owns the daemon round-trips. Overlay just
/// displays state and dispatches the chosen action via callbacks.
class RewindOverlay : public Overlay {
public:
  /// One row in the turn picker. App constructs this from the agent
  /// transcript before opening the overlay.
  struct TurnEntry {
    std::string turnId;
    /// Free-form preview the user actually sees. Already truncated by App.
    std::string preview;
    /// Millis-since-epoch of the message — overlay formats relative time.
    std::uint64_t createdAtMs = 0;
  };

  using PreviewRequestCallback =
      std::function<void(const std::string& turnId)>;
  using ExecuteCallback =
      std::function<void(const std::string& turnId,
                         firmius::daemon::RewindMode mode)>;
  using DismissCallback = std::function<void()>;

  RewindOverlay() = default;

  // ── Setup ────────────────────────────────────────────────────────────
  /// Replace the row list. Called once when /undo opens.
  void setEntries(std::vector<TurnEntry> entries);

  /// Called whenever selection changes — App should fire previewRewind
  /// and feed the result back via setPreview().
  void setOnPreviewRequest(PreviewRequestCallback cb) {
    onPreviewRequest_ = std::move(cb);
  }

  /// Called when the user confirms a mode in the mode picker.
  void setOnExecute(ExecuteCallback cb) { onExecute_ = std::move(cb); }
  void setOnDismiss(DismissCallback cb) { onDismiss_ = std::move(cb); }

  /// Push a freshly-fetched preview into the overlay. The overlay only
  /// renders it if the preview's targetTurnId still matches the user's
  /// current selection (avoids races where slow previews land late).
  void setPreview(firmius::daemon::RewindPreviewResponse preview);

  /// Mark the overlay as "executing" — disables further input until the
  /// daemon reports a RewindApplied event (then App calls close()).
  void setExecuting(bool executing);

  /// Inline error bar at the bottom.
  void showError(const std::string& message);

  // ── Overlay interface ────────────────────────────────────────────────
  void open() override;
  void close() override;
  bool isActive() const override { return isOpen_; }

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;

  bool handleInput(const std::string& key) override;
  bool handleMouse(const MouseEvent& event,
                   int screenRow,
                   int screenCol) override;

private:
  enum class Phase {
    PickingTurn,   ///< Top-level list, navigating turns.
    PickingMode,   ///< User confirmed a turn, picking restore mode.
    Executing,     ///< Awaiting RewindApplied from daemon.
  };

  /// Render a single turn-picker row.
  std::string renderTurnRow(const TurnEntry& entry, bool selected,
                             int width) const;

  /// Render the bottom preview pane.
  std::vector<std::string> renderPreviewPane(int width) const;

  /// Render the mode-picker view.
  std::vector<std::string> renderModePane(int width) const;

  /// Whether a given mode is selectable given the current preview.
  bool modeAvailable(firmius::daemon::RewindMode mode) const;

  Phase phase_ = Phase::PickingTurn;
  std::vector<TurnEntry> entries_;
  int cursorIdx_ = 0;
  int scrollOffset_ = 0;
  int modeCursor_ = 0;  ///< 0..2 in PickingMode.
  std::optional<firmius::daemon::RewindPreviewResponse> preview_;
  std::string errorMessage_;
  bool isOpen_ = false;

  static constexpr int kMaxVisibleTurns = 8;

  PreviewRequestCallback onPreviewRequest_;
  ExecuteCallback onExecute_;
  DismissCallback onDismiss_;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_REWINDOVERLAY_HPP
