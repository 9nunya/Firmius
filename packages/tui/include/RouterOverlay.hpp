#pragma once

#include "Overlay.hpp"
#include "daemon/Protocol.hpp"
#include "ConfigLoader.hpp"
#include "Enums.hpp"

#include <functional>
#include <string>
#include <vector>

namespace firmius::tui {

/// /router overlay — manage model routing categories + fallback order.
///
/// Phases:
///   Browse          — list categories, set/clear default, enter detail
///   Detail          — list models in a category, add/remove
///   AddName         — text input for new category name
///   AddPickModel    — model picker after naming a new category
///   DetailPickModel — model picker to add a model to existing category
///   Rename          — text input to rename a category
///   ConfirmDelete   — y/N to delete a category
///   ConfirmDeleteModel — y/N to remove a model from a category
///   FallbackOrder   — reorder categories for subagent fallback
class RouterOverlay : public Overlay {
public:
  using SaveCallback =
      std::function<void(const firmius::daemon::RouterConfigUpdateRequest&)>;
  using DismissCallback = std::function<void()>;

  RouterOverlay() = default;

  void seed(firmius::daemon::RouterConfigSnapshot snapshot,
            std::vector<firmius::shared::ModelInfo> models);

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
  enum class Phase {
    Browse,
    Detail,
    AddName,
    AddPickModel,
    DetailPickModel,
    Rename,
    ConfirmDelete,
    ConfirmDeleteModel,
    FallbackOrder,
  };

  // ── Helpers ──
  std::string selectedCategory() const;
  void save();
  void rebuildFilteredModels();
  std::string renderModelOption(const firmius::shared::ModelOption& m) const;

  // ── State ──
  Phase phase_ = Phase::Browse;
  bool isOpen_ = false;

  // Data
  std::map<std::string, firmius::shared::ModelRouteCategory> categories_;
  std::string defaultRoute_;
  bool fallbackEnabled_ = true;
  std::vector<std::string> fallbackOrder_;

  std::vector<std::string> sortedNames_;  // derived from categories_
  int cursor_ = 0;
  int detailCursor_ = 0;
  bool detailGrabbed_ = false;

  // Text input
  std::string inputBuf_;

  // Model picker
  struct PickerEntry {
    std::string providerId;
    std::string modelId;
    std::string variantName;
    std::string label;
    std::string searchText;
  };
  std::vector<PickerEntry> allModels_;
  std::vector<int> filteredIndices_;
  std::string modelFilter_;
  int modelCursor_ = 0;

  // Fallback order editing
  int fallbackCursor_ = 0;
  bool fallbackGrabbed_ = false;

  std::string message_;

  SaveCallback onSave_;
  DismissCallback onDismiss_;

  static constexpr int kMaxVisible = 10;
};

} // namespace firmius::tui
