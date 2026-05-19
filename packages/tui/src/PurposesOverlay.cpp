
#include "PurposesOverlay.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <algorithm>

namespace firmius::tui {

void PurposesOverlay::seed(std::vector<std::string> purposes,
                           std::map<std::string, std::string> routes,
                           std::vector<std::string> categories) {
  purposes_ = std::move(purposes);
  routes_ = std::move(routes);
  categories_ = std::move(categories);
  cursor_ = 0;
  message_.clear();
}

void PurposesOverlay::open() { isOpen_ = true; message_.clear(); }
void PurposesOverlay::close() { isOpen_ = false; message_.clear(); }

void PurposesOverlay::save() {
  if (!onSave_) return;
  firmius::daemon::PurposesConfigUpdateRequest req;
  req.purposeRoutes = routes_;
  onSave_(req);
}

int PurposesOverlay::height(int) const {
  if (!isOpen_) return 0;
  return 4 + std::max(1, static_cast<int>(purposes_.size())) + 4;
}

std::vector<std::string> PurposesOverlay::render(int width) const {
  std::vector<std::string> lines;
  if (!isOpen_) return lines;

  lines.push_back(ansi::fitToWidth(
      theme_ansi::accent(ansi::bold(" Purposes — Persona Route Mapping ")), width));
  lines.push_back(theme_ansi::divider(width));
  lines.push_back(ansi::fitToWidth(
      "  " + theme_ansi::dim("←/→ cycle category for selected persona"), width));

  if (purposes_.empty()) {
    lines.push_back(ansi::fitToWidth("  " + theme_ansi::dim("No personas found."), width));
  } else {
    for (int i = 0; i < static_cast<int>(purposes_.size()); ++i) {
      const bool sel = (i == cursor_);
      const auto& p = purposes_[i];
      auto it = routes_.find(p);
      const std::string mapped = (it == routes_.end() || it->second.empty())
                                     ? "(none)" : it->second;
      std::string line = (sel ? "> " : "  ") + p + " → " + mapped;
      std::string fitted = ansi::fitToWidth("  " + theme_ansi::foreground(line), width);
      lines.push_back(sel ? theme_ansi::selection(fitted) : fitted);
    }
  }

  // Show available categories
  lines.push_back(ansi::fitToWidth("", width));
  if (categories_.empty()) {
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::dim("No categories defined. Use /router to create some."), width));
  } else {
    std::string catLine;
    for (size_t i = 0; i < categories_.size(); ++i) {
      if (i) catLine += ", ";
      catLine += categories_[i];
    }
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::dim("Categories: " + catLine), width));
  }

  if (!message_.empty())
    lines.push_back(ansi::fitToWidth("  " + theme_ansi::dim(message_), width));
  lines.push_back(ansi::fitToWidth("", width));
  lines.push_back(ansi::fitToWidth(
      theme_ansi::dim(" ↑↓ nav · ←/→ cycle · C clear · Esc close "), width));
  return lines;
}

bool PurposesOverlay::handleInput(const std::string& key) {
  if (!isOpen_) return false;

  if (key == "\x1b") {
    if (onDismiss_) onDismiss_();
    return true;
  }
  if (key == "\x1b[A" && cursor_ > 0) { --cursor_; return true; }
  if (key == "\x1b[B" && cursor_ + 1 < static_cast<int>(purposes_.size())) { ++cursor_; return true; }

  if (purposes_.empty() || cursor_ < 0 || cursor_ >= static_cast<int>(purposes_.size()))
    return true;

  const std::string& purpose = purposes_[cursor_];

  if ((key == "c" || key == "C")) {
    routes_.erase(purpose);
    message_ = "Cleared route for '" + purpose + "'.";
    save();
    return true;
  }

  if ((key == "\x1b[C" || key == "\x1b[D") && !categories_.empty()) {
    auto it = routes_.find(purpose);
    const std::string current = (it != routes_.end()) ? it->second : "";
    int idx = -1;
    for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
      if (categories_[i] == current) { idx = i; break; }
    }
    if (key == "\x1b[C") {
      idx = (idx + 1) % static_cast<int>(categories_.size());
    } else {
      idx = (idx <= 0) ? static_cast<int>(categories_.size()) - 1 : idx - 1;
    }
    routes_[purpose] = categories_[idx];
    message_ = "Mapped '" + purpose + "' → '" + categories_[idx] + "'.";
    save();
    return true;
  }

  return true;
}

} // namespace firmius::tui
