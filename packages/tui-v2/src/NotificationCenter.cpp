#include "NotificationCenter.hpp"

#include <algorithm>

namespace firmius::tui2 {

NotificationFrame
describeNotification(const Notification& notif,
                      std::chrono::steady_clock::time_point now) {
  NotificationFrame frame;
  if (now >= notif.fadeOutUntil) {
    frame.phase = NotificationPhase::Expired;
    frame.opacity = 0.0f;
    return frame;
  }
  if (now < notif.fadeInUntil) {
    // Linear ramp 0 → 1 over [createdAt, fadeInUntil).
    const auto total = std::chrono::duration<float>(
                            notif.fadeInUntil - notif.createdAt)
                            .count();
    const auto into  = std::chrono::duration<float>(now - notif.createdAt).count();
    frame.phase = NotificationPhase::FadingIn;
    frame.opacity = total > 0.0f ? std::clamp(into / total, 0.0f, 1.0f) : 1.0f;
    return frame;
  }
  if (now < notif.visibleUntil) {
    frame.phase = NotificationPhase::Visible;
    frame.opacity = 1.0f;
    return frame;
  }
  // FadingOut: linear ramp 1 → 0 over [visibleUntil, fadeOutUntil).
  const auto total = std::chrono::duration<float>(
                          notif.fadeOutUntil - notif.visibleUntil)
                          .count();
  const auto into  = std::chrono::duration<float>(now - notif.visibleUntil).count();
  frame.phase = NotificationPhase::FadingOut;
  frame.opacity = total > 0.0f ? std::clamp(1.0f - (into / total), 0.0f, 1.0f) : 0.0f;
  return frame;
}

std::uint64_t NotificationCenter::push(NotificationKind kind,
                                        std::string message,
                                        std::string dedupeKey) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  // Pick durations based on kind — errors stay longer because they're
  // less recoverable.
  const auto fadeIn   = kDefaultFadeIn;
  const auto fadeOut  = kDefaultFadeOut;
  const auto visible  = (kind == NotificationKind::Error)
                            ? kErrorVisible : kDefaultVisible;

  // Dedup: if a live notification with the same key exists, update it
  // in place (no fade-in, fresh visible window).
  if (!dedupeKey.empty()) {
    for (auto &n : notifications_) {
      if (n.dedupeKey != dedupeKey) continue;
      if (now >= n.fadeOutUntil) continue;  // already expired, replace
      n.kind = kind;
      n.message = std::move(message);
      // Skip fade-in on update — the user just refreshed an existing toast.
      n.fadeInUntil   = now;
      n.visibleUntil  = now + visible;
      n.fadeOutUntil  = n.visibleUntil + fadeOut;
      return n.id;
    }
  }

  Notification n;
  n.id = nextId_++;
  n.kind = kind;
  n.message = std::move(message);
  n.createdAt = now;
  n.fadeInUntil  = now + fadeIn;
  n.visibleUntil = n.fadeInUntil + visible;
  n.fadeOutUntil = n.visibleUntil + fadeOut;
  n.dedupeKey = std::move(dedupeKey);
  notifications_.push_back(std::move(n));
  return notifications_.back().id;
}

void NotificationCenter::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  notifications_.clear();
}

std::vector<Notification> NotificationCenter::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return notifications_;
}

bool NotificationCenter::needsAnimationTick(
    std::chrono::steady_clock::time_point now) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &n : notifications_) {
    auto frame = describeNotification(n, now);
    if (frame.phase == NotificationPhase::FadingIn ||
        frame.phase == NotificationPhase::FadingOut) {
      return true;
    }
  }
  return false;
}

bool NotificationCenter::hasLive(
    std::chrono::steady_clock::time_point now) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &n : notifications_) {
    if (now < n.fadeOutUntil) return true;
  }
  return false;
}

void NotificationCenter::prune(std::chrono::steady_clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  notifications_.erase(
      std::remove_if(notifications_.begin(), notifications_.end(),
                     [&](const Notification &n) {
                       return now >= n.fadeOutUntil;
                     }),
      notifications_.end());
}

} // namespace firmius::tui2
