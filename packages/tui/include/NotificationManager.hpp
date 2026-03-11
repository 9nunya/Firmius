#ifndef FIRMIUS_TUI_NOTIFICATION_MANAGER_HPP
#define FIRMIUS_TUI_NOTIFICATION_MANAGER_HPP

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>
#include <chrono>
#include <memory>
#include <functional>

namespace firmius::tui {

enum class NotificationType {
  Info,
  Success,
  Warning,
  Error,
  Progress
};

enum class NotificationDismiss {
  Auto,      // Disappears after timeout
  Manual,    // Must be dismissed by user
  Persistent // Stays until action completes
};

struct Notification {
  std::string id;
  std::string title;
  std::string message;
  NotificationType type = NotificationType::Info;
  NotificationDismiss dismiss = NotificationDismiss::Auto;
  
  // For progress notifications
  float progress = 0.0f;  // 0.0 to 1.0
  std::string progressLabel;
  
  // Timing
  std::chrono::steady_clock::time_point createdAt;
  std::chrono::milliseconds autoDismissAfter{3000}; // 3 seconds for auto
  
  // Actions
  std::function<void()> onDismiss;
  std::function<void()> onAction;
  std::string actionLabel;
  
  bool visible = true;
  float opacity = 1.0f; // For fade animation
};

class NotificationManager {
public:
  static NotificationManager& instance();
  
  // Create notifications
  std::string notifyInfo(const std::string& title, const std::string& message,
                         std::chrono::milliseconds duration = std::chrono::milliseconds(3000));
  
  std::string notifySuccess(const std::string& title, const std::string& message,
                            std::chrono::milliseconds duration = std::chrono::milliseconds(3000));
  
  std::string notifyWarning(const std::string& title, const std::string& message,
                           std::chrono::milliseconds duration = std::chrono::milliseconds(5000));
  
  std::string notifyError(const std::string& title, const std::string& message,
                         bool persistent = false);
  
  std::string notifyProgress(const std::string& title, const std::string& message,
                            float progress = 0.0f, bool persistent = true);
  
  // Update progress
  void updateProgress(const std::string& id, float progress, 
                     const std::string& label = "");
  
  // Dismiss notifications
  void dismiss(const std::string& id);
  void dismissAll();
  void dismissOldest();
  
  // Toggle visibility
  void toggleVisibility();
  bool isVisible() const { return visible_; }
  
  // Render
  ftxui::Element render();
  
  // Get active notifications
  const std::vector<Notification>& getNotifications() const { return notifications_; }
  size_t getUnreadCount() const;
  
private:
  NotificationManager() = default;
  void cleanupExpired();
  ftxui::Element renderNotification(const Notification& notif);
  ftxui::Color getColorForType(NotificationType type);
  
  std::vector<Notification> notifications_;
  bool visible_ = true;
  int notificationCounter_ = 0;
};

} // namespace firmius::tui

#endif
