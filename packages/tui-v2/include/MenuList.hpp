#pragma once

#include "Component.hpp"

#include <functional>
#include <string>
#include <vector>

namespace firmius::tui2 {

/// A generic interactive list menu rendered in the pinned zone.
///
/// Displays a title, items with a selection cursor, and a hint line.
/// Reusable for /models, /resume, etc.
class MenuList : public Component {
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
  void setItems(std::vector<Item> items) { items_ = std::move(items); selectedIndex_ = 0; }
  void setOnSelect(SelectCallback cb) { onSelect_ = std::move(cb); }
  void setOnDismiss(DismissCallback cb) { onDismiss_ = std::move(cb); }
  void open() { isOpen_ = true; }
  void close() { isOpen_ = false; items_.clear(); }

  /// Maximum number of visible rows (excluding title + hints).
  void setMaxVisibleItems(int n) { maxVisible_ = n; }

  // Navigation.
  void moveUp();
  void moveDown();
  void selectCurrent();
  void dismiss();

  bool isActive() const { return isOpen_; }
  int selectedIndex() const { return selectedIndex_; }

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;

private:
  std::string title_;
  std::vector<Item> items_;
  int selectedIndex_ = 0;
  int maxVisible_ = 10;
  SelectCallback onSelect_;
  DismissCallback onDismiss_;
  bool isOpen_ = false;
};

} // namespace firmius::tui2
