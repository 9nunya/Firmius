#pragma once

#include "Overlay.hpp"
#include "daemon/Protocol.hpp"

#include <functional>
#include <string>
#include <vector>

namespace firmius::tui2 {

/// /purposes overlay — map personas to routing categories.
/// Single-phase: ↑↓ navigate purposes, ←/→ cycle category, C clear.
class PurposesOverlay : public Overlay {
public:
  using SaveCallback =
      std::function<void(const firmius::daemon::PurposesConfigUpdateRequest&)>;
  using DismissCallback = std::function<void()>;

  PurposesOverlay() = default;

  void seed(std::vector<std::string> purposes,
            std::map<std::string, std::string> routes,
            std::vector<std::string> categories);

  void setOnSave(SaveCallback cb) { onSave_ = std::move(cb); }
  void setOnDismiss(DismissCallback cb) { onDismiss_ = std::move(cb); }

  void open() override;
  void close() override;
  bool isActive() const override { return isOpen_; }

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;
  bool handleInput(const std::string& key) override;
  bool handleMouse(const MouseEvent&, int, int) override { return isOpen_; }

private:
  void save();

  bool isOpen_ = false;
  std::vector<std::string> purposes_;
  std::map<std::string, std::string> routes_;
  std::vector<std::string> categories_;
  int cursor_ = 0;
  std::string message_;

  SaveCallback onSave_;
  DismissCallback onDismiss_;
};

} // namespace firmius::tui2
