
#include "PermissionPromptOverlay.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <algorithm>

namespace firmius::tui2 {

namespace {

std::string truncate(const std::string& s, int w) {
  if (w <= 0) return "";
  if (static_cast<int>(s.size()) <= w) return s;
  if (w <= 1) return s.substr(0, w);
  return s.substr(0, w - 1) + "…";
}

const char *categoryLabel(const std::string &c) {
  if (c == "process.exec")    return "Run command";
  if (c == "process.cwd")     return "Use working dir";
  if (c == "file.read")       return "Read file";
  if (c == "file.write")      return "Write file";
  if (c == "file.create")     return "Create file";
  if (c == "file.delete")     return "Delete file";
  if (c == "network.fetch")   return "Network fetch";
  if (c == "network.search")  return "Web search";
  if (c == "agent.spawn")     return "Spawn subagent";
  if (c == "artifact.write")  return "Write artifact";
  return "Permission";
}

} // namespace

void PermissionPromptOverlay::setPermission(PendingPermission perm,
                                              int queueIndex, int queueSize,
                                              std::string nextHint) {
  perm_ = std::move(perm);
  phase_ = Phase::Action;
  actionCursor_ = 0;
  suggestionCursor_ = 0;
  suggestionSelected_.assign(perm_.suggestions.size(), false);
  for (size_t i = 0; i < perm_.suggestions.size(); ++i) {
    suggestionSelected_[i] = perm_.suggestions[i].defaultSelected;
  }
  queueIndex_ = queueIndex < 1 ? 1 : queueIndex;
  queueSize_ = queueSize < queueIndex_ ? queueIndex_ : queueSize;
  nextHint_ = std::move(nextHint);
}

void PermissionPromptOverlay::open() {
  isOpen_ = true;
  phase_ = Phase::Action;
  actionCursor_ = 0;
}

void PermissionPromptOverlay::close() {
  isOpen_ = false;
  perm_ = {};
  suggestionSelected_.clear();
}

int PermissionPromptOverlay::height(int /*width*/) const {
  if (!isOpen_) return 0;
  if (phase_ == Phase::Action) {
    int detailLines = std::max(2, static_cast<int>(perm_.suggestions.size()));
    (void)detailLines;
    // header(3) + summary(2) + detail(<=6) + actions(5) + footer(2)
    return 18;
  }
  // Suggestions phase
  int suggestLines =
      std::max(1, static_cast<int>(perm_.suggestions.size()));
  return 5 + suggestLines + 4;
}

std::string PermissionPromptOverlay::severityBadge() const {
  // 0=LOW 1=MEDIUM 2=HIGH 3=VULNERABLE — matches CommandSeverity enum order.
  switch (perm_.severity) {
  case 3: return theme_ansi::error(" VULNERABLE ");
  case 2: return theme_ansi::error(" HIGH ");
  case 1: return theme_ansi::warning(" MEDIUM ");
  default: return theme_ansi::dim(" LOW ");
  }
}

std::vector<std::string> PermissionPromptOverlay::renderHeader(int width) const {
  std::vector<std::string> out;
  std::string title = " ";
  title += categoryLabel(perm_.category);
  title += " — Permission ";

  // Queue badge — looks like a tab indicator. Always present so the
  // user can see whether they're 1-of-1 or 1-of-many; hides when only
  // one is pending to keep the chrome quiet for the common case.
  std::string queueBadge;
  if (queueSize_ > 1) {
    queueBadge = " " + theme_ansi::accent(
        "[" + std::to_string(queueIndex_) + "/" +
        std::to_string(queueSize_) + "]") + " ";
  }

  out.push_back(ansi::fitToWidth(
      theme_ansi::accent(ansi::bold(title)) + queueBadge +
          "  " + severityBadge(),
      width));
  out.push_back(theme_ansi::divider(width));
  // Dimmed next-up preview: gives the user a glance of what they'll
  // see after this resolution, like a tab strip's active+next pair.
  if (queueSize_ > 1 && !nextHint_.empty()) {
    out.push_back(ansi::fitToWidth(
        "  " + theme_ansi::dim("next: " + truncate(nextHint_, width - 10)),
        width));
  }
  return out;
}

std::string PermissionPromptOverlay::formatRequestSummary() const {
  if (perm_.category == "process.exec" && !perm_.command.empty())
    return perm_.command;
  if (!perm_.targetPath.empty())
    return perm_.targetPath;
  if (!perm_.url.empty())
    return perm_.url;
  if (!perm_.persona.empty())
    return "spawn '" + perm_.persona + "'";
  if (!perm_.query.empty())
    return "search: " + perm_.query;
  return perm_.message;
}

std::vector<std::string> PermissionPromptOverlay::formatRequestDetail(int width) const {
  std::vector<std::string> out;
  auto addDetail = [&](const std::string &label, const std::string &value) {
    if (value.empty()) return;
    std::string line = "  " + theme_ansi::dim(label + ": ") +
                       theme_ansi::foreground(truncate(value, width - 4 -
                                                                static_cast<int>(label.size()) - 2));
    out.push_back(ansi::fitToWidth(line, width));
  };
  if (perm_.category == "process.exec") {
    addDetail("command", perm_.command);
    addDetail("cwd", perm_.cwd);
    if (!perm_.subcommands.empty() && perm_.subcommands.size() > 1) {
      std::string subs;
      for (size_t i = 0; i < perm_.subcommands.size() && i < 4; ++i) {
        if (i) subs += " | ";
        subs += perm_.subcommands[i];
      }
      addDetail("parts", subs);
    }
    addDetail("agent", perm_.toolName);
  } else if (perm_.category == "process.cwd") {
    addDetail("cwd", perm_.cwd);
  } else if (perm_.category.rfind("file.", 0) == 0) {
    addDetail("path", perm_.targetPath);
    addDetail("kind", perm_.isDirectory ? "directory" : "file");
    addDetail("tool", perm_.toolName);
  } else if (perm_.category == "network.fetch") {
    addDetail("url", perm_.url);
    addDetail("host", perm_.host);
    addDetail("scheme", perm_.scheme);
  } else if (perm_.category == "network.search") {
    addDetail("query", perm_.query);
  } else if (perm_.category == "agent.spawn") {
    addDetail("persona", perm_.persona);
    addDetail("parent", perm_.parentPersona);
  }
  return out;
}

std::vector<std::string> PermissionPromptOverlay::renderActionPhase(int width) const {
  std::vector<std::string> out;
  // Big summary line
  std::string summary = formatRequestSummary();
  out.push_back(ansi::fitToWidth(
      "  " + theme_ansi::foreground(ansi::bold(truncate(summary, width - 4))),
      width));
  out.push_back(ansi::fitToWidth("", width));

  // Detail block
  for (auto &line : formatRequestDetail(width)) {
    out.push_back(line);
  }

  out.push_back(ansi::fitToWidth("", width));
  out.push_back(ansi::fitToWidth(theme_ansi::divider(width), width));

  // Action buttons (radio-style)
  struct Action { const char *label; const char *hint; };
  const Action items[] = {
      {"Allow once",        "approve this single operation"},
      {"Allow always",      "configure rules and approve forever"},
      {"Deny",              "block this operation"},
  };
  for (int i = 0; i < 3; ++i) {
    bool sel = (i == actionCursor_);
    std::string marker = sel ? theme_ansi::accent("> ") : "  ";
    std::string keyHint = std::string(" [") + (i == 0 ? "y" : (i == 1 ? "a" : "n")) + "]";
    std::string body = items[i].label + std::string("  ") +
                        theme_ansi::dim(items[i].hint);
    std::string line = "  " + marker + theme_ansi::foreground(body) +
                        theme_ansi::dim(keyHint);
    std::string fitted = ansi::fitToWidth(line, width);
    out.push_back(sel ? theme_ansi::selection(fitted) : fitted);
  }
  out.push_back(ansi::fitToWidth("", width));
  out.push_back(ansi::fitToWidth(
      theme_ansi::dim(" ↑↓ pick · y allow once · a allow always · n deny · Esc dismiss "),
      width));
  return out;
}

std::vector<std::string> PermissionPromptOverlay::renderSuggestionPhase(int width) const {
  std::vector<std::string> out;
  out.push_back(ansi::fitToWidth(
      theme_ansi::accent(ansi::bold(" Customize allow rules ")), width));
  out.push_back(theme_ansi::divider(width));
  if (perm_.suggestions.empty()) {
    out.push_back(ansi::fitToWidth(
        "  " + theme_ansi::dim("No tailored suggestions available."), width));
    out.push_back(ansi::fitToWidth(
        "  " + theme_ansi::dim("Press Enter to apply nothing and allow once."),
        width));
  } else {
    for (int i = 0; i < static_cast<int>(perm_.suggestions.size()); ++i) {
      const auto &s = perm_.suggestions[i];
      bool sel = (i == suggestionCursor_);
      bool checked = i < static_cast<int>(suggestionSelected_.size())
                         ? suggestionSelected_[i] : false;
      std::string mark = checked ? theme_ansi::accent("[x] ") :
                                     theme_ansi::dim("[ ] ");
      std::string scopeBadge = " [" + s.scope + "]";
      std::string line = "  " + (sel ? theme_ansi::accent("> ") : std::string("  ")) +
                          mark + theme_ansi::foreground(s.label) +
                          theme_ansi::dim(scopeBadge);
      std::string fitted = ansi::fitToWidth(line, width);
      out.push_back(sel ? theme_ansi::selection(fitted) : fitted);
      if (sel && !s.explanation.empty()) {
        out.push_back(ansi::fitToWidth(
            "      " + theme_ansi::dim(truncate(s.explanation, width - 6)),
            width));
      }
    }
  }
  out.push_back(ansi::fitToWidth("", width));
  out.push_back(ansi::fitToWidth(
      theme_ansi::dim(" ↑↓ nav · Space toggle · Enter apply & allow · Esc back "),
      width));
  return out;
}

std::vector<std::string> PermissionPromptOverlay::render(int width) const {
  std::vector<std::string> lines;
  if (!isOpen_) return lines;
  for (auto &l : renderHeader(width)) lines.push_back(std::move(l));
  if (phase_ == Phase::Action) {
    for (auto &l : renderActionPhase(width)) lines.push_back(std::move(l));
  } else {
    for (auto &l : renderSuggestionPhase(width)) lines.push_back(std::move(l));
  }
  return lines;
}

bool PermissionPromptOverlay::handleInput(const std::string& key) {
  if (!isOpen_) return false;

  if (key == "\x1b") {
    if (phase_ == Phase::Suggestions) {
      phase_ = Phase::Action;
      return true;
    }
    if (onDismiss_) onDismiss_();
    return true;
  }

  if (phase_ == Phase::Action) {
    if (key == "\x1b[A" && actionCursor_ > 0) { --actionCursor_; return true; }
    if (key == "\x1b[B" && actionCursor_ < 2) { ++actionCursor_; return true; }
    // Hotkeys
    if (key == "y" || key == "Y") {
      if (onAllowOnce_) onAllowOnce_(perm_.requestId);
      return true;
    }
    if (key == "n" || key == "N") {
      if (onDeny_) onDeny_(perm_.requestId);
      return true;
    }
    if (key == "a" || key == "A") {
      if (perm_.suggestions.empty()) {
        // No suggestions to pick from — fall through to old AllowAlways
        // behavior by sending an empty selection list.
        if (onAllowAlways_) onAllowAlways_(perm_.requestId, {});
        return true;
      }
      phase_ = Phase::Suggestions;
      return true;
    }
    if (key == "\r" || key == "\n") {
      if (actionCursor_ == 0 && onAllowOnce_) onAllowOnce_(perm_.requestId);
      else if (actionCursor_ == 1) {
        if (perm_.suggestions.empty()) {
          if (onAllowAlways_) onAllowAlways_(perm_.requestId, {});
        } else {
          phase_ = Phase::Suggestions;
        }
      } else if (actionCursor_ == 2 && onDeny_) onDeny_(perm_.requestId);
      return true;
    }
    return true;
  }

  // Suggestions phase
  if (key == "\x1b[A" && suggestionCursor_ > 0) { --suggestionCursor_; return true; }
  if (key == "\x1b[B" &&
      suggestionCursor_ + 1 < static_cast<int>(perm_.suggestions.size())) {
    ++suggestionCursor_;
    return true;
  }
  if (key == " ") {
    if (suggestionCursor_ >= 0 &&
        suggestionCursor_ < static_cast<int>(suggestionSelected_.size())) {
      suggestionSelected_[suggestionCursor_] =
          !suggestionSelected_[suggestionCursor_];
    }
    return true;
  }
  if (key == "\r" || key == "\n") {
    std::vector<std::string> picks;
    for (size_t i = 0; i < perm_.suggestions.size() &&
                       i < suggestionSelected_.size(); ++i) {
      if (suggestionSelected_[i]) {
        picks.push_back(perm_.suggestions[i].ruleId);
      }
    }
    if (onAllowAlways_) onAllowAlways_(perm_.requestId, picks);
    return true;
  }
  return true;
}

} // namespace firmius::tui2
