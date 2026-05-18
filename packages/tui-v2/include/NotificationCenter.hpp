#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace firmius::tui2 {

/// Severity / colour key for a notification. The renderer maps these to
/// theme-aware colours and a small Unicode glyph.
enum class NotificationKind {
  Info,
  Success,
  Warning,
  Error,
};

/// A transient toast. The lifecycle is described entirely by timestamps
/// — there is no separate "phase" enum because that would let the data
/// drift away from wall-clock time. Renderers compute opacity / phase
/// on demand from `createdAt`, `fadeInUntil`, `visibleUntil`,
/// `fadeOutUntil`.
struct Notification {
  std::uint64_t id = 0;
  NotificationKind kind = NotificationKind::Info;
  std::string message;
  std::chrono::steady_clock::time_point createdAt{};
  std::chrono::steady_clock::time_point fadeInUntil{};
  std::chrono::steady_clock::time_point visibleUntil{};
  std::chrono::steady_clock::time_point fadeOutUntil{};
  /// Optional dedupe key. When two notifications share a non-empty key,
  /// the new one replaces the old one in place — useful for repeated
  /// "Copied N chars" while the user is dragging selections.
  std::string dedupeKey;
};

/// Computed lifecycle phase for a notification at a given moment.
enum class NotificationPhase {
  FadingIn,
  Visible,
  FadingOut,
  Expired,
};

/// Phase + opacity (0..1) at a given moment. Pure helper; doesn't touch
/// state.
struct NotificationFrame {
  NotificationPhase phase = NotificationPhase::Expired;
  float opacity = 0.0f;
};

NotificationFrame describeNotification(const Notification& notif,
                                        std::chrono::steady_clock::time_point now);

/// Thread-safe registry of live notifications. Owns no rendering — App
/// reads `snapshot()` each frame and passes it to the renderer.
class NotificationCenter {
public:
  /// Common defaults so callers don't have to think about durations.
  static constexpr auto kDefaultFadeIn      = std::chrono::milliseconds(120);
  static constexpr auto kDefaultVisible     = std::chrono::milliseconds(2400);
  static constexpr auto kDefaultFadeOut     = std::chrono::milliseconds(360);
  static constexpr auto kErrorVisible       = std::chrono::milliseconds(6000);

  NotificationCenter() = default;

  /// Push a new notification. Returns its id. If `dedupeKey` is set and
  /// matches an existing live notification, that one is updated in place
  /// (message replaced, fade-in skipped) instead of stacking.
  std::uint64_t push(NotificationKind kind, std::string message,
                     std::string dedupeKey = "");

  /// Convenience overloads — all auto-dismiss.
  std::uint64_t info(std::string message, std::string dedupeKey = "") {
    return push(NotificationKind::Info, std::move(message), std::move(dedupeKey));
  }
  std::uint64_t success(std::string message, std::string dedupeKey = "") {
    return push(NotificationKind::Success, std::move(message), std::move(dedupeKey));
  }
  std::uint64_t warning(std::string message, std::string dedupeKey = "") {
    return push(NotificationKind::Warning, std::move(message), std::move(dedupeKey));
  }
  std::uint64_t error(std::string message, std::string dedupeKey = "") {
    return push(NotificationKind::Error, std::move(message), std::move(dedupeKey));
  }

  /// Drop everything. Used on /clear or thread-switch.
  void clear();

  /// Snapshot of currently-live (not yet fully expired) notifications.
  /// Cheap: returns by value but the list is short and the lock is held
  /// briefly.
  std::vector<Notification> snapshot() const;

  /// Whether any live notification is currently animating. App uses this
  /// to decide whether to bump the render-loop cadence.
  bool needsAnimationTick(std::chrono::steady_clock::time_point now) const;

  /// Whether anything is live at all (including solid Visible state).
  bool hasLive(std::chrono::steady_clock::time_point now) const;

  /// Remove fully-expired entries. Call from the render loop tick.
  void prune(std::chrono::steady_clock::time_point now);

private:
  mutable std::mutex mutex_;
  std::vector<Notification> notifications_;
  std::uint64_t nextId_ = 1;
};

} // namespace firmius::tui2
