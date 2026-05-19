#pragma once

#include "Overlay.hpp"
#include "daemon/Protocol.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace firmius::tui {

/// Overlay that renders the daemon's /connect wizard.
///
/// The wizard is server-side: this overlay only renders the current
/// WizardPromptSnapshot (text input or radio choices) and forwards user
/// answers back to the daemon. Long-running phases (OAuth browser flows,
/// finalize) are surfaced as "waiting" prompts and "polling/finalizing"
/// progress lines respectively.
///
/// Lifecycle, driven by App:
///   1. App calls beginConnect on the daemon, then loadPrompt(...) here.
///   2. User types/selects an answer, presses Enter.
///        - Overlay calls submitCallback_ with the raw answer.
///        - App calls submitConnect on the daemon, then either loadPrompt
///          (next step) or markReadyToFinalize() (no more prompts).
///   3. When ready to finalize, App calls finalizeConnect; ConnectProgress
///      events arrive via EventRouter and end up as updateProgress() calls
///      here, which switch the overlay into "Finalizing/Succeeded/Failed"
///      view modes.
///   4. ESC at any point → cancelCallback_ → App calls cancelConnect.
///   5. Enter on the success/failure screen → dismissCallback_.
class ConnectOverlay : public Overlay {
public:
  using SubmitCallback = std::function<void(const std::string& answer)>;
  using CancelCallback = std::function<void()>;
  using DismissCallback = std::function<void()>;

  ConnectOverlay() = default;

  // ── Lifecycle wiring (called once after construction) ──
  void setProviderId(const std::string& providerId) { providerId_ = providerId; }
  void setProviderKind(const std::string& kind) { providerKind_ = kind; }
  void setSessionId(const std::string& sessionId) { sessionId_ = sessionId; }
  void setOnSubmit(SubmitCallback cb) { onSubmit_ = std::move(cb); }
  void setOnCancel(CancelCallback cb) { onCancel_ = std::move(cb); }
  void setOnDismiss(DismissCallback cb) { onDismiss_ = std::move(cb); }
  void setOnFinalize(std::function<void()> cb) { onFinalize_ = std::move(cb); }

  const std::string& sessionId() const { return sessionId_; }

  // ── Daemon-driven state updates ──
  /// Display the next prompt from the wizard.
  void loadPrompt(firmius::daemon::WizardPromptSnapshot prompt);

  /// Wizard is out of prompts. Show "press Enter to finalize" if not auto.
  /// If `autoFinalize` is true, App will call finalizeConnect immediately.
  void markReadyToFinalize();

  /// Update the overlay with a ConnectProgress event from the daemon.
  void updateProgress(firmius::daemon::ConnectProgressSnapshot snapshot);

  /// Show an inline error (e.g. submit RPC returned errorMessage).
  void showError(const std::string& message);

  // ── Overlay interface ──
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
  enum class Mode {
    /// Showing a wizard prompt — user is filling in an answer.
    Prompting,
    /// Wizard has no more prompts; awaiting Enter to fire finalize. (Rare
    /// path — usually finalize is auto-fired.)
    AwaitingFinalize,
    /// finalizeConnect kicked off; ConnectProgress events update the message.
    Finalizing,
    /// ConnectProgress: phase=Succeeded.
    Done,
    /// ConnectProgress: phase=Failed (or inline error).
    Failed,
  };

  std::vector<std::string> wrapMessage(const std::string& text, int width) const;

  std::string providerId_;
  std::string providerKind_;
  std::string sessionId_;

  Mode mode_ = Mode::Prompting;
  std::optional<firmius::daemon::WizardPromptSnapshot> prompt_;
  std::string inputBuffer_;
  int choiceCursor_ = 0;
  std::string progressMessage_;  ///< "Polling..." / "Finalizing..." / final outcome
  std::string errorMessage_;     ///< inline error (from submit response)

  bool isOpen_ = false;

  SubmitCallback onSubmit_;
  CancelCallback onCancel_;
  DismissCallback onDismiss_;
  std::function<void()> onFinalize_;
};

} // namespace firmius::tui
