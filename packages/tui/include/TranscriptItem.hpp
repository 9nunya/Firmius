#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace firmius::tui {

enum class ToolPhase { Preparing, Called, FinishedSuccess, FinishedError };

/// Base class for all transcript items. Each item owns its state and rendering.
class TranscriptItem {
public:
  virtual ~TranscriptItem() = default;

  /// Type identifier for this item (e.g. "UserMessage", "ToolCall").
  virtual std::string_view type() const = 0;

  /// Render this item into N lines of ANSI-formatted text.
  virtual std::vector<std::string> render(int width) const = 0;

  /// How many terminal rows this item occupies at the given width.
  virtual int rowCount(int width) const = 0;

  /// Whether this item has received all its content (no more updates coming).
  /// Immutable items (UserMessage, SystemNotice, etc.) are always finalized.
  /// Streaming items (AgentText, AgentThinking) return false until finalized.
  virtual bool isFinalized() const { return true; }

  /// Whether this item needs re-rendering.
  bool needsRender() const { return dirty_; }

  /// Mark this item as clean (already rendered).
  void markClean() { dirty_ = false; }

  /// Mark this item as dirty (needs re-render).
  void markDirty() { dirty_ = true; }

  /// When this item was created.
  std::chrono::steady_clock::time_point createdAt() const { return createdAt_; }

protected:
  /// Subclasses call this when their state changes.
  void touch() { dirty_ = true; }

private:
  std::chrono::steady_clock::time_point createdAt_ = std::chrono::steady_clock::now();
  bool dirty_ = true;
};

} // namespace firmius::tui
