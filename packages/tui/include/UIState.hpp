#ifndef FIRMIUS_TUI_UI_STATE_HPP
#define FIRMIUS_TUI_UI_STATE_HPP

namespace firmius::tui {

// Global UI state that can be accessed by all components
struct UIState {
  static UIState& instance() {
    static UIState inst;
    return inst;
  }
  
  // Diff expansion state (Ctrl+G toggle)
  bool diffsExpanded = true;
  
  // Syntax highlighting enabled
  bool syntaxHighlightingEnabled = true;
  
  // Max lines to show when diffs are collapsed
  int maxCollapsedLines = 10;
  
private:
  UIState() = default;
};

} // namespace firmius::tui

#endif
