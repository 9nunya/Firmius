#pragma once

#include "AppState.hpp"
#include "Component.hpp"

#include <string>

namespace firmius::tui2 {

/// Input bar component. Renders the user's input buffer surrounded by
/// horizontal rules. Supports multiline input (the buffer can contain
/// embedded '\n' characters, inserted by Shift+Enter).
class InputBar : public Component {
public:
  explicit InputBar(const AppState& state);

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;
  /// Vertical distance from the top of the rendered InputBar to the first
  /// line of input content. The App adds the cursor's logical line index
  /// on top of this to position the terminal cursor.
  int cursorRowOffset() const;

  /// How many rows down from the InputBar's top the cursor line is rendered
  /// at. Accounts for vertical scrolling of long inputs.
  int cursorVisualRow() const;

  /// Width-aware variant of cursorVisualRow that accounts for soft-wrap
  /// of long logical lines. The App passes the same `width` it passes to
  /// render().
  int cursorVisualRowFor(int width) const;

  /// Visible column (0-indexed) of the cursor inside the wrapped row.
  /// Equivalent to cursorColumnOnLine() % bodyWidth — the App needs this
  /// because the bare AppState column is the column on the LOGICAL line,
  /// not the wrapped visual line.
  int cursorVisualColumnFor(int width) const;

  /// Cap on visible input rows. Beyond this, the bar acts like a 12-line
  /// scrolling viewport — the cursor stays in view, distant lines are
  /// hidden until the user navigates to them.
  static constexpr int kMaxVisibleLines = 12;

private:
  const AppState& state_;
};

} // namespace firmius::tui2
