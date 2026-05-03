#include "tools/ModeSwitchToolPresentation.hpp"

#include "utils/ErrorCleaner.hpp"

#include <rapidjson/document.h>

#include <algorithm>
#include <string>

namespace firmius::tui {

using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

namespace {

ToolPresentationLifecycle DeriveLifecycle(const ToolCallView &view) {
  if (view.phase == ToolPhase::Preparing) {
    return ToolPresentationLifecycle::Preparing;
  }
  if (view.phase == ToolPhase::Called ||
      view.phase == ToolPhase::BackgroundRunning) {
    return ToolPresentationLifecycle::Running;
  }
  if (view.phase == ToolPhase::Error ||
      (view.phase == ToolPhase::Finished && !view.success)) {
    return ToolPresentationLifecycle::Error;
  }
  return ToolPresentationLifecycle::Success;
}

// Extract a string field from a parsed JSON object. Returns empty when
// missing or wrong type — by design the renderer just skips empty bits.
std::string getString(const rapidjson::Value &v, const char *key) {
  if (!v.IsObject() || !v.HasMember(key)) {
    return "";
  }
  const auto &field = v[key];
  return field.IsString() ? std::string(field.GetString()) : std::string{};
}

// Trim a stance / reason snippet to keep the row genuinely one-line on
// narrow terminals. We pick a budget that survives the typical
// "AgentName · ToolBlock prefix" lead-in without wrapping.
std::string truncateForOneLine(const std::string &s, std::size_t budget) {
  if (s.size() <= budget) {
    return s;
  }
  return s.substr(0, std::max<std::size_t>(budget, 1) - 1) + "…";
}

} // namespace

bool IsModeSwitchTool(const std::string &tool_name) {
  return tool_name == "ModeSwitch" || tool_name == "mode_switch";
}

ToolPresentation
BuildModeSwitchToolPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = DeriveLifecycle(view);
  // The whole point: a single inline status row, no fact card, no
  // expandable JSON blob.
  presentation.layout = ToolPresentationLayoutKind::InlineStatusRow;
  presentation.density = ToolPresentationDensity::OneLineSummary;
  presentation.subtitle = "mode";
  presentation.expandable = false;
  presentation.expanded = false;

  // Pre-call: we usually have args (the requested mode) but no result
  // yet. Show the destination so the user sees what's about to land.
  rapidjson::Document args_doc;
  args_doc.Parse(view.args.c_str());
  std::string requested;
  std::string reason;
  if (!args_doc.HasParseError() && args_doc.IsObject()) {
    requested = getString(args_doc, "name");
    reason = getString(args_doc, "reason");
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    // Errors deserve their own one-liner shape so the operator sees the
    // failure inline without expanding anything.
    presentation.title = "mode switch failed";
    presentation.error_text =
        firmius::shared::ErrorCleaner::clean(view.result);
    std::string headline = requested.empty()
                               ? "mode switch failed"
                               : "mode switch failed (" + requested + ")";
    presentation.compact_summary = headline;
    return presentation;
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing ||
      presentation.lifecycle == ToolPresentationLifecycle::Running) {
    const std::string target =
        requested.empty() ? std::string{"…"} : requested;
    presentation.title = "→ " + target;
    presentation.compact_summary = presentation.title;
    return presentation;
  }

  // Success: parse the tool's structured result for from_mode, to_mode,
  // stance, cleared. Compose `from → to · stance`. When clearing, render
  // `<from> → (cleared)`. Reason (if the agent supplied one) wins over
  // stance in the trailing slot since the operator wrote it themselves.
  rapidjson::Document res_doc;
  res_doc.Parse(view.result.c_str());

  std::string fromMode;
  std::string toMode;
  std::string stance;
  bool cleared = false;
  if (!res_doc.HasParseError() && res_doc.IsObject()) {
    fromMode = getString(res_doc, "from_mode");
    toMode = getString(res_doc, "to_mode");
    stance = getString(res_doc, "stance");
    if (res_doc.HasMember("cleared") && res_doc["cleared"].IsBool()) {
      cleared = res_doc["cleared"].GetBool();
    }
  }

  std::string fromLabel = fromMode.empty() ? std::string{"·"} : fromMode;
  std::string toLabel = cleared || toMode.empty() ? std::string{"(cleared)"}
                                                  : toMode;

  std::string headline = fromLabel + " → " + toLabel;
  std::string trailing = !reason.empty() ? reason : stance;
  if (!trailing.empty()) {
    headline += " · " + truncateForOneLine(trailing, /*budget=*/72);
  }

  presentation.title = headline;
  presentation.compact_summary = headline;
  return presentation;
}

} // namespace firmius::tui
