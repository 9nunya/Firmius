#pragma once

#include "Overlay.hpp"

#include <functional>
#include <string>
#include <vector>

namespace firmius::tui2 {

class InfoOverlay : public Overlay {
public:
  using DismissCallback = std::function<void()>;

  InfoOverlay() = default;

  void setTitle(const std::string& title) { title_ = title; }

  void setContent(std::vector<std::string> lines);

  void setOnDismiss(DismissCallback cb) { onDismiss_ = std::move(cb); }

  void setMaxVisibleLines(int n) { maxVisible_ = n; }

  void open() override;
  void close() override;

  bool isActive() const override { return isOpen_; }

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;

  bool handleInput(const std::string& key) override;
  bool handleMouse(const MouseEvent& event,
                   int screenRow,
                   int screenCol) override;

private:
  std::string title_;
  std::vector<std::string> content_;
  mutable int scrollOffset_ = 0;
  int maxVisible_ = 20;
  DismissCallback onDismiss_;
  bool isOpen_ = false;
};

} // namespace firmius::tui2
