#pragma once

#include "AppState.hpp"
#include "Overlay.hpp"

#include <functional>
#include <string>
#include <vector>

namespace firmius::tui {

/// Two-phase permission prompt:
///   Phase 1 — three big actions: Allow once / Allow always / Deny
///   Phase 2 — when "Allow always" is picked, show the suggestion
///             multi-picker so the user can craft a tailored rule set.
///
/// All UI is presentational. App owns the daemon round-trips:
///   - onAllowOnce(requestId)
///   - onDeny(requestId)
///   - onAllowAlways(requestId, vector<ruleId>)  — selected suggestions
///   - onDismiss()
///
/// The overlay reads the pending permission from AppState directly via a
/// snapshot passed at open() time. Dispatch refreshes the snapshot when
/// new permissions queue up.
class PermissionPromptOverlay : public Overlay {
public:
  using AllowOnceCallback = std::function<void(const std::string& requestId)>;
  using DenyCallback = std::function<void(const std::string& requestId)>;
  using AllowAlwaysCallback = std::function<void(
      const std::string& requestId,
      const std::vector<std::string>& selectedSuggestionIds)>;
  using DismissCallback = std::function<void()>;

  PermissionPromptOverlay() = default;

  /// Set the active permission + queue context. `queueIndex` is 1-based,
  /// `queueSize` is the total batch size, and `nextHint` is a one-line
  /// preview of the next pending request (empty if none).
  void setPermission(PendingPermission perm,
                     int queueIndex = 1, int queueSize = 1,
                     std::string nextHint = "");

  void setOnAllowOnce(AllowOnceCallback cb) { onAllowOnce_ = std::move(cb); }
  void setOnDeny(DenyCallback cb) { onDeny_ = std::move(cb); }
  void setOnAllowAlways(AllowAlwaysCallback cb) {
    onAllowAlways_ = std::move(cb);
  }
  void setOnDismiss(DismissCallback cb) { onDismiss_ = std::move(cb); }

  void open() override;
  void close() override;
  bool isActive() const override { return isOpen_; }

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;
  bool handleInput(const std::string& key) override;
  bool handleMouse(const MouseEvent&, int, int) override { return isOpen_; }

private:
  enum class Phase {
    Action,      ///< Allow once / Allow always / Deny
    Suggestions, ///< Multi-pick the suggestion bundle
  };

  std::vector<std::string> renderHeader(int width) const;
  std::vector<std::string> renderActionPhase(int width) const;
  std::vector<std::string> renderSuggestionPhase(int width) const;
  std::string formatRequestSummary() const;
  std::vector<std::string> formatRequestDetail(int width) const;
  std::string severityBadge() const;

  bool isOpen_ = false;
  Phase phase_ = Phase::Action;
  PendingPermission perm_;
  int actionCursor_ = 0;     // 0=Allow once, 1=Allow always, 2=Deny
  int suggestionCursor_ = 0;
  std::vector<bool> suggestionSelected_;
  int queueIndex_ = 1;       ///< 1-based position in the batch.
  int queueSize_ = 1;        ///< Total pending count when this opened.
  std::string nextHint_;     ///< Preview of the next request (if any).

  AllowOnceCallback onAllowOnce_;
  DenyCallback onDeny_;
  AllowAlwaysCallback onAllowAlways_;
  DismissCallback onDismiss_;
};

} // namespace firmius::tui
