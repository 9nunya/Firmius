#include "NotificationManager.hpp"

#include "components/GlintEffect.hpp"

#include <algorithm>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <sstream>

namespace firmius::tui {

NotificationManager &NotificationManager::instance() {
  static NotificationManager inst;
  return inst;
}

std::string NotificationManager::notifyInfo(const std::string &title,
                                           const std::string &message,
                                           std::chrono::milliseconds duration) {
  Notification notif;
  notif.id = "notif_" + std::to_string(++notificationCounter_);
  notif.title = title;
  notif.message = message;
  notif.type = NotificationType::Info;
  notif.dismiss = NotificationDismiss::Auto;
  notif.autoDismissAfter = duration;
  notif.createdAt = std::chrono::steady_clock::now();
  notifications_.push_back(notif);
  return notif.id;
}

std::string NotificationManager::notifySuccess(
    const std::string &title, const std::string &message,
    std::chrono::milliseconds duration) {
  Notification notif;
  notif.id = "notif_" + std::to_string(++notificationCounter_);
  notif.title = title;
  notif.message = message;
  notif.type = NotificationType::Success;
  notif.dismiss = NotificationDismiss::Auto;
  notif.autoDismissAfter = duration;
  notif.createdAt = std::chrono::steady_clock::now();
  notifications_.push_back(notif);
  return notif.id;
}

std::string NotificationManager::notifyWarning(
    const std::string &title, const std::string &message,
    std::chrono::milliseconds duration) {
  Notification notif;
  notif.id = "notif_" + std::to_string(++notificationCounter_);
  notif.title = title;
  notif.message = message;
  notif.type = NotificationType::Warning;
  notif.dismiss = NotificationDismiss::Auto;
  notif.autoDismissAfter = duration;
  notif.createdAt = std::chrono::steady_clock::now();
  notifications_.push_back(notif);
  return notif.id;
}

std::string NotificationManager::notifyError(const std::string &title,
                                            const std::string &message,
                                            bool persistent) {
  Notification notif;
  notif.id = "notif_" + std::to_string(++notificationCounter_);
  notif.title = title;
  notif.message = message;
  notif.type = NotificationType::Error;
  notif.dismiss =
      persistent ? NotificationDismiss::Manual : NotificationDismiss::Auto;
  notif.autoDismissAfter = persistent ? std::chrono::milliseconds(0)
                                      : std::chrono::milliseconds(8000);
  notif.createdAt = std::chrono::steady_clock::now();
  notifications_.push_back(notif);
  return notif.id;
}

std::string NotificationManager::notifyProgress(const std::string &title,
                                               const std::string &message,
                                               float progress,
                                               bool persistent) {
  Notification notif;
  notif.id = "notif_" + std::to_string(++notificationCounter_);
  notif.title = title;
  notif.message = message;
  notif.type = NotificationType::Progress;
  notif.dismiss =
      persistent ? NotificationDismiss::Persistent : NotificationDismiss::Auto;
  notif.progress = progress;
  notif.createdAt = std::chrono::steady_clock::now();
  notifications_.push_back(notif);
  return notif.id;
}

void NotificationManager::updateProgress(const std::string &id, float progress,
                                        const std::string &label) {
  for (auto &notif : notifications_) {
    if (notif.id == id) {
      notif.progress = progress;
      notif.progressLabel = label;
      if (progress >= 1.0f &&
          notif.dismiss == NotificationDismiss::Persistent) {
        notif.dismiss = NotificationDismiss::Auto;
        notif.autoDismissAfter = std::chrono::milliseconds(2000);
      }
      break;
    }
  }
}

void NotificationManager::dismiss(const std::string &id) {
  for (auto &notif : notifications_) {
    if (notif.id == id) {
      notif.visible = false;
      if (notif.onDismiss) {
        notif.onDismiss();
      }
      break;
    }
  }
  notifications_.erase(
      std::remove_if(notifications_.begin(), notifications_.end(),
                     [](const Notification &n) { return !n.visible; }),
      notifications_.end());
}

void NotificationManager::dismissAll() { notifications_.clear(); }

void NotificationManager::dismissOldest() {
  if (!notifications_.empty()) {
    dismiss(notifications_.front().id);
  }
}

void NotificationManager::toggleVisibility() { visible_ = !visible_; }

size_t NotificationManager::getUnreadCount() const {
  return std::count_if(notifications_.begin(), notifications_.end(),
                       [](const Notification &n) { return n.visible; });
}

void NotificationManager::cleanupExpired() {
  auto now = std::chrono::steady_clock::now();
  notifications_.erase(
      std::remove_if(
          notifications_.begin(), notifications_.end(),
          [now](const Notification &notif) {
            if (notif.dismiss != NotificationDismiss::Auto)
              return false;
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - notif.createdAt);
            return elapsed > notif.autoDismissAfter;
          }),
      notifications_.end());
}

ftxui::Color NotificationManager::getColorForType(NotificationType type) {
  switch (type) {
  case NotificationType::Info:
    return ftxui::Color::RGB(100, 180, 255);
  case NotificationType::Success:
    return ftxui::Color::RGB(100, 220, 150);
  case NotificationType::Warning:
    return ftxui::Color::RGB(255, 200, 100);
  case NotificationType::Error:
    return ftxui::Color::RGB(255, 100, 100);
  case NotificationType::Progress:
    return ftxui::Color::RGB(180, 150, 255);
  }
  return ftxui::Color::White;
}

ftxui::Element NotificationManager::renderNotification(const Notification &notif) {
  auto color = getColorForType(notif.type);
  const int terminal_width = std::max(40, ftxui::Terminal::Size().dimx);
  const int card_width = std::max(28, std::min(46, terminal_width - 8));

  // Icon based on type
  std::string icon;
  switch (notif.type) {
  case NotificationType::Info:
    icon = "ℹ";
    break;
  case NotificationType::Success:
    icon = "✓";
    break;
  case NotificationType::Warning:
    icon = "⚠";
    break;
  case NotificationType::Error:
    icon = "✕";
    break;
  case NotificationType::Progress:
    icon = "◐";
    break;
  }

  ftxui::Elements body_rows;
  body_rows.push_back(ftxui::hbox({
      ftxui::text(icon + " ") | ftxui::bold | ftxui::color(color),
      ftxui::paragraph(notif.title) | ftxui::bold | ftxui::color(color) |
          ftxui::xflex,
  }));
  if (!notif.message.empty()) {
    body_rows.push_back(ftxui::paragraph(notif.message) |
                        ftxui::color(ftxui::Color::RGB(200, 200, 220)));
  }

  // Progress bar for progress notifications
  if (notif.type == NotificationType::Progress) {
    auto bar = ftxui::gauge(notif.progress) | ftxui::color(color) |
               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 10);
    body_rows.push_back(ftxui::text(""));
    body_rows.push_back(ftxui::hbox({ftxui::text(" "), bar}));
    if (!notif.progressLabel.empty()) {
      body_rows.push_back(ftxui::text(" " + notif.progressLabel) | ftxui::dim);
    }
  }

  // Action button if available
  if (!notif.actionLabel.empty()) {
    body_rows.push_back(ftxui::text(""));
    body_rows.push_back(ftxui::hbox({
        ftxui::text(" "),
        ftxui::text(" " + notif.actionLabel + " ") | ftxui::color(color) |
            ftxui::bold,
    }));
  }

  auto body = ftxui::vbox({
      ftxui::text(""),
      ftxui::hbox({
          ftxui::text("  "),
          ftxui::vbox(std::move(body_rows)) | ftxui::xflex,
          ftxui::text("  "),
      }),
      ftxui::text(""),
  }) |
              ftxui::size(ftxui::WIDTH, ftxui::EQUAL, card_width) |
              ftxui::bgcolor(ftxui::Color::RGB(30, 30, 50)) |
              ftxui::color(ftxui::Color::RGB(200, 200, 220));

  return ftxui::hbox({
             ftxui::text(" ") | ftxui::bgcolor(color),
             body,
         }) |
         ftxui::clear_under;
}

ftxui::Element NotificationManager::render() {
  cleanupExpired();

  if (!visible_ || notifications_.empty()) {
    return ftxui::text("");
  }

  // Show max 4 notifications at once
  const size_t count = std::min(notifications_.size(), size_t(4));

  ftxui::Elements notif_elements;
  for (size_t i = 0; i < count; ++i) {
    if (i > 0) {
      notif_elements.push_back(ftxui::text(""));
    }
    notif_elements.push_back(renderNotification(notifications_[i]));
  }

  // Keep the overlay anchored near the top-right of the screen, but let the
  // notification card itself handle clearing its own occupied cells.
  return ftxui::vbox({
             ftxui::hbox({
                 ftxui::filler(),
                 ftxui::vbox(std::move(notif_elements)),
                 ftxui::text(" "),
             }),
             ftxui::filler(),
         });
}

} // namespace firmius::tui
