#pragma once

#include "Overlay.hpp"
#include "daemon/Protocol.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace firmius::tui2 {

/// Claude-Code-style /redo overlay.
///
/// Forward of /undo. After the user has rewound once, /redo lets them
/// step back forward by picking a previously-persisted TranscriptUndoAction
/// and choosing what to restore.
///
/// Two sub-views:
///   1. UndoAction picker (default).
///        - Newest-first list of recent undo actions, each a single row:
///          first restored turn preview + "Nt / Mb" badge + relative time.
///        - Greyed if redoAvailable is false (already redone, or daemon
///          marked it stale).
///   2. Mode picker (after Enter on a row).
///        - 3 options identical to RewindOverlay:
///            (1) Redo conversation and code
///            (2) Redo conversation
///            (3) Redo code
///        - "Redo code" greyed when editBatchesToRedo == 0.
///        - "Redo conversation" greyed when turnsToRedo == 0.
///
/// As with RewindOverlay, this widget is purely presentational. App owns
/// the daemon round-trips and feeds state in via setActions / setExecuting.
class RedoOverlay : public Overlay {
public:
  using ExecuteCallback =
      std::function<void(const std::string& undoActionId,
                         firmius::daemon::RedoMode mode)>;
  using DismissCallback = std::function<void()>;

  RedoOverlay() = default;

  /// Replace the row list. Called once when /redo opens.
  void setActions(std::vector<firmius::daemon::RedoUndoActionSummary> actions);

  void setOnExecute(ExecuteCallback cb) { onExecute_ = std::move(cb); }
  void setOnDismiss(DismissCallback cb) { onDismiss_ = std::move(cb); }

  /// Disables further input until daemon reports the redo applied.
  void setExecuting(bool executing);

  /// Bottom-bar inline error.
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
    PickingAction,
    PickingMode,
    Executing,
  };

  std::string renderActionRow(
      const firmius::daemon::RedoUndoActionSummary& action,
      bool selected, int width) const;
  std::vector<std::string> renderModePane(int width) const;

  bool modeAvailable(firmius::daemon::RedoMode mode) const;

  Phase phase_ = Phase::PickingAction;
  std::vector<firmius::daemon::RedoUndoActionSummary> actions_;
  int cursorIdx_ = 0;
  int scrollOffset_ = 0;
  int modeCursor_ = 0;
  std::string errorMessage_;
  bool isOpen_ = false;

  static constexpr int kMaxVisibleActions = 8;

  ExecuteCallback onExecute_;
  DismissCallback onDismiss_;
};

} // namespace firmius::tui2
