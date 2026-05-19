
#include "RouterOverlay.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"
#include "utils/ModelUtil.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace firmius::tui {

namespace {

std::string normalizeSearch(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    out.push_back(std::isalnum(c) ? static_cast<char>(std::tolower(c)) : ' ');
  }
  return out;
}

bool fuzzyMatch(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  if (haystack.find(needle) != std::string::npos) return true;
  std::size_t cur = 0;
  for (char ch : needle) {
    cur = haystack.find(ch, cur);
    if (cur == std::string::npos) return false;
    ++cur;
  }
  return true;
}

std::string truncate(const std::string& s, int w) {
  if (w <= 0) return "";
  if (static_cast<int>(s.size()) <= w) return s;
  return s.substr(0, w - 1) + "…";
}

} // namespace

// ── Lifecycle ────────────────────────────────────────────────────────────

void RouterOverlay::seed(firmius::daemon::RouterConfigSnapshot snapshot,
                         std::vector<firmius::shared::ModelInfo> models) {
  categories_ = std::move(snapshot.categories);
  defaultRoute_ = std::move(snapshot.defaultRouteCategory);
  fallbackEnabled_ = snapshot.enableSubagentRouteFallback;
  fallbackOrder_ = std::move(snapshot.subagentRouteFallbackOrder);

  sortedNames_.clear();
  for (const auto& [name, _] : categories_) sortedNames_.push_back(name);
  std::sort(sortedNames_.begin(), sortedNames_.end());
  cursor_ = 0;

  // Build picker entries from ModelInfo
  allModels_.clear();
  for (const auto& m : models) {
    const std::string pretty = firmius::shared::PrettifyModelName(m.id);
    auto addEntry = [&](const std::string& variant) {
      std::string label = pretty + " [" + m.provider + "]";
      if (!variant.empty()) label += " (" + variant + ")";
      std::string search = normalizeSearch(label + " " + m.id + " " + variant);
      allModels_.push_back({m.provider, m.id, variant, label, search});
    };
    addEntry("");
    for (const auto& v : m.variants) {
      if (!v.variantName.empty()) addEntry(v.variantName);
    }
  }
  std::sort(allModels_.begin(), allModels_.end(),
            [](const auto& a, const auto& b) { return a.label < b.label; });

  modelFilter_.clear();
  modelCursor_ = 0;
  rebuildFilteredModels();
}

void RouterOverlay::open() {
  isOpen_ = true;
  phase_ = Phase::Browse;
  message_.clear();
}

void RouterOverlay::close() {
  isOpen_ = false;
  phase_ = Phase::Browse;
  message_.clear();
  inputBuf_.clear();
  modelFilter_.clear();
}

// ── Helpers ──────────────────────────────────────────────────────────────

std::string RouterOverlay::selectedCategory() const {
  if (sortedNames_.empty() || cursor_ < 0 ||
      cursor_ >= static_cast<int>(sortedNames_.size()))
    return "";
  return sortedNames_[cursor_];
}

void RouterOverlay::save() {
  if (!onSave_) return;
  firmius::daemon::RouterConfigUpdateRequest req;
  req.categories = categories_;
  req.defaultRouteCategory = defaultRoute_;
  req.enableSubagentRouteFallback = fallbackEnabled_;
  req.subagentRouteFallbackOrder = fallbackOrder_;
  onSave_(req);
}

void RouterOverlay::rebuildFilteredModels() {
  const std::string norm = normalizeSearch(modelFilter_);
  // Tokenize
  std::vector<std::string> tokens;
  std::istringstream iss(norm);
  std::string tok;
  while (iss >> tok) tokens.push_back(tok);

  filteredIndices_.clear();
  for (int i = 0; i < static_cast<int>(allModels_.size()); ++i) {
    bool ok = true;
    for (const auto& t : tokens) {
      if (!fuzzyMatch(allModels_[i].searchText, t)) { ok = false; break; }
    }
    if (ok) filteredIndices_.push_back(i);
  }
  if (modelCursor_ >= static_cast<int>(filteredIndices_.size()))
    modelCursor_ = filteredIndices_.empty() ? 0 : static_cast<int>(filteredIndices_.size()) - 1;
}

std::string RouterOverlay::renderModelOption(const firmius::shared::ModelOption& m) const {
  std::string s = m.providerId + "/" + m.modelId;
  if (!m.variantName.empty()) s += " (" + m.variantName + ")";
  return s;
}

// ── Render ───────────────────────────────────────────────────────────────

int RouterOverlay::height(int) const {
  if (!isOpen_) return 0;
  if (phase_ == Phase::FallbackOrder) return 4 + std::max(1, static_cast<int>(fallbackOrder_.size())) + 4;
  if (phase_ == Phase::AddPickModel || phase_ == Phase::DetailPickModel) return 18;
  if (phase_ == Phase::ConfirmDelete || phase_ == Phase::ConfirmDeleteModel) return 8;
  return 4 + std::min(static_cast<int>(sortedNames_.size()), kMaxVisible) + 6;
}

std::vector<std::string> RouterOverlay::render(int width) const {
  std::vector<std::string> lines;
  if (!isOpen_) return lines;

  auto title = [&](const std::string& t) {
    lines.push_back(ansi::fitToWidth(theme_ansi::accent(ansi::bold(" " + t + " ")), width));
    lines.push_back(theme_ansi::divider(width));
  };

  auto footer = [&](const std::string& hint) {
    if (!message_.empty())
      lines.push_back(ansi::fitToWidth("  " + theme_ansi::dim(message_), width));
    lines.push_back(ansi::fitToWidth("", width));
    lines.push_back(ansi::fitToWidth(theme_ansi::dim(" " + hint + " "), width));
  };

  if (phase_ == Phase::Browse) {
    title("Router — Model Categories");
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::dim("Priority: category → purpose route → default → model"), width));
    if (sortedNames_.empty()) {
      lines.push_back(ansi::fitToWidth("  " + theme_ansi::dim("No categories yet."), width));
    } else {
      for (int i = 0; i < static_cast<int>(sortedNames_.size()); ++i) {
        const auto& name = sortedNames_[i];
        const bool sel = (i == cursor_);
        const bool isDef = (name == defaultRoute_);
        auto it = categories_.find(name);
        std::string info = "no models";
        if (it != categories_.end() && !it->second.models.empty()) {
          if (it->second.models.size() == 1)
            info = renderModelOption(it->second.models.front());
          else
            info = std::to_string(it->second.models.size()) + " models";
        }
        std::string line = (sel ? "> " : "  ") + name +
                           (isDef ? " [default]" : "") + " → " + info;
        std::string fitted = ansi::fitToWidth("  " + theme_ansi::foreground(line), width);
        lines.push_back(sel ? theme_ansi::selection(fitted) : fitted);
      }
    }
    // Show fallback status
    lines.push_back(ansi::fitToWidth("", width));
    std::string fb = fallbackEnabled_ ? "enabled" : "disabled";
    std::string order = fallbackOrder_.empty() ? "(all categories)" : "";
    for (size_t i = 0; i < fallbackOrder_.size(); ++i) {
      if (i) order += " → ";
      order += fallbackOrder_[i];
    }
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::dim("Fallback: " + fb + "  Order: " + order), width));
    footer("↑↓ nav · Enter detail · A add · R rename · D del · S default · X clear · F fallback · Esc close");
    return lines;
  }

  if (phase_ == Phase::Detail) {
    const std::string cat = selectedCategory();
    title("Router — " + cat);
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::dim("Model #1 is used first. Space to grab, ↑↓ to reorder."), width));
    auto it = categories_.find(cat);
    if (it == categories_.end() || it->second.models.empty()) {
      lines.push_back(ansi::fitToWidth("  " + theme_ansi::dim("No models."), width));
    } else {
      for (int i = 0; i < static_cast<int>(it->second.models.size()); ++i) {
        const bool sel = (i == detailCursor_);
        const bool grabbed = sel && detailGrabbed_;
        std::string marker = grabbed ? "≡ " : (sel ? "> " : "  ");
        std::string line = std::to_string(i + 1) + ". " + renderModelOption(it->second.models[i]);
        if (i == 0) line += "  " + std::string("[primary]");
        std::string fitted = ansi::fitToWidth("  " + theme_ansi::foreground(marker + line), width);
        lines.push_back(sel ? theme_ansi::selection(fitted) : fitted);
      }
    }
    footer("↑↓ nav · Space grab/drop · A add model · D remove · Esc back");
    return lines;
  }

  if (phase_ == Phase::AddName || phase_ == Phase::Rename) {
    title(phase_ == Phase::AddName ? "Router — New Category" : "Router — Rename");
    lines.push_back(ansi::fitToWidth("  Name: " + theme_ansi::accent(inputBuf_ + "▌"), width));
    footer("Enter confirm · Esc cancel");
    return lines;
  }

  if (phase_ == Phase::AddPickModel || phase_ == Phase::DetailPickModel) {
    title("Router — Pick Model");
    lines.push_back(ansi::fitToWidth(
        "  Filter: " + theme_ansi::accent(modelFilter_ + "▌"), width));
    const int show = std::min(static_cast<int>(filteredIndices_.size()), kMaxVisible);
    if (show == 0) {
      lines.push_back(ansi::fitToWidth("  " + theme_ansi::dim("No matches."), width));
    } else {
      int scrollOff = std::max(0, modelCursor_ - kMaxVisible + 1);
      for (int i = 0; i < show; ++i) {
        int idx = scrollOff + i;
        if (idx >= static_cast<int>(filteredIndices_.size())) break;
        const bool sel = (idx == modelCursor_);
        const auto& e = allModels_[filteredIndices_[idx]];
        std::string fitted = ansi::fitToWidth(
            "  " + (sel ? theme_ansi::accent("> ") : std::string("  ")) +
            truncate(e.label, width - 8), width);
        lines.push_back(sel ? theme_ansi::selection(fitted) : fitted);
      }
    }
    footer("↑↓ nav · type to filter · Enter add · Esc cancel");
    return lines;
  }

  if (phase_ == Phase::ConfirmDelete) {
    title("Router — Delete Category");
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::error("Delete '" + selectedCategory() + "'? [y/N]"), width));
    footer("y confirm · any other key cancel");
    return lines;
  }

  if (phase_ == Phase::ConfirmDeleteModel) {
    title("Router — Remove Model");
    const std::string cat = selectedCategory();
    auto it = categories_.find(cat);
    std::string modelName = "model";
    if (it != categories_.end() && detailCursor_ >= 0 &&
        detailCursor_ < static_cast<int>(it->second.models.size()))
      modelName = renderModelOption(it->second.models[detailCursor_]);
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::error("Remove '" + modelName + "'? [y/N]"), width));
    footer("y confirm · any other key cancel");
    return lines;
  }

  if (phase_ == Phase::FallbackOrder) {
    title("Router — Fallback Order");
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::dim("Move categories to set subagent model fallback priority."), width));
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::dim("Toggle: " + std::string(fallbackEnabled_ ? "ENABLED" : "DISABLED")), width));
    if (fallbackOrder_.empty()) {
      lines.push_back(ansi::fitToWidth("  " + theme_ansi::dim("(empty — all categories used in alphabetical order)"), width));
    } else {
      for (int i = 0; i < static_cast<int>(fallbackOrder_.size()); ++i) {
        const bool sel = (i == fallbackCursor_);
        const bool grabbed = sel && fallbackGrabbed_;
        std::string marker = grabbed ? "≡ " : (sel ? "> " : "  ");
        std::string line = std::to_string(i + 1) + ". " + fallbackOrder_[i];
        std::string fitted = ansi::fitToWidth(
            "  " + theme_ansi::foreground(marker + line), width);
        lines.push_back(sel ? theme_ansi::selection(fitted) : fitted);
      }
    }
    footer("↑↓ move · Space grab/drop · A add · D remove · T toggle · Esc back");
    return lines;
  }

  return lines;
}

// ── Input ────────────────────────────────────────────────────────────────

bool RouterOverlay::handleInput(const std::string& key) {
  if (!isOpen_) return false;

  // Esc always backs out one level
  if (key == "\x1b") {
    switch (phase_) {
    case Phase::Browse:
      if (onDismiss_) onDismiss_();
      return true;
    case Phase::Detail:
    case Phase::FallbackOrder:
      phase_ = Phase::Browse;
      message_.clear();
      return true;
    default:
      // Cancel any sub-phase
      phase_ = (phase_ == Phase::DetailPickModel || phase_ == Phase::ConfirmDeleteModel)
                   ? Phase::Detail : Phase::Browse;
      message_.clear();
      return true;
    }
  }

  // ── Browse ──
  if (phase_ == Phase::Browse) {
    if (key == "\x1b[A" && cursor_ > 0) { --cursor_; return true; }
    if (key == "\x1b[B" && cursor_ + 1 < static_cast<int>(sortedNames_.size())) { ++cursor_; return true; }
    if ((key == "\r" || key == "\n") && !sortedNames_.empty()) {
      detailCursor_ = 0;
      phase_ = Phase::Detail;
      return true;
    }
    if (key == "a" || key == "A") {
      inputBuf_.clear(); modelFilter_.clear(); modelCursor_ = 0;
      phase_ = Phase::AddName;
      return true;
    }
    if ((key == "r" || key == "R") && !sortedNames_.empty()) {
      inputBuf_ = selectedCategory();
      phase_ = Phase::Rename;
      return true;
    }
    if ((key == "d" || key == "D") && !sortedNames_.empty()) {
      phase_ = Phase::ConfirmDelete;
      return true;
    }
    if ((key == "s" || key == "S") && !sortedNames_.empty()) {
      defaultRoute_ = selectedCategory();
      message_ = "Default set to '" + defaultRoute_ + "'.";
      save();
      return true;
    }
    if (key == "x" || key == "X") {
      defaultRoute_.clear();
      message_ = "Default cleared.";
      save();
      return true;
    }
    if (key == "f" || key == "F") {
      // Enter fallback order editor. Seed with current order or all categories.
      if (fallbackOrder_.empty()) fallbackOrder_ = sortedNames_;
      fallbackCursor_ = 0;
      fallbackGrabbed_ = false;
      phase_ = Phase::FallbackOrder;
      return true;
    }
    return true;
  }

  // ── Detail ──
  if (phase_ == Phase::Detail) {
    auto it = categories_.find(selectedCategory());
    int count = (it != categories_.end()) ? static_cast<int>(it->second.models.size()) : 0;
    if (key == "\x1b[A") {
      if (detailGrabbed_ && detailCursor_ > 0) {
        std::swap(it->second.models[detailCursor_], it->second.models[detailCursor_ - 1]);
        --detailCursor_;
        message_ = "Moved up — first model is used first.";
        save();
      } else if (!detailGrabbed_ && detailCursor_ > 0) {
        --detailCursor_;
      }
      return true;
    }
    if (key == "\x1b[B") {
      if (detailGrabbed_ && detailCursor_ + 1 < count) {
        std::swap(it->second.models[detailCursor_], it->second.models[detailCursor_ + 1]);
        ++detailCursor_;
        message_ = "Moved down — first model is used first.";
        save();
      } else if (!detailGrabbed_ && detailCursor_ + 1 < count) {
        ++detailCursor_;
      }
      return true;
    }
    if (key == " " && count > 0) {
      detailGrabbed_ = !detailGrabbed_;
      message_ = detailGrabbed_ ? "Grabbed — use ↑↓ to reorder, Space to drop."
                                : "Dropped.";
      return true;
    }
    if (key == "a" || key == "A") {
      detailGrabbed_ = false;
      modelFilter_.clear(); modelCursor_ = 0; rebuildFilteredModels();
      phase_ = Phase::DetailPickModel;
      return true;
    }
    if ((key == "d" || key == "D") && count > 0) {
      detailGrabbed_ = false;
      phase_ = Phase::ConfirmDeleteModel;
      return true;
    }
    return true;
  }

  // ── AddName / Rename ──
  if (phase_ == Phase::AddName || phase_ == Phase::Rename) {
    if (key == "\r" || key == "\n") {
      if (inputBuf_.empty()) { message_ = "Name cannot be empty."; return true; }
      if (phase_ == Phase::AddName) {
        if (categories_.count(inputBuf_)) { message_ = "Already exists."; phase_ = Phase::Browse; return true; }
        modelFilter_.clear(); modelCursor_ = 0; rebuildFilteredModels();
        phase_ = Phase::AddPickModel;
      } else {
        // Rename
        const std::string old = selectedCategory();
        if (old != inputBuf_) {
          if (categories_.count(inputBuf_)) { message_ = "Name already exists."; return true; }
          auto cat = categories_[old];
          categories_.erase(old);
          categories_[inputBuf_] = cat;
          if (defaultRoute_ == old) defaultRoute_ = inputBuf_;
          for (auto& f : fallbackOrder_) { if (f == old) f = inputBuf_; }
          sortedNames_.clear();
          for (const auto& [n, _] : categories_) sortedNames_.push_back(n);
          std::sort(sortedNames_.begin(), sortedNames_.end());
          message_ = "Renamed '" + old + "' → '" + inputBuf_ + "'.";
          save();
        }
        phase_ = Phase::Browse;
      }
      return true;
    }
    if (key == "\x7f" || key == "\b") {
      if (!inputBuf_.empty()) inputBuf_.pop_back();
      return true;
    }
    if (key.size() == 1 && key[0] >= 32) { inputBuf_ += key; return true; }
    return true;
  }

  // ── Model Picker (AddPickModel / DetailPickModel) ──
  if (phase_ == Phase::AddPickModel || phase_ == Phase::DetailPickModel) {
    if (key == "\x1b[A" && modelCursor_ > 0) { --modelCursor_; return true; }
    if (key == "\x1b[B" && modelCursor_ + 1 < static_cast<int>(filteredIndices_.size())) { ++modelCursor_; return true; }
    if (key == "\r" || key == "\n") {
      if (filteredIndices_.empty()) { message_ = "No model selected."; return true; }
      const auto& e = allModels_[filteredIndices_[modelCursor_]];
      firmius::shared::ModelOption opt{e.providerId, e.modelId, e.variantName};
      if (phase_ == Phase::AddPickModel) {
        firmius::shared::ModelRouteCategory cat;
        cat.models.push_back(opt);
        categories_[inputBuf_] = cat;
        sortedNames_.clear();
        for (const auto& [n, _] : categories_) sortedNames_.push_back(n);
        std::sort(sortedNames_.begin(), sortedNames_.end());
        message_ = "Added category '" + inputBuf_ + "'.";
      } else {
        categories_[selectedCategory()].models.push_back(opt);
        message_ = "Added model.";
      }
      save();
      phase_ = (phase_ == Phase::AddPickModel) ? Phase::Browse : Phase::Detail;
      return true;
    }
    if (key == "\x7f" || key == "\b") {
      if (!modelFilter_.empty()) { modelFilter_.pop_back(); modelCursor_ = 0; rebuildFilteredModels(); }
      return true;
    }
    if (key.size() == 1 && key[0] >= 32) {
      modelFilter_ += key; modelCursor_ = 0; rebuildFilteredModels();
      return true;
    }
    return true;
  }

  // ── ConfirmDelete ──
  if (phase_ == Phase::ConfirmDelete) {
    if (key == "y" || key == "Y") {
      const std::string cat = selectedCategory();
      categories_.erase(cat);
      if (defaultRoute_ == cat) defaultRoute_.clear();
      fallbackOrder_.erase(std::remove(fallbackOrder_.begin(), fallbackOrder_.end(), cat), fallbackOrder_.end());
      sortedNames_.clear();
      for (const auto& [n, _] : categories_) sortedNames_.push_back(n);
      std::sort(sortedNames_.begin(), sortedNames_.end());
      if (cursor_ >= static_cast<int>(sortedNames_.size()))
        cursor_ = std::max(0, static_cast<int>(sortedNames_.size()) - 1);
      message_ = "Deleted '" + cat + "'.";
      save();
    }
    phase_ = Phase::Browse;
    return true;
  }

  // ── ConfirmDeleteModel ──
  if (phase_ == Phase::ConfirmDeleteModel) {
    if (key == "y" || key == "Y") {
      const std::string cat = selectedCategory();
      auto& models = categories_[cat].models;
      if (detailCursor_ >= 0 && detailCursor_ < static_cast<int>(models.size())) {
        models.erase(models.begin() + detailCursor_);
        if (detailCursor_ >= static_cast<int>(models.size()))
          detailCursor_ = std::max(0, static_cast<int>(models.size()) - 1);
        message_ = "Removed model.";
        save();
      }
    }
    phase_ = Phase::Detail;
    return true;
  }

  // ── FallbackOrder ──
  if (phase_ == Phase::FallbackOrder) {
    const int count = static_cast<int>(fallbackOrder_.size());
    if (key == "\x1b[A") {
      if (fallbackGrabbed_ && fallbackCursor_ > 0) {
        std::swap(fallbackOrder_[fallbackCursor_], fallbackOrder_[fallbackCursor_ - 1]);
        --fallbackCursor_;
        save();
      } else if (!fallbackGrabbed_ && fallbackCursor_ > 0) {
        --fallbackCursor_;
      }
      return true;
    }
    if (key == "\x1b[B") {
      if (fallbackGrabbed_ && fallbackCursor_ + 1 < count) {
        std::swap(fallbackOrder_[fallbackCursor_], fallbackOrder_[fallbackCursor_ + 1]);
        ++fallbackCursor_;
        save();
      } else if (!fallbackGrabbed_ && fallbackCursor_ + 1 < count) {
        ++fallbackCursor_;
      }
      return true;
    }
    if (key == " ") {
      fallbackGrabbed_ = !fallbackGrabbed_;
      return true;
    }
    if (key == "a" || key == "A") {
      // Add a category to fallback order (pick from those not already in it)
      for (const auto& name : sortedNames_) {
        if (std::find(fallbackOrder_.begin(), fallbackOrder_.end(), name) == fallbackOrder_.end()) {
          fallbackOrder_.push_back(name);
          message_ = "Added '" + name + "' to fallback order.";
          save();
          break;
        }
      }
      return true;
    }
    if ((key == "d" || key == "D") && !fallbackOrder_.empty()) {
      fallbackOrder_.erase(fallbackOrder_.begin() + fallbackCursor_);
      if (fallbackCursor_ >= static_cast<int>(fallbackOrder_.size()))
        fallbackCursor_ = std::max(0, static_cast<int>(fallbackOrder_.size()) - 1);
      fallbackGrabbed_ = false;
      message_ = "Removed from fallback order.";
      save();
      return true;
    }
    if (key == "t" || key == "T") {
      fallbackEnabled_ = !fallbackEnabled_;
      message_ = fallbackEnabled_ ? "Fallback enabled." : "Fallback disabled.";
      save();
      return true;
    }
    return true;
  }

  return true;
}

} // namespace firmius::tui
