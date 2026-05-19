#include "ConnectOverlay.hpp"

#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace firmius::tui {

namespace {

// Best-effort cross-platform "open URL in default browser" helper. v1's
// modal also uses xdg-open / open / start; we mirror that here so OAuth
// flows can pop the browser the moment the user submits the prompt.
bool openUrlPlatform(const std::string& url) {
  if (url.empty()) return false;
#if defined(_WIN32)
  std::string cmd = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
  std::string cmd = "open \"" + url + "\" >/dev/null 2>&1 &";
#else
  std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
#endif
  return std::system(cmd.c_str()) == 0;
}

std::string maskSecret(const std::string& text) {
  return std::string(text.size(), '*');
}

} // namespace

void ConnectOverlay::loadPrompt(firmius::daemon::WizardPromptSnapshot prompt) {
  prompt_ = std::move(prompt);
  inputBuffer_.clear();
  choiceCursor_ = 0;
  errorMessage_.clear();
  mode_ = Mode::Prompting;
}

void ConnectOverlay::markReadyToFinalize() {
  prompt_.reset();
  inputBuffer_.clear();
  errorMessage_.clear();
  mode_ = Mode::Finalizing;
  progressMessage_ = "Finalizing connection...";
}

void ConnectOverlay::updateProgress(
    firmius::daemon::ConnectProgressSnapshot snapshot) {
  using P = firmius::daemon::ConnectProgressPhase;
  switch (snapshot.phase) {
  case P::Polling:
    mode_ = Mode::Finalizing;
    progressMessage_ = snapshot.message.empty()
                           ? "Waiting for the provider..."
                           : snapshot.message;
    break;
  case P::Finalizing:
    mode_ = Mode::Finalizing;
    progressMessage_ = snapshot.message.empty() ? "Finalizing connection..."
                                                : snapshot.message;
    break;
  case P::Succeeded:
    mode_ = Mode::Done;
    progressMessage_ = snapshot.message.empty() ? "Connected." : snapshot.message;
    break;
  case P::Failed:
    mode_ = Mode::Failed;
    progressMessage_ = snapshot.message.empty() ? "Connection failed."
                                                : snapshot.message;
    break;
  case P::Cancelled:
    mode_ = Mode::Failed;
    progressMessage_ =
        snapshot.message.empty() ? "Cancelled." : snapshot.message;
    break;
  }
}

void ConnectOverlay::showError(const std::string& message) {
  errorMessage_ = message;
}

void ConnectOverlay::open() {
  isOpen_ = true;
  mode_ = Mode::Prompting;
  inputBuffer_.clear();
  choiceCursor_ = 0;
  errorMessage_.clear();
  progressMessage_.clear();
}

void ConnectOverlay::close() {
  isOpen_ = false;
  prompt_.reset();
  inputBuffer_.clear();
  choiceCursor_ = 0;
  errorMessage_.clear();
  progressMessage_.clear();
  sessionId_.clear();
}

std::vector<std::string> ConnectOverlay::wrapMessage(const std::string& text,
                                                      int width) const {
  std::vector<std::string> lines;
  if (width <= 0) return lines;
  std::istringstream stream(text);
  std::string raw;
  while (std::getline(stream, raw)) {
    if (raw.empty()) {
      lines.push_back("");
      continue;
    }
    // Soft-wrap on spaces.
    std::string current;
    std::istringstream words(raw);
    std::string word;
    while (words >> word) {
      if (current.empty()) {
        current = word;
      } else if (static_cast<int>(current.size() + 1 + word.size()) <= width) {
        current += " " + word;
      } else {
        lines.push_back(current);
        current = word;
      }
    }
    if (!current.empty()) lines.push_back(current);
  }
  return lines;
}

int ConnectOverlay::height(int width) const {
  // Approximate: title + separator + body (varies) + actions.
  // We always return a generous range; the App reserves it.
  if (!isOpen_) return 0;
  int h = 4;  // title + separator + actions + spacing
  if (mode_ == Mode::Done || mode_ == Mode::Failed) {
    h += static_cast<int>(wrapMessage(progressMessage_, std::max(20, width - 2)).size()) + 2;
    return std::min(20, h);
  }
  if (mode_ == Mode::Finalizing) {
    h += 3;
    return h;
  }
  if (prompt_.has_value()) {
    h += static_cast<int>(wrapMessage(prompt_->message, std::max(20, width - 2)).size());
    if (!prompt_->detectedUrl.empty()) h += 2;
    if (!prompt_->choices.empty()) {
      h += static_cast<int>(prompt_->choices.size()) + 1;
    } else if (prompt_->allowFreeformInput) {
      h += 2;
    }
    if (!errorMessage_.empty()) h += 2;
  }
  return std::min(22, h);
}

std::vector<std::string> ConnectOverlay::render(int width) const {
  std::vector<std::string> lines;
  if (!isOpen_) return lines;

  // ── Title ───────────────────────────────────────────────────────────────
  std::string titleText = " Connect: " + providerId_;
  if (!providerKind_.empty()) {
    titleText += "  (" + providerKind_ + ")";
  }
  lines.push_back(theme_ansi::accent(ansi::bold(ansi::fitToWidth(titleText, width))));
  lines.push_back(theme_ansi::divider(width));

  const int innerWidth = std::max(20, width - 2);

  // ── Body ────────────────────────────────────────────────────────────────
  switch (mode_) {
  case Mode::Done: {
    auto wrapped = wrapMessage(progressMessage_, innerWidth);
    lines.push_back(ansi::fitToWidth(
        theme_ansi::success(ansi::bold("  ✓ Connected")), width));
    lines.push_back(ansi::fitToWidth("", width));
    for (const auto& l : wrapped) {
      lines.push_back(ansi::fitToWidth("  " + theme_ansi::foreground(l), width));
    }
    break;
  }
  case Mode::Failed: {
    auto wrapped = wrapMessage(progressMessage_, innerWidth);
    lines.push_back(ansi::fitToWidth(
        theme_ansi::error(ansi::bold("  ✗ Failed")), width));
    lines.push_back(ansi::fitToWidth("", width));
    for (const auto& l : wrapped) {
      lines.push_back(ansi::fitToWidth("  " + theme_ansi::foreground(l), width));
    }
    break;
  }
  case Mode::Finalizing: {
    lines.push_back(ansi::fitToWidth(
        theme_ansi::accent("  ◌ ") + theme_ansi::foreground(progressMessage_),
        width));
    lines.push_back(ansi::fitToWidth("", width));
    lines.push_back(ansi::fitToWidth(
        theme_ansi::dim("  This can take a moment for OAuth flows."), width));
    break;
  }
  case Mode::AwaitingFinalize:
  case Mode::Prompting: {
    if (!prompt_.has_value()) {
      lines.push_back(ansi::fitToWidth(
          theme_ansi::dim("  Preparing wizard..."), width));
      break;
    }
    const auto& p = *prompt_;
    auto wrapped = wrapMessage(p.message, innerWidth);
    for (const auto& l : wrapped) {
      lines.push_back(ansi::fitToWidth("  " + theme_ansi::foreground(l), width));
    }

    if (!p.detectedUrl.empty()) {
      lines.push_back(ansi::fitToWidth("", width));
      lines.push_back(ansi::fitToWidth(
          "  " + theme_ansi::dim("URL: ") +
              theme_ansi::accent(p.detectedUrl),
          width));
    }

    if (p.isWaiting) {
      // Synthetic "Waiting..." prompt — no user input, just a hint.
      lines.push_back(ansi::fitToWidth("", width));
      lines.push_back(ansi::fitToWidth(
          theme_ansi::dim("  (press Enter to continue, Esc to cancel)"),
          width));
    } else if (!p.choices.empty()) {
      lines.push_back(ansi::fitToWidth("", width));
      for (size_t i = 0; i < p.choices.size(); ++i) {
        const bool focused = static_cast<int>(i) == choiceCursor_;
        std::string marker = focused ? theme_ansi::accent("> ") : "  ";
        std::string body = "  " + marker + p.choices[i].label;
        if (focused) {
          lines.push_back(theme_ansi::selection(ansi::fitToWidth(body, width)));
        } else {
          lines.push_back(ansi::fitToWidth(theme_ansi::foreground(body), width));
        }
      }
    } else if (p.allowFreeformInput) {
      lines.push_back(ansi::fitToWidth("", width));
      std::string field;
      if (inputBuffer_.empty()) {
        field = "  ❯ " + theme_ansi::dim(p.placeholder.empty()
                                              ? "type your answer..."
                                              : p.placeholder);
      } else {
        std::string display = p.isSecret ? maskSecret(inputBuffer_) : inputBuffer_;
        field = "  ❯ " + theme_ansi::foreground(display);
      }
      lines.push_back(ansi::fitToWidth(field, width));
    } else {
      lines.push_back(ansi::fitToWidth("", width));
      lines.push_back(ansi::fitToWidth(
          theme_ansi::dim("  (press Enter to continue, Esc to cancel)"),
          width));
    }

    if (!errorMessage_.empty()) {
      lines.push_back(ansi::fitToWidth("", width));
      lines.push_back(ansi::fitToWidth(
          "  " + theme_ansi::error("⚠ " + errorMessage_), width));
    }
    break;
  }
  }

  // ── Actions footer ─────────────────────────────────────────────────────
  lines.push_back(ansi::fitToWidth("", width));

  std::string hints;
  switch (mode_) {
  case Mode::Done:
  case Mode::Failed:
    hints = theme_ansi::dim(" Enter close ") + theme_ansi::dim(" │ ") +
            theme_ansi::dim("Esc close");
    break;
  case Mode::Finalizing:
    hints = theme_ansi::dim(" Esc cancel ");
    break;
  case Mode::AwaitingFinalize:
  case Mode::Prompting: {
    if (prompt_.has_value() && !prompt_->choices.empty()) {
      hints = theme_ansi::dim(" ↑↓ select ") + theme_ansi::dim(" │ ") +
              theme_ansi::dim("Enter ") +
              (prompt_->submitLabel.empty() ? "submit"
                                            : prompt_->submitLabel) +
              theme_ansi::dim(" │ ") + theme_ansi::dim("Esc cancel");
    } else if (prompt_.has_value()) {
      const std::string label =
          prompt_->submitLabel.empty()
              ? (prompt_->detectedUrl.empty() ? std::string("submit")
                                              : std::string("open url / continue"))
              : prompt_->submitLabel;
      hints = theme_ansi::dim(" Enter ") + label +
              theme_ansi::dim(" │ ") + theme_ansi::dim("Esc cancel");
    } else {
      hints = theme_ansi::dim(" Esc cancel ");
    }
    break;
  }
  }
  lines.push_back(ansi::fitToWidth(hints, width));

  return lines;
}

bool ConnectOverlay::handleInput(const std::string& key) {
  if (!isOpen_) return false;

  // ESC always cancels (or dismisses if we're already in a terminal state).
  if (key == "\x1b") {
    if (mode_ == Mode::Done || mode_ == Mode::Failed) {
      if (onDismiss_) onDismiss_();
    } else {
      if (onCancel_) onCancel_();
    }
    return true;
  }

  // Done / Failed: Enter closes.
  if (mode_ == Mode::Done || mode_ == Mode::Failed) {
    if (key == "\r" || key == "\n") {
      if (onDismiss_) onDismiss_();
      return true;
    }
    return true;  // swallow other input
  }

  // While finalizing, the only useful key is Esc (handled above).
  if (mode_ == Mode::Finalizing) {
    return true;  // swallow
  }

  // Prompting mode: drive the wizard.
  if (!prompt_.has_value()) {
    return true;  // swallow until next prompt arrives
  }
  auto& p = *prompt_;

  // Choices: arrow keys + enter.
  if (!p.choices.empty()) {
    if (key == "\x1b[A") {  // Up
      if (choiceCursor_ > 0) --choiceCursor_;
      return true;
    }
    if (key == "\x1b[B") {  // Down
      if (choiceCursor_ + 1 < static_cast<int>(p.choices.size())) {
        ++choiceCursor_;
      }
      return true;
    }
    if (key == "\r" || key == "\n") {
      if (choiceCursor_ >= 0 &&
          choiceCursor_ < static_cast<int>(p.choices.size())) {
        const auto& chosen = p.choices[choiceCursor_].value;
        // Best effort: if message has a URL, open it (parity with v1).
        if (!p.detectedUrl.empty() && !p.allowFreeformInput) {
          openUrlPlatform(p.detectedUrl);
        }
        if (onSubmit_) onSubmit_(chosen);
      }
      return true;
    }
    return true;  // swallow other keys when in choice mode
  }

  // Confirmation-only / waiting prompt: Enter advances.
  if (!p.allowFreeformInput) {
    if (key == "\r" || key == "\n") {
      if (!p.detectedUrl.empty()) {
        openUrlPlatform(p.detectedUrl);
      }
      if (onSubmit_) onSubmit_("");
      return true;
    }
    return true;
  }

  // Freeform text input.
  if (key == "\r" || key == "\n") {
    if (inputBuffer_.empty() && !p.allowEmptyInput) {
      // Reject empty submit; user can keep typing.
      return true;
    }
    if (onSubmit_) onSubmit_(inputBuffer_);
    return true;
  }
  if (key == "\x7f" || key == "\b") {
    if (!inputBuffer_.empty()) inputBuffer_.pop_back();
    errorMessage_.clear();
    return true;
  }
  // Printable bytes: append. Multi-byte UTF-8 sequences arrive as one key
  // string; appending them as-is preserves them in the buffer.
  if (!key.empty() && key[0] != '\x1b') {
    bool printable = true;
    for (unsigned char ch : key) {
      if (ch < 32 && ch != '\t') { printable = false; break; }
    }
    if (printable) {
      inputBuffer_ += key;
      errorMessage_.clear();
    }
    return true;
  }

  return true;  // swallow everything else while overlay is active
}

bool ConnectOverlay::handleMouse(const MouseEvent&, int, int) {
  // No mouse interactions for the wizard. Click-through is undesirable
  // because the overlay covers the input.
  return true;
}

} // namespace firmius::tui
