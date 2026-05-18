#pragma once

#include "Component.hpp"
#include "NotificationCenter.hpp"

#include <string>
#include <vector>

namespace firmius::tui2 {

/// One-line-per-toast renderer for the notification stack. Lives just
/// above the input bar in the pinned zone.
///
/// Why not an Overlay? Overlays grab input focus; a notification strip
/// is "ambient" — it never steals key/mouse events, just displays.
/// Component is the right base.
class NotificationStrip : public Component {
public:
  explicit NotificationStrip(const NotificationCenter& center);

  /// Maximum number of notifications shown at once. Beyond this, oldest
  /// non-error toasts get squeezed out of the visible stack but stay
  /// alive in the centre (they'll appear if a newer one expires first).
  static constexpr int kMaxVisible = 4;

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;

private:
  /// Pull the active stack — newest at the bottom (so animations slide
  /// in from below the most recent), capped at kMaxVisible.
  std::vector<Notification> activeStack() const;

  const NotificationCenter& center_;
};

} // namespace firmius::tui2
