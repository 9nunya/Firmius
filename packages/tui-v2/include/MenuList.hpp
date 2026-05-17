#pragma once

#include "Overlay.hpp"

#include <functional>
#include <string>
#include <vector>

namespace firmius::tui2 {

/// A generic interactive list menu rendered in the pinned zone.
///
/// Displays a title, items with a selection cursor, and a hint line.
/// Supports fuzzy search with space-separated tag matching.
class MenuList : public Overlay {
public:
  struct Item {
    std::string label;
    std::string detail;   ///< Secondary text (e.g. model description, time ago).
    std::string id;       ///< Opaque identifier returned on selection.
    bool marked = false;  ///< E.g. currently active model.
  };

  using SelectCallback = std::function<void(const Item&)>;
  using DismissCallback = std::function<void()>;

  MenuList() = default;

  void setTitle(const std::string& title) { title_ = title; }
  void setItems(std::vector<Item> items);
  void setOnSelect(SelectCallback cb) { onSelect_ = std::move(cb); }
  void setOnDismiss(DismissCallback cb) { onDismiss_ = std::move(cb); }
  void open() { isOpen_ = true; searchQuery_.clear(); filteredDirty_ = true; }
  void close() { isOpen_ = false; items_.clear(); searchQuery_.clear(); }

  /// Maximum number of visible rows (excluding title + search + hints).
  void setMaxVisibleItems(int n) { maxVisible_ = n; }

  // Navigation.
  void moveUp();
  void moveDown();
  void selectCurrent();
  void dismiss();

  // Search.
  void setSearchQuery(const std::string& query);
  void appendToSearch(char ch);
  void backspaceSearch();
  const std::string& searchQuery() const { return searchQuery_; }

  // Mouse support.
  /// Returns the screen row where the menu starts (set by App before render).
  void setScreenRow(int row) { screenRow_ = row; }
  int screenRow() const { return screenRow_; }
  /// Height of the items area (excluding title/separator/hint).
  int itemsAreaHeight() const;
  /// Index of the first visible item (for offset calculation).
  int firstVisibleIndex() const;

  bool isActive() const { return isOpen_; }
  int selectedIndex() const { return selectedIndex_; }

  /// Get the actual item at the given filtered index.
  const Item* itemAtFilteredIndex(int idx) const;

  /// Set which item the mouse is hovering over (-1 for none).
  void setHoveredIndex(int idx) { hoveredIndex_ = idx; }
  int hoveredIndex() const { return hoveredIndex_; }

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;

  bool handleInput(const std::string& key) override;
  bool handleMouse(const MouseEvent& event,
                   int screenRow,
                   int screenCol) override;

private:
  void rebuildFiltered() const;

  std::string title_;
  std::vector<Item> items_;
  int selectedIndex_ = 0;
  int maxVisible_ = 10;
  SelectCallback onSelect_;
  DismissCallback onDismiss_;
  bool isOpen_ = false;

  // Search state.
  std::string searchQuery_;
  mutable bool filteredDirty_ = true;
  mutable std::vector<int> filteredIndices_;  ///< Indices into items_ that match.

  // Mouse state.
  int screenRow_ = 0;
  int hoveredIndex_ = -1;  ///< Index of item under mouse cursor, -1 if none.
};

} // namespace firmius::tui2
