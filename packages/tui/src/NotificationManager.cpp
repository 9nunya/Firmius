#include "NotificationManager.hpp"
#include "components/GlintEffect.hpp"
#include <ftxui/dom/elements.hpp>
#include <algorithm>
#include <sstream>

namespace firmius::tui {

NotificationManager& NotificationManager::instance() {
  static NotificationManager inst;
  return inst;
}

std::string NotificationManager::notifyInfo(
    const std::string& title, const std::string& message,
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
    const std::string& title, const std::string& message,
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
    const std::string& title, const std::string& message,
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

std::string NotificationManager::notifyError(
    const std::string& title, const std::string& message,
    bool persistent) {
  Notification notif;
  notif.id = "notif_" + std::to_string(++notificationCounter_);
  notif.title = title;
  notif.message = message;
  notif.type = NotificationType::Error;
  notif.dismiss = persistent ? NotificationDismiss::Manual : NotificationDismiss::Auto;
  notif.autoDismissAfter = persistent ? std::chrono::milliseconds(0) : std::chrono::milliseconds(8000);
  notif.createdAt = std::chrono::steady_clock::now();
  notifications_.push_back(notif);
  return notif.id;
}

std::string NotificationManager::notifyProgress(
    const std::string& title, const std::string& message,
    float progress, bool persistent) {
  Notification notif;
  notif.id = "notif_" + std::to_string(++notificationCounter_);
  notif.title = title;
  notif.message = message;
  notif.type = NotificationType::Progress;
  notif.dismiss = persistent ? NotificationDismiss::Persistent : NotificationDismiss::Auto;
  notif.progress = progress;
  notif.createdAt = std::chrono::steady_clock::now();
  notifications_.push_back(notif);
  return notif.id;
}

void NotificationManager::updateProgress(const std::string& id, float progress,
                                         const std::string& label) {
  for (auto& notif : notifications_) {
    if (notif.id == id) {
      notif.progress = progress;
      notif.progressLabel = label;
      if (progress >= 1.0f && notif.dismiss == NotificationDismiss::Persistent) {
        notif.dismiss = NotificationDismiss::Auto;
        notif.autoDismissAfter = std::chrono::milliseconds(2000);
      }
      break;
    }
  }
}

void NotificationManager::dismiss(const std::string& id) {
  for (auto& notif : notifications_) {
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
                   [](const Notification& n) { return !n.visible; }),
    notifications_.end());
}

void NotificationManager::dismissAll() {
  notifications_.clear();
}

void NotificationManager::dismissOldest() {
  if (!notifications_.empty()) {
    dismiss(notifications_.front().id);
  }
}

void NotificationManager::toggleVisibility() {
  visible_ = !visible_;
}

size_t NotificationManager::getUnreadCount() const {
  return std::count_if(notifications_.begin(), notifications_.end(),
                       [](const Notification& n) { return n.visible; });
}

void NotificationManager::cleanupExpired() {
  auto now = std::chrono::steady_clock::now();
  notifications_.erase(
    std::remove_if(notifications_.begin(), notifications_.end(),
      [now](const Notification& notif) {
        if (notif.dismiss != NotificationDismiss::Auto) return false;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
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

ftxui::Element NotificationManager::renderNotification(const Notification& notif) {
  auto color = getColorForType(notif.type);
  
  // Icon based on type
  std::string icon;
  switch (notif.type) {
    case NotificationType::Info: icon = "ℹ"; break;
    case NotificationType::Success: icon = "✓"; break;
    case NotificationType::Warning: icon = "⚠"; break;
    case NotificationType::Error: icon = "✕"; break;
    case NotificationType::Progress: icon = "◐"; break;
  }
  
  ftxui::Elements parts;
  
  // Icon with glint effect for active notifications
  auto icon_el = ftxui::text(icon + " ") | ftxui::bold | ftxui::color(color);
  parts.push_back(icon_el);
  
  // Title
  parts.push_back(ftxui::text(notif.title + " ") | ftxui::bold | ftxui::color(color));
  
  // Message (truncated if too long)
  std::string msg = notif.message;
  if (msg.size() > 60) msg = msg.substr(0, 57) + "…";
  parts.push_back(ftxui::text(msg) | ftxui::dim);
  
  // Progress bar for progress notifications
  if (notif.type == NotificationType::Progress) {
    auto bar = ftxui::gauge(notif.progress) | ftxui::color(color) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 10);
    parts.push_back(ftxui::text(" "));
    parts.push_back(bar);
    if (!notif.progressLabel.empty()) {
      parts.push_back(ftxui::text(" " + notif.progressLabel) | ftxui::dim);
    }
  }
  
  // Action button if available
  if (!notif.actionLabel.empty()) {
    parts.push_back(ftxui::text(" ["));
    parts.push_back(ftxui::text(notif.actionLabel) | ftxui::color(color) | ftxui::bold);
    parts.push_back(ftxui::text("]"));
  }
  
  return ftxui::hbox(parts) | 
         ftxui::bgcolor(ftxui::Color::RGB(30, 30, 50)) |
         ftxui::borderRounded |
         ftxui::color(ftxui::Color::RGB(200, 200, 220));
}

ftxui::Element NotificationManager::render() {
  cleanupExpired();
  
  if (!visible_ || notifications_.empty()) {
    return ftxui::text("");
  }
  
  // Show max 4 notifications at once
  size_t count = std::min(notifications_.size(), size_t(4));
  
  ftxui::Elements notif_elements;
  for (size_t i = 0; i < count; ++i) {
    notif_elements.push_back(renderNotification(notifications_[i]));
  }
  
  return ftxui::dbox(notif_elements) | ftxui::align_right;
}

} // namespace firmius::tui
