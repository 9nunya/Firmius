#include "tools/SubagentToolPresentation.hpp"

#include "utils/ErrorCleaner.hpp"
#include "utils/ToolSummaries.hpp"
#include <algorithm>
#include <rapidjson/document.h>
#include <sstream>

namespace firmius::tui {

using firmius::shared::SummarizeToolCall;
using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

namespace {

struct ParsedSubagentArgs {
  std::string agent_id;
  std::string name;
  std::string title;
  std::string task;
  std::string category;
};

struct ParsedSubagentResult {
  std::string agent_id;
  std::string status;
  std::string result;
  std::string error;
  bool fallback_used = false;
  std::string route_category;
  std::vector<std::string> attempted_categories;
  std::vector<std::string> artifacts_created;
  std::vector<std::string> artifacts_updated;
};

bool IsMatch(const std::string &actual, const std::string &expected) {
  if (actual.empty() || expected.empty()) {
    return false;
  }
  return actual.find(expected) != std::string::npos;
}

ParsedSubagentArgs ParseArgs(const std::string &args) {
  ParsedSubagentArgs parsed;
  rapidjson::Document doc;
  doc.Parse(args.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return parsed;
  }
  if (doc.HasMember("agent_id") && doc["agent_id"].IsString()) {
    parsed.agent_id = doc["agent_id"].GetString();
  }
  if (doc.HasMember("name") && doc["name"].IsString()) {
    parsed.name = doc["name"].GetString();
  }
  if (doc.HasMember("title") && doc["title"].IsString()) {
    parsed.title = doc["title"].GetString();
  }
  if (doc.HasMember("task") && doc["task"].IsString()) {
    parsed.task = doc["task"].GetString();
  }
  if (doc.HasMember("category") && doc["category"].IsString()) {
    parsed.category = doc["category"].GetString();
  }
  return parsed;
}

std::vector<std::string> ParseArtifactArray(const rapidjson::Value &array) {
  std::vector<std::string> refs;
  if (!array.IsArray()) {
    return refs;
  }
  for (const auto &item : array.GetArray()) {
    if (item.IsString()) {
      refs.push_back(item.GetString());
      continue;
    }
    if (!item.IsObject()) {
      continue;
    }
    if (item.HasMember("reference") && item["reference"].IsString()) {
      refs.push_back(item["reference"].GetString());
      continue;
    }
    if (item.HasMember("filename") && item["filename"].IsString()) {
      refs.push_back(item["filename"].GetString());
    }
  }
  return refs;
}

ParsedSubagentResult ParseResult(const std::string &result) {
  ParsedSubagentResult parsed;
  rapidjson::Document doc;
  doc.Parse(result.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return parsed;
  }
  if (doc.HasMember("agentId") && doc["agentId"].IsString()) {
    parsed.agent_id = doc["agentId"].GetString();
  }
  if (doc.HasMember("status") && doc["status"].IsString()) {
    parsed.status = doc["status"].GetString();
  }
  if (doc.HasMember("result") && doc["result"].IsString()) {
    parsed.result = doc["result"].GetString();
  }
  if (doc.HasMember("error") && doc["error"].IsString()) {
    parsed.error = doc["error"].GetString();
  }
  if (doc.HasMember("fallback_used") && doc["fallback_used"].IsBool()) {
    parsed.fallback_used = doc["fallback_used"].GetBool();
  }
  if (doc.HasMember("category") && doc["category"].IsString()) {
    parsed.route_category = doc["category"].GetString();
  }
  if (doc.HasMember("attempted_categories") &&
      doc["attempted_categories"].IsArray()) {
    for (const auto &entry : doc["attempted_categories"].GetArray()) {
      if (entry.IsString()) {
        parsed.attempted_categories.push_back(entry.GetString());
      }
    }
  }
  if (doc.HasMember("artifacts_created")) {
    parsed.artifacts_created = ParseArtifactArray(doc["artifacts_created"]);
  }
  if (doc.HasMember("artifacts_updated")) {
    parsed.artifacts_updated = ParseArtifactArray(doc["artifacts_updated"]);
  }
  return parsed;
}

ToolPresentationLifecycle
DeriveLifecycle(const ToolCallView &view,
                const NormalizedSubagentState *state) {
  if (view.phase == ToolPhase::Preparing) {
    return ToolPresentationLifecycle::Preparing;
  }
  if (view.phase == ToolPhase::Called ||
      view.phase == ToolPhase::BackgroundRunning) {
    return ToolPresentationLifecycle::Running;
  }
  if (state && state->outcome == SubagentOutcomeKind::Failed) {
    return ToolPresentationLifecycle::Error;
  }
  if (view.phase == ToolPhase::Error ||
      (view.phase == ToolPhase::Finished && !view.success)) {
    return ToolPresentationLifecycle::Error;
  }
  return ToolPresentationLifecycle::Success;
}

std::string Join(const std::vector<std::string> &values, size_t limit = 5) {
  if (values.empty()) {
    return "";
  }
  std::string out;
  size_t count = 0;
  for (const auto &v : values) {
    if (v.empty()) {
      continue;
    }
    if (count > 0) {
      out += ", ";
    }
    out += v;
    count++;
    if (count >= limit && limit > 0) {
      break;
    }
  }
  return out;
}

std::string PreferredSubagentLabel(const std::string &friendly_name,
                                   const std::string &title,
                                   const std::string &agent_id) {
  if (!friendly_name.empty()) {
    return friendly_name;
  }
  if (!title.empty()) {
    return title;
  }
  return agent_id;
}

std::string CompactStateBadge(const std::string &state_label) {
  if (state_label == "completed_no_summary") {
    return "no summary";
  }
  return state_label;
}

std::string FirstNonEmptyLine(const std::string &text) {
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) {
      return line;
    }
  }
  return "";
}

std::string ClampPreviewText(const std::string &text, size_t max_chars = 96) {
  if (text.size() <= max_chars) {
    return text;
  }
  if (max_chars <= 3) {
    return text.substr(0, max_chars);
  }
  return text.substr(0, max_chars - 3) + "...";
}

std::vector<std::string> BuildCuratedActivityPreview(
    const std::vector<firmius::shared::SubagentToolLogEntry> &activity) {
  std::vector<std::string> lines;
  lines.reserve(activity.size());
  for (const auto &entry : activity) {
    if (entry.summary.empty()) {
      continue;
    }
    if (entry.summary.rfind("State: ", 0) == 0 || entry.summary == "Done") {
      continue;
    }
    const std::string preview = "recent: " + ClampPreviewText(entry.summary);
    if (!lines.empty() && lines.back() == preview) {
      continue;
    }
    lines.push_back(preview);
  }
  return lines;
}

std::optional<std::string>
BuildCuratedSummaryPreview(const std::string &final_summary) {
  const std::string first_line = FirstNonEmptyLine(final_summary);
  if (first_line.empty()) {
    return std::nullopt;
  }
  return "summary: " + ClampPreviewText(first_line);
}

std::string DeriveStateLabel(const ToolCallView &view,
                             const NormalizedSubagentState *state,
                             const ParsedSubagentResult &parsed_result) {
  if (state) {
    if (state->retrying) {
      return "retrying";
    }
    if (state->provider_waiting) {
      return "provider waiting";
    }
    if (state->account_switched) {
      return "account switched";
    }
    if (state->waiting) {
      return "waiting";
    }
    if (state->running) {
      return "running";
    }
    if (!state->wait_state.empty()) {
      return state->wait_state;
    }
  }
  if (!parsed_result.status.empty()) {
    return parsed_result.status;
  }
  if (view.phase == ToolPhase::Preparing) {
    return "preparing";
  }
  if (view.phase == ToolPhase::Called ||
      view.phase == ToolPhase::BackgroundRunning) {
    return "running";
  }
  if (view.phase == ToolPhase::Error || !view.success) {
    return "failed";
  }
  return "completed";
}

} // namespace

bool IsSubagentFamilyTool(const std::string &tool_name) {
  return IsMatch(tool_name, "summon_subagent") ||
         IsMatch(tool_name, "subagent_wait");
}

ToolPresentation
BuildSubagentToolPresentation(const ToolCallView &view,
                              const NormalizedSubagentState *subagent_state) {
  ToolPresentation presentation;
  presentation.lifecycle = DeriveLifecycle(view, subagent_state);
  const bool is_wait = IsMatch(view.name, "subagent_wait");
  const bool is_summon = IsMatch(view.name, "summon_subagent");
  presentation.layout = is_wait ? ToolPresentationLayoutKind::InlineStatusRow
                                : ToolPresentationLayoutKind::BodyFirstPreview;
  presentation.density = is_wait ? ToolPresentationDensity::OneLineSummary
                                 : ToolPresentationDensity::CompactSummaryCard;

  const ParsedSubagentArgs args = ParseArgs(view.args);
  const ParsedSubagentResult parsed_result = ParseResult(view.result);

  const std::string child_id =
      subagent_state && !subagent_state->child_agent_id.empty()
          ? subagent_state->child_agent_id
          : (!view.subagent_id.empty()
                 ? view.subagent_id
                 : (!parsed_result.agent_id.empty() ? parsed_result.agent_id
                                                    : args.agent_id));
  const std::string child_title =
      subagent_state && !subagent_state->child_title.empty()
          ? subagent_state->child_title
          : (!view.subagent_title.empty()
                 ? view.subagent_title
                 : (!args.title.empty() ? args.title : args.name));
  const std::string child_friendly_name =
      subagent_state && !subagent_state->child_friendly_name.empty()
          ? subagent_state->child_friendly_name
          : "";
  const std::string child_label =
      PreferredSubagentLabel(child_friendly_name, child_title, child_id);
  const std::string state_label =
      DeriveStateLabel(view, subagent_state, parsed_result);
  const std::string compact_state = CompactStateBadge(state_label);
  const bool fallback_used = subagent_state ? subagent_state->fallback_used
                                            : parsed_result.fallback_used;
  const std::string route_category =
      subagent_state && !subagent_state->route_category.empty()
          ? subagent_state->route_category
          : (!parsed_result.route_category.empty()
                 ? parsed_result.route_category
                 : args.category);
  const std::vector<std::string> attempted_categories =
      subagent_state && !subagent_state->attempted_categories.empty()
          ? subagent_state->attempted_categories
          : parsed_result.attempted_categories;
  const std::vector<std::string> artifacts_created =
      subagent_state && !subagent_state->artifacts_created.empty()
          ? subagent_state->artifacts_created
          : parsed_result.artifacts_created;
  const std::vector<std::string> artifacts_updated =
      subagent_state && !subagent_state->artifacts_updated.empty()
          ? subagent_state->artifacts_updated
          : parsed_result.artifacts_updated;

  if (is_summon) {
    presentation.title =
        !child_label.empty() ? child_label : "Summoning subagent..";
  } else {
    presentation.title =
        !child_label.empty() ? ("waiting " + child_label) : "waiting";
  }
  presentation.subtitle.clear();
  presentation.compact_summary.clear();
  if (!child_id.empty() && !is_wait) {
    presentation.footer_badges.push_back("id " + child_id);
    presentation.facts.push_back({"Agent ID", child_id});
  }
  if (!compact_state.empty()) {
    const bool redundant_wait_state =
        is_wait && (compact_state == "waiting" || compact_state == "running");
    if (!redundant_wait_state) {
      presentation.footer_badges.push_back(compact_state);
    }
  }
  if (!is_wait && !route_category.empty() &&
      (fallback_used || view.show_result)) {
    presentation.footer_badges.push_back("route " + route_category);
    presentation.facts.push_back({"Route", route_category});
  }
  if (!is_wait && !attempted_categories.empty() && view.show_result) {
    presentation.footer_badges.push_back("attempts " +
                                         Join(attempted_categories, 0));
    presentation.facts.push_back({"Attempts", Join(attempted_categories, 0)});
  }
  if (fallback_used) {
    ToolPresentationNotice notice;
    notice.kind = ToolPresentationNoticeKind::Warning;
    notice.text = "Fallback route used";
    presentation.notices.push_back(std::move(notice));
  }
  if (!artifacts_created.empty()) {
    presentation.footer_badges.push_back(
        "+" + std::to_string(artifacts_created.size()) + " artifact(s)");
  }
  if (!artifacts_updated.empty()) {
    presentation.footer_badges.push_back(
        "~" + std::to_string(artifacts_updated.size()) + " artifact(s)");
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    std::string error = subagent_state ? subagent_state->error_text : "";
    if (error.empty()) {
      error = !parsed_result.error.empty() ? parsed_result.error : view.result;
    }
    error = firmius::shared::ErrorCleaner::clean(error);
    if (!error.empty()) {
      presentation.error_text = error;
    }
    if (presentation.title.find("failed") == std::string::npos) {
      presentation.title += " failed";
    }
  }

  const std::string final_summary =
      subagent_state && !subagent_state->final_summary.empty()
          ? subagent_state->final_summary
          : (!parsed_result.result.empty() ? parsed_result.result
                                           : view.subagent_wait_message);
  const size_t collapsed_lines = 2;
  const size_t expanded_lines = 4;
  const size_t max_lines = view.show_result ? expanded_lines : collapsed_lines;
  if (is_wait) {
    presentation.body_lines.clear();
    presentation.expandable = false;
  }

  if (state_label == "completed_no_summary") {
    ToolPresentationNotice notice;
    notice.kind = ToolPresentationNoticeKind::Warning;
    notice.text = "Completed without usable summary";
    presentation.notices.push_back(std::move(notice));
  } else if (state_label == "cancelled") {
    ToolPresentationNotice notice;
    notice.kind = ToolPresentationNoticeKind::Warning;
    notice.text = "Subagent run was cancelled";
    presentation.notices.push_back(std::move(notice));
  }

  std::vector<firmius::shared::SubagentToolLogEntry> activity;
  if (subagent_state) {
    activity = subagent_state->activity_log;
  } else {
    activity = view.subagent_tool_log;
  }
  if (is_summon && !activity.empty()) {
    const std::vector<std::string> activity_lines =
        BuildCuratedActivityPreview(activity);
    if (!activity_lines.empty()) {
      const size_t show = std::min(max_lines, activity_lines.size());
      presentation.body_lines.assign(
          activity_lines.end() - static_cast<long>(show), activity_lines.end());
      if (activity_lines.size() > collapsed_lines) {
        presentation.expandable = true;
      }
    }
  }
  if (is_summon && presentation.body_lines.empty() &&
      presentation.lifecycle != ToolPresentationLifecycle::Preparing &&
      presentation.lifecycle != ToolPresentationLifecycle::Error) {
    const auto summary_line = BuildCuratedSummaryPreview(final_summary);
    if (summary_line.has_value()) {
      presentation.body_lines.push_back(*summary_line);
    }
  }
  presentation.expanded = !is_wait && view.show_result;
  return presentation;
}

} // namespace firmius::tui
