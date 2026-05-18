#pragma once

#include "Overlay.hpp"
#include "daemon/Protocol.hpp"

#include <functional>
#include <string>
#include <vector>

namespace firmius::tui2 {

/// `/permissions` overlay — single stop-shop for the new permissions
/// system. Two tabs:
///   - **Modes** (default): list of permission modes (ask, yolo, custom).
///     ↑↓ navigate, Enter activates, A adds, R renames, D deletes (y/N
///     confirm). Built-in modes can't be renamed/deleted.
///   - **Rules**: list of policy rules. ↑↓ navigate, D deletes (y/N
///     confirm), Tab swaps tabs. Rules are color-coded by decision and
///     show their match keys + scope.
///
/// Both panes share Esc=close and R=reload-from-disk.
class PermissionsOverlay : public Overlay {
public:
  using SetActiveCallback = std::function<bool(const std::string& modeId)>;
  using CreateModeCallback = std::function<std::string(const std::string& name,
                                                        bool seedFromActive)>;
  using RenameModeCallback = std::function<bool(const std::string& modeId,
                                                  const std::string& newName)>;
  using DeleteModeCallback = std::function<bool(const std::string& modeId)>;
  using DeleteRuleCallback = std::function<bool(const std::string& ruleId)>;
  using ReloadCallback = std::function<bool()>;
  using DismissCallback = std::function<void()>;

  PermissionsOverlay() = default;

  /// Seed rule + mode listings. Called from App on open + after every
  /// successful mutation.
  void seedRules(firmius::daemon::PermissionListRulesResponse snapshot);
  void seedModes(std::vector<firmius::daemon::PermissionModeWire> modes,
                 std::string activeModeId);

  void setOnSetActiveMode(SetActiveCallback cb) {
    onSetActive_ = std::move(cb);
  }
  void setOnCreateMode(CreateModeCallback cb) { onCreate_ = std::move(cb); }
  void setOnRenameMode(RenameModeCallback cb) { onRename_ = std::move(cb); }
  void setOnDeleteMode(DeleteModeCallback cb) { onDeleteMode_ = std::move(cb); }
  void setOnDeleteRule(DeleteRuleCallback cb) { onDeleteRule_ = std::move(cb); }
  void setOnReload(ReloadCallback cb) { onReload_ = std::move(cb); }
  void setOnDismiss(DismissCallback cb) { onDismiss_ = std::move(cb); }

  void open() override;
  void close() override;
  bool isActive() const override { return isOpen_; }

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;
  bool handleInput(const std::string& key) override;
  bool handleMouse(const MouseEvent&, int, int) override { return isOpen_; }

private:
  enum class Tab { Modes, Rules };
  enum class Phase {
    Browse,
    PromptCreateName,
    PromptRenameName,
    ConfirmDeleteMode,
    ConfirmDeleteRule,
  };

  std::vector<std::string> renderTabBar(int width) const;
  std::vector<std::string> renderModes(int width) const;
  std::vector<std::string> renderRules(int width) const;
  std::string renderRuleRow(const firmius::daemon::PolicyRuleWire& rule,
                             bool selected, int width) const;
  std::string formatMatch(const firmius::daemon::PolicyRuleWire& rule) const;

  bool isOpen_ = false;
  Tab tab_ = Tab::Modes;
  Phase phase_ = Phase::Browse;

  // Mode state
  std::vector<firmius::daemon::PermissionModeWire> modes_;
  std::string activeModeId_;
  int modeCursor_ = 0;

  // Rule state
  firmius::daemon::PermissionListRulesResponse rules_;
  int ruleCursor_ = 0;
  int ruleScrollOffset_ = 0;

  // Text input buffer for create/rename phases.
  std::string inputBuf_;

  std::string message_;

  SetActiveCallback onSetActive_;
  CreateModeCallback onCreate_;
  RenameModeCallback onRename_;
  DeleteModeCallback onDeleteMode_;
  DeleteRuleCallback onDeleteRule_;
  ReloadCallback onReload_;
  DismissCallback onDismiss_;

  static constexpr int kMaxVisible = 10;
};

} // namespace firmius::tui2
