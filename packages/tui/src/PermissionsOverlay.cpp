
#include "PermissionsOverlay.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <algorithm>
#include <sstream>

namespace firmius::tui {

namespace {

std::string truncate(const std::string& s, int w) {
  if (w <= 0) return "";
  if (static_cast<int>(s.size()) <= w) return s;
  if (w <= 1) return s.substr(0, w);
  return s.substr(0, w - 1) + "…";
}

const char *categoryIcon(const std::string &c) {
  if (c == "process.exec")    return "⚙";
  if (c == "process.cwd")     return "📁";
  if (c == "file.read")       return "📖";
  if (c == "file.write")      return "✏";
  if (c == "file.create")     return "✨";
  if (c == "file.delete")     return "🗑";
  if (c == "network.fetch")   return "🌐";
  if (c == "network.search")  return "🔍";
  if (c == "agent.spawn")     return "👥";
  if (c == "artifact.write")  return "📦";
  return "•";
}

} // namespace

void PermissionsOverlay::seedRules(
    firmius::daemon::PermissionListRulesResponse snapshot) {
  rules_ = std::move(snapshot);
  ruleCursor_ = 0;
  ruleScrollOffset_ = 0;
}

void PermissionsOverlay::seedModes(
    std::vector<firmius::daemon::PermissionModeWire> modes,
    std::string activeModeId) {
  modes_ = std::move(modes);
  activeModeId_ = std::move(activeModeId);
  if (modeCursor_ >= static_cast<int>(modes_.size())) {
    modeCursor_ = std::max(0, static_cast<int>(modes_.size()) - 1);
  }
}

void PermissionsOverlay::open() {
  isOpen_ = true;
  tab_ = Tab::Modes;
  phase_ = Phase::Browse;
  message_.clear();
  inputBuf_.clear();
}

void PermissionsOverlay::close() {
  isOpen_ = false;
  rules_ = {};
  modes_.clear();
  activeModeId_.clear();
  message_.clear();
  inputBuf_.clear();
  modeCursor_ = 0;
  ruleCursor_ = 0;
  ruleScrollOffset_ = 0;
}

int PermissionsOverlay::height(int) const {
  if (!isOpen_) return 0;
  // tabbar(2) + content(varies) + footer(3). Cap at a sensible limit
  // so the overlay doesn't crowd the transcript.
  if (tab_ == Tab::Modes) {
    return 2 + std::min(static_cast<int>(modes_.size()), 8) + 4;
  }
  return 2 + std::min(static_cast<int>(rules_.rules.size()), kMaxVisible) + 4;
}

std::vector<std::string> PermissionsOverlay::renderTabBar(int width) const {
  std::vector<std::string> out;
  std::string tabs;
  auto tab = [&](const std::string &name, bool active) {
    return active ? theme_ansi::accent(ansi::bold(" " + name + " "))
                  : theme_ansi::dim(" " + name + " ");
  };
  tabs = tab("Modes", tab_ == Tab::Modes) + theme_ansi::dim("│") +
         tab("Rules", tab_ == Tab::Rules);
  out.push_back(ansi::fitToWidth(
      theme_ansi::accent(ansi::bold(" /permissions ")) + "  " + tabs, width));
  out.push_back(theme_ansi::divider(width));
  return out;
}

std::vector<std::string> PermissionsOverlay::renderModes(int width) const {
  std::vector<std::string> out;
  if (modes_.empty()) {
    out.push_back(ansi::fitToWidth(
        "  " + theme_ansi::dim("No modes configured."), width));
    return out;
  }
  for (int i = 0; i < static_cast<int>(modes_.size()); ++i) {
    const auto &m = modes_[i];
    const bool selected = (i == modeCursor_);
    const bool active = (m.id == activeModeId_);
    std::string marker = selected ? theme_ansi::accent("> ") : "  ";
    std::string activeBadge = active ? theme_ansi::success(" ●") : "  ";
    std::string builtIn = m.builtIn ? theme_ansi::dim(" (built-in)") : "";
    std::string body = m.name + builtIn;
    if (!m.description.empty()) {
      body += "  " + theme_ansi::dim(truncate(m.description, width / 2));
    }
    std::string line = "  " + marker + theme_ansi::foreground(body) + activeBadge;
    std::string fitted = ansi::fitToWidth(line, width);
    out.push_back(selected ? theme_ansi::selection(fitted) : fitted);
  }
  return out;
}

std::string PermissionsOverlay::formatMatch(
    const firmius::daemon::PolicyRuleWire& rule) const {
  if (rule.match.empty()) return "(any)";
  std::ostringstream oss;
  bool first = true;
  for (const auto &[k, v] : rule.match) {
    if (!first) oss << " · ";
    first = false;
    oss << k << "=" << v;
  }
  return oss.str();
}

std::string PermissionsOverlay::renderRuleRow(
    const firmius::daemon::PolicyRuleWire& rule,
    bool selected, int width) const {
  const std::string marker = selected ? theme_ansi::accent("> ") : "  ";
  const std::string deciTag = rule.decision == "allow"
      ? theme_ansi::success("[allow]")
      : (rule.decision == "deny" ? theme_ansi::error("[deny] ")
                                  : theme_ansi::dim("[ask]  "));
  const std::string scopeTag = " " + theme_ansi::dim("(" + rule.scope + ")");
  const std::string body = std::string(categoryIcon(rule.category)) + " " +
                           rule.category + " " +
                           formatMatch(rule);
  std::string line = "  " + marker + deciTag + " " +
                     theme_ansi::foreground(body) + scopeTag;
  std::string fitted = ansi::fitToWidth(truncate(line, width * 4), width);
  return selected ? theme_ansi::selection(fitted) : fitted;
}

std::vector<std::string> PermissionsOverlay::renderRules(int width) const {
  std::vector<std::string> out;
  if (rules_.rules.empty()) {
    out.push_back(ansi::fitToWidth(
        "  " + theme_ansi::dim("No rules. Approve a request with 'Allow always' to add one."),
        width));
    return out;
  }
  const int total = static_cast<int>(rules_.rules.size());
  const int show = std::min(total, kMaxVisible);
  int off = std::max(0, std::min(ruleScrollOffset_, total - show));
  for (int i = 0; i < show; ++i) {
    const int idx = off + i;
    if (idx >= total) break;
    out.push_back(renderRuleRow(rules_.rules[idx], idx == ruleCursor_, width));
  }
  return out;
}

std::vector<std::string> PermissionsOverlay::render(int width) const {
  std::vector<std::string> lines;
  if (!isOpen_) return lines;

  for (auto &l : renderTabBar(width)) lines.push_back(std::move(l));

  if (tab_ == Tab::Modes) {
    for (auto &l : renderModes(width)) lines.push_back(std::move(l));
  } else {
    for (auto &l : renderRules(width)) lines.push_back(std::move(l));
  }

  if (phase_ == Phase::PromptCreateName) {
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::accent("New mode name: ") +
        theme_ansi::foreground(inputBuf_) + theme_ansi::dim("▌"), width));
  }
  if (phase_ == Phase::PromptRenameName) {
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::accent("Rename to: ") +
        theme_ansi::foreground(inputBuf_) + theme_ansi::dim("▌"), width));
  }
  if (phase_ == Phase::ConfirmDeleteMode &&
      modeCursor_ >= 0 && modeCursor_ < static_cast<int>(modes_.size())) {
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::error("Delete mode '" + modes_[modeCursor_].name +
                                  "'? [y/N]"), width));
  }
  if (phase_ == Phase::ConfirmDeleteRule &&
      ruleCursor_ >= 0 && ruleCursor_ < static_cast<int>(rules_.rules.size())) {
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::error("Delete rule '" +
                                   rules_.rules[ruleCursor_].id + "'? [y/N]"),
        width));
  }

  if (!message_.empty()) {
    lines.push_back(ansi::fitToWidth(
        "  " + theme_ansi::dim(message_), width));
  }

  lines.push_back(ansi::fitToWidth("", width));
  if (tab_ == Tab::Modes) {
    lines.push_back(ansi::fitToWidth(
        theme_ansi::dim(" ↑↓ nav · Enter activate · A add · R rename · D delete · Tab→Rules · Esc close "),
        width));
  } else {
    lines.push_back(ansi::fitToWidth(
        theme_ansi::dim(" ↑↓ nav · D delete · Tab→Modes · Esc close "), width));
  }
  return lines;
}

bool PermissionsOverlay::handleInput(const std::string& key) {
  if (!isOpen_) return false;

  // ── Modal phases first ──
  if (phase_ == Phase::PromptCreateName ||
      phase_ == Phase::PromptRenameName) {
    if (key == "\x1b") {
      phase_ = Phase::Browse;
      inputBuf_.clear();
      message_.clear();
      return true;
    }
    if (key == "\r" || key == "\n") {
      if (inputBuf_.empty()) {
        message_ = "Name cannot be empty.";
        return true;
      }
      if (phase_ == Phase::PromptCreateName) {
        if (onCreate_) {
          auto id = onCreate_(inputBuf_, /*seedFromActive=*/false);
          if (id.empty()) message_ = "Create failed (duplicate name?).";
          else            message_ = "Created '" + inputBuf_ + "'.";
        }
      } else {
        if (modeCursor_ < 0 ||
            modeCursor_ >= static_cast<int>(modes_.size())) {
          phase_ = Phase::Browse;
          return true;
        }
        const auto id = modes_[modeCursor_].id;
        if (onRename_) {
          if (onRename_(id, inputBuf_)) message_ = "Renamed.";
          else                          message_ = "Rename failed.";
        }
      }
      inputBuf_.clear();
      phase_ = Phase::Browse;
      return true;
    }
    if (key == "\x7f" || key == "\b") {
      if (!inputBuf_.empty()) inputBuf_.pop_back();
      return true;
    }
    if (key.size() == 1 && key[0] >= 32) { inputBuf_ += key; return true; }
    return true;
  }

  if (phase_ == Phase::ConfirmDeleteMode) {
    if (key == "y" || key == "Y") {
      if (modeCursor_ >= 0 && modeCursor_ < static_cast<int>(modes_.size())) {
        const auto id = modes_[modeCursor_].id;
        if (onDeleteMode_ && onDeleteMode_(id)) {
          modes_.erase(modes_.begin() + modeCursor_);
          if (modeCursor_ >= static_cast<int>(modes_.size()))
            modeCursor_ = std::max(0, static_cast<int>(modes_.size()) - 1);
          message_ = "Removed mode.";
        } else {
          message_ = "Cannot delete (built-in or active).";
        }
      }
    }
    phase_ = Phase::Browse;
    return true;
  }

  if (phase_ == Phase::ConfirmDeleteRule) {
    if (key == "y" || key == "Y") {
      if (ruleCursor_ >= 0 &&
          ruleCursor_ < static_cast<int>(rules_.rules.size())) {
        const auto id = rules_.rules[ruleCursor_].id;
        if (onDeleteRule_ && onDeleteRule_(id)) {
          rules_.rules.erase(rules_.rules.begin() + ruleCursor_);
          if (ruleCursor_ >= static_cast<int>(rules_.rules.size()))
            ruleCursor_ = std::max(0,
                static_cast<int>(rules_.rules.size()) - 1);
          message_ = "Removed rule.";
        } else {
          message_ = "Failed to remove rule.";
        }
      }
    }
    phase_ = Phase::Browse;
    return true;
  }

  // ── Top-level browse ──
  if (key == "\x1b") {
    if (onDismiss_) onDismiss_();
    return true;
  }
  if (key == "\t") {
    tab_ = (tab_ == Tab::Modes ? Tab::Rules : Tab::Modes);
    message_.clear();
    return true;
  }
  if (key == "r" || key == "R") {
    if (onReload_ && onReload_()) message_ = "Policy reloaded.";
    return true;
  }

  if (tab_ == Tab::Modes) {
    if (key == "\x1b[A" && modeCursor_ > 0) { --modeCursor_; return true; }
    if (key == "\x1b[B" && modeCursor_ + 1 < static_cast<int>(modes_.size())) {
      ++modeCursor_;
      return true;
    }
    if (key == "\r" || key == "\n") {
      if (modeCursor_ >= 0 &&
          modeCursor_ < static_cast<int>(modes_.size())) {
        const auto id = modes_[modeCursor_].id;
        if (onSetActive_ && onSetActive_(id)) {
          activeModeId_ = id;
          message_ = "Active mode: " + modes_[modeCursor_].name;
        }
      }
      return true;
    }
    if (key == "a" || key == "A") {
      phase_ = Phase::PromptCreateName;
      inputBuf_.clear();
      return true;
    }
    if ((key == "r" || key == "R") || key == "n") { /* handled above */ }
    if ((key == "n" || key == "N")) {
      // capital R is reload; rename uses lowercase r... wait we used r
      // for reload above, so use Shift+R to rename. Simpler: use 'e'
      // for "edit name". Already mapped: A/D are clear; rename uses 'r'
      // but it conflicts with reload. Disambiguate by checking if a
      // mode is selected: 'r' renames if selected. Let's keep 'r' as
      // reload and use 'e' for rename.
    }
    if (key == "e" || key == "E") {
      if (modeCursor_ >= 0 &&
          modeCursor_ < static_cast<int>(modes_.size())) {
        const auto &m = modes_[modeCursor_];
        if (m.builtIn) {
          message_ = "Built-in modes can't be renamed.";
          return true;
        }
        phase_ = Phase::PromptRenameName;
        inputBuf_ = m.name;
      }
      return true;
    }
    if ((key == "d" || key == "D") &&
        !modes_.empty() && modeCursor_ >= 0 &&
        modeCursor_ < static_cast<int>(modes_.size())) {
      if (modes_[modeCursor_].builtIn) {
        message_ = "Built-in modes can't be deleted.";
        return true;
      }
      if (modes_[modeCursor_].id == activeModeId_) {
        message_ = "Switch to another mode first.";
        return true;
      }
      phase_ = Phase::ConfirmDeleteMode;
      return true;
    }
    return true;
  }

  // Rules tab
  if (key == "\x1b[A" && ruleCursor_ > 0) {
    --ruleCursor_;
    if (ruleCursor_ < ruleScrollOffset_) ruleScrollOffset_ = ruleCursor_;
    return true;
  }
  if (key == "\x1b[B" &&
      ruleCursor_ + 1 < static_cast<int>(rules_.rules.size())) {
    ++ruleCursor_;
    if (ruleCursor_ >= ruleScrollOffset_ + kMaxVisible) {
      ruleScrollOffset_ = ruleCursor_ - kMaxVisible + 1;
    }
    return true;
  }
  if ((key == "d" || key == "D") && !rules_.rules.empty()) {
    phase_ = Phase::ConfirmDeleteRule;
    return true;
  }
  return true;
}

} // namespace firmius::tui
