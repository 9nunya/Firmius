#include "tools/WorkToolPresentation.hpp"

#include "utils/ErrorCleaner.hpp"

#include <rapidjson/document.h>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace firmius::tui {

namespace {

using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

bool IsMatch(const std::string &actual, const std::string &expected) {
  return !actual.empty() && !expected.empty() &&
         actual.find(expected) != std::string::npos;
}

ToolPresentationLifecycle LifecycleFromPhase(const ToolCallView &view) {
  if (view.phase == ToolPhase::Preparing) {
    return ToolPresentationLifecycle::Preparing;
  }
  if (view.phase == ToolPhase::Called || view.phase == ToolPhase::BackgroundRunning) {
    return ToolPresentationLifecycle::Running;
  }
  if (view.phase == ToolPhase::Error ||
      (view.phase == ToolPhase::Finished && !view.success)) {
    return ToolPresentationLifecycle::Error;
  }
  return ToolPresentationLifecycle::Success;
}

bool ParseObject(const std::string &json, rapidjson::Document &doc) {
  doc.Parse(json.c_str());
  return !doc.HasParseError() && doc.IsObject();
}

bool ParseArray(const std::string &json, rapidjson::Document &doc) {
  doc.Parse(json.c_str());
  return !doc.HasParseError() && doc.IsArray();
}

std::string StringMember(const rapidjson::Value &value, const char *key) {
  if (value.IsObject() && value.HasMember(key) && value[key].IsString()) {
    return value[key].GetString();
  }
  return "";
}

std::vector<std::string> StringArrayMember(const rapidjson::Value &value, const char *key) {
  std::vector<std::string> values;
  if (!value.IsObject() || !value.HasMember(key) || !value[key].IsArray()) {
    return values;
  }
  for (const auto &item : value[key].GetArray()) {
    if (item.IsString()) {
      values.push_back(item.GetString());
    }
  }
  return values;
}

std::string UnescapeJsonString(std::string value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    const char ch = value[i];
    if (ch != '\\' || i + 1 >= value.size()) {
      out.push_back(ch);
      continue;
    }

    const char escaped = value[++i];
    switch (escaped) {
    case 'n':
      out.push_back('\n');
      break;
    case 'r':
      out.push_back('\r');
      break;
    case 't':
      out.push_back('\t');
      break;
    case '\\':
    case '"':
    case '/':
      out.push_back(escaped);
      break;
    default:
      out.push_back('\\');
      out.push_back(escaped);
      break;
    }
  }
  return out;
}

std::string ExtractTodoPatch(const ToolCallView &view) {
  rapidjson::Document args;
  if (ParseObject(view.args, args)) {
    return StringMember(args, "patch");
  }

  static const std::regex patch_pattern(
      R"PATCH("patch"\s*:\s*"((?:\\.|[^"\\])*)")PATCH");
  std::smatch match;
  if (std::regex_search(view.args, match, patch_pattern) && match.size() > 1) {
    return UnescapeJsonString(match[1].str());
  }
  return "";
}

struct TodoDeltaRow {
  std::string line;
  char marker = ' ';
};

struct TodoItemSnapshot {
  int id = 0;
  std::string status;
  std::string text;
};

std::vector<TodoItemSnapshot> ParseTodoItems(const std::string &json) {
  rapidjson::Document doc;
  if (!ParseObject(json, doc) || !doc.HasMember("items") || !doc["items"].IsArray()) {
    return {};
  }
  std::vector<TodoItemSnapshot> items;
  for (const auto &item : doc["items"].GetArray()) {
    if (!item.IsObject()) {
      continue;
    }
    TodoItemSnapshot snapshot;
    if (item.HasMember("id") && item["id"].IsInt()) {
      snapshot.id = item["id"].GetInt();
    }
    snapshot.status = StringMember(item, "status");
    snapshot.text = StringMember(item, "text");
    items.push_back(std::move(snapshot));
  }
  return items;
}

bool HasTodoSnapshot(const std::string &json) {
  rapidjson::Document doc;
  return ParseObject(json, doc) && doc.HasMember("items") && doc["items"].IsArray();
}

char TodoMarkerForStatus(const std::string &status) {
  if (status == "done") {
    return 'x';
  }
  if (status == "in_progress") {
    return '*';
  }
  return ' ';
}

std::vector<TodoDeltaRow> DiffTodoResults(const std::string &previous_result,
                                          const std::string &current_result) {
  const auto previous_items = ParseTodoItems(previous_result);
  const auto current_items = ParseTodoItems(current_result);
  if (!HasTodoSnapshot(previous_result) || !HasTodoSnapshot(current_result)) {
    return {};
  }

  std::map<int, TodoItemSnapshot> previous_by_id;
  for (const auto &item : previous_items) {
    previous_by_id[item.id] = item;
  }

  std::set<int> seen_ids;
  std::vector<TodoDeltaRow> delta;
  for (const auto &item : current_items) {
    seen_ids.insert(item.id);
    auto it_previous = previous_by_id.find(item.id);
    if (it_previous == previous_by_id.end()) {
      delta.push_back({"+ " + item.text, '+'});
      continue;
    }
    if (it_previous->second.status != item.status ||
        it_previous->second.text != item.text) {
      const char marker = TodoMarkerForStatus(item.status);
      std::string prefix = marker == 'x' ? "✓ "
                          : marker == '*' ? "~ "
                                          : "○ ";
      delta.push_back({prefix + item.text, marker});
    }
  }

  for (const auto &item : previous_items) {
    if (seen_ids.count(item.id) == 0) {
      delta.push_back({"- " + item.text, '-'});
    }
  }
  return delta;
}

void ApplyError(ToolPresentation &presentation, const ToolCallView &view,
                const std::string &title) {
  presentation.lifecycle = ToolPresentationLifecycle::Error;
  presentation.title = title;
  presentation.subtitle = view.name;
  presentation.error_text = firmius::shared::ErrorCleaner::clean(view.result);
}

std::string JoinCSV(const std::vector<std::string> &values) {
  std::string out;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }
    out += values[i];
  }
  return out;
}

std::vector<std::string> CollectChangedFields(const rapidjson::Value &args) {
  static const std::vector<std::string> kFields = {
      "title",      "objective", "summary", "status",       "context",
      "strategy",   "goal",      "depends_on",
      "verification_condition", "files_to_read", "files_to_touch",
      "handoff_notes", "completion", "notes", "assigned_agent_id"};
  std::vector<std::string> fields;
  if (!args.IsObject()) {
    return fields;
  }
  for (const auto &field : kFields) {
    if (args.HasMember(field.c_str())) {
      fields.push_back(field);
    }
  }
  return fields;
}

void AddLinesIfPresent(ToolPresentationSection &section, const rapidjson::Value &value,
                       const char *key, const std::string &prefix) {
  if (!value.IsObject() || !value.HasMember(key) || !value[key].IsArray()) {
    return;
  }
  for (const auto &item : value[key].GetArray()) {
    if (item.IsString()) {
      section.lines.push_back(prefix + item.GetString());
    }
  }
}

ToolPresentation BuildPlanCreatePresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::BodyFirstPreview;
  p.subtitle = view.name;

  rapidjson::Document args;
  const bool has_args = ParseObject(view.args, args);
  const std::string title = has_args ? StringMember(args, "title") : "";
  const std::string objective = has_args ? StringMember(args, "objective") : "";
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = "prepare plan creation";
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = "creating plan";
  } else {
    p.title = "plan created";
  }
  if (!title.empty()) {
    p.facts.push_back({"Title", title});
    p.compact_summary = title;
  }
  if (!objective.empty()) {
    p.facts.push_back({"Objective", objective});
  }
  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "plan creation failed");
    return p;
  }
  if (p.lifecycle != ToolPresentationLifecycle::Success) {
    return p;
  }

  rapidjson::Document result;
  if (ParseObject(view.result, result)) {
    const std::string plan_id = StringMember(result, "plan_id");
    if (!plan_id.empty()) {
      p.facts.push_back({"Plan ID", plan_id});
    }
    const std::string status = StringMember(result, "status");
    if (!status.empty()) {
      p.facts.push_back({"Status", status});
    }
    if (result.HasMember("active") && result["active"].IsBool()) {
      p.facts.push_back({"Active", result["active"].GetBool() ? "yes" : "no"});
    }
  }
  return p;
}

ToolPresentation BuildPlanGetPresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::BodyFirstPreview;
  p.subtitle = view.name;
  rapidjson::Document args;
  const bool has_args = ParseObject(view.args, args);
  const std::string plan_id = has_args ? StringMember(args, "plan_id") : "";
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = "prepare plan load";
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = "loading plan";
  } else {
    p.title = "plan details";
  }
  if (!plan_id.empty()) {
    p.facts.push_back({"Plan ID", plan_id});
  }
  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "plan load failed");
    return p;
  }
  if (p.lifecycle != ToolPresentationLifecycle::Success) {
    return p;
  }

  rapidjson::Document result;
  if (!ParseObject(view.result, result)) {
    return p;
  }
  const std::string title = StringMember(result, "title");
  if (!title.empty()) {
    p.facts.push_back({"Title", title});
    p.compact_summary = title;
  }
  const std::string status = StringMember(result, "status");
  if (!status.empty()) {
    p.facts.push_back({"Status", status});
  }
  const std::string objective = StringMember(result, "objective");
  if (!objective.empty()) {
    p.facts.push_back({"Objective", objective});
  }
  if (result.HasMember("chunks") && result["chunks"].IsArray()) {
    p.facts.push_back({"Chunk count", std::to_string(result["chunks"].Size())});
  }
  ToolPresentationSection summary;
  summary.title = "Plan summary";
  const std::string context = StringMember(result, "context");
  const std::string strategy = StringMember(result, "strategy");
  if (!context.empty()) {
    summary.lines.push_back("Context: " + context);
  }
  if (!strategy.empty()) {
    summary.lines.push_back("Strategy: " + strategy);
  }
  if (result.HasMember("chunks") && result["chunks"].IsArray()) {
    int gated = 0;
    for (const auto &chunk : result["chunks"].GetArray()) {
      if (chunk.IsObject() && chunk.HasMember("planning_gate") &&
          chunk["planning_gate"].IsBool() && chunk["planning_gate"].GetBool()) {
        ++gated;
      }
    }
    summary.lines.push_back("Planning gates: " + std::to_string(gated));
  }
  if (!summary.lines.empty()) {
    p.sections.push_back(std::move(summary));
  }
  return p;
}

ToolPresentation BuildPlanListPresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::CompactFactCard;
  p.density = ToolPresentationDensity::OneLineSummary;
  p.subtitle = view.name;
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = "prepare plan listing";
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = "listing plans";
  } else {
    p.title = "plans";
  }
  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "plan list failed");
    return p;
  }
  if (p.lifecycle != ToolPresentationLifecycle::Success) {
    return p;
  }
  rapidjson::Document result;
  if (!ParseArray(view.result, result)) {
    return p;
  }
  const auto total = result.Size();
  p.footer_badges.push_back(std::to_string(total) + " plan(s)");
  std::vector<std::string> rows;
  for (const auto &item : result.GetArray()) {
    const std::string id = StringMember(item, "plan_id");
    const std::string title = StringMember(item, "title");
    const std::string status = StringMember(item, "status");
    const bool active = item.IsObject() && item.HasMember("is_active") &&
                        item["is_active"].IsBool() && item["is_active"].GetBool();
    std::string row = active ? "* " : "  ";
    row += title.empty() ? "(untitled)" : title;
    if (!id.empty()) {
      row += " [" + id + "]";
    }
    if (!status.empty()) {
      row += " - " + status;
    }
    rows.push_back(std::move(row));
  }
  if (rows.empty()) {
    p.compact_summary = "no plans";
    return p;
  }
  p.compact_summary = rows.front();
  const size_t max_rows = view.show_result ? std::min<size_t>(rows.size(), 3u) : 1u;
  for (size_t i = 1; i < max_rows; ++i) {
    p.body_lines.push_back(rows[i]);
  }
  if (rows.size() > max_rows) {
    p.expandable = true;
    p.expanded = view.show_result;
  }
  return p;
}

ToolPresentation BuildPlanSetActivePresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::CompactFactCard;
  p.subtitle = view.name;
  rapidjson::Document args;
  ParseObject(view.args, args);
  const std::string requested_plan = StringMember(args, "plan_id");
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = "prepare active plan change";
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = "setting active plan";
  } else {
    p.title = "active plan updated";
  }
  if (!requested_plan.empty()) {
    p.facts.push_back({"Plan ID", requested_plan});
  }
  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "plan activation failed");
    return p;
  }
  if (p.lifecycle != ToolPresentationLifecycle::Success) {
    return p;
  }
  rapidjson::Document result;
  if (ParseObject(view.result, result)) {
    const std::string plan_id = StringMember(result, "plan_id");
    if (!plan_id.empty()) {
      p.facts.push_back({"Active plan", plan_id});
    }
    const std::string status = StringMember(result, "status");
    if (!status.empty()) {
      p.facts.push_back({"Status", status});
    }
  }
  return p;
}

ToolPresentation BuildPlanUpdatePresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::ResultsList;
  p.subtitle = view.name;
  rapidjson::Document args;
  const bool has_args = ParseObject(view.args, args);
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = "prepare plan update";
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = "updating plan";
  } else {
    p.title = "plan updated";
  }
  if (has_args) {
    const std::string id = StringMember(args, "plan_id");
    const std::string title = StringMember(args, "title");
    if (!id.empty()) {
      p.facts.push_back({"Plan ID", id});
    }
    if (!title.empty()) {
      p.facts.push_back({"Title", title});
    }
    auto fields = CollectChangedFields(args);
    if (!fields.empty()) {
      p.facts.push_back({"Changed fields", std::to_string(fields.size())});
      ToolPresentationSection diff;
      diff.title = "Updated fields";
      for (const auto &field : fields) {
        diff.lines.push_back(field);
        p.body_lines.push_back(field);
      }
      p.sections.push_back(std::move(diff));
    }
  }
  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "plan update failed");
    return p;
  }
  if (p.lifecycle != ToolPresentationLifecycle::Success) {
    return p;
  }
  rapidjson::Document result;
  if (ParseObject(view.result, result)) {
    const std::string status = StringMember(result, "status");
    if (!status.empty()) {
      p.facts.push_back({"Status", status});
    }
  }
  return p;
}

ToolPresentation BuildChunkAddPresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::BodyFirstPreview;
  p.subtitle = view.name;
  rapidjson::Document args;
  const bool has_args = ParseObject(view.args, args);
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = "prepare chunk add";
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = "adding chunk";
  } else {
    p.title = "chunk added";
  }
  if (has_args) {
    p.facts.push_back({"Plan ID", StringMember(args, "plan_id")});
    const std::string title = StringMember(args, "title");
    if (!title.empty()) {
      p.facts.push_back({"Title", title});
      p.compact_summary = title;
    }
    const std::string goal = StringMember(args, "goal");
    if (!goal.empty()) {
      p.facts.push_back({"Goal", goal});
    }
    auto deps = StringArrayMember(args, "depends_on");
    if (!deps.empty()) {
      p.facts.push_back({"Dependencies", std::to_string(deps.size())});
    }
    if (args.HasMember("planning_gate") && args["planning_gate"].IsBool()) {
      p.facts.push_back({"Planning gate", args["planning_gate"].GetBool() ? "yes" : "no"});
    }
  }
  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "chunk add failed");
    return p;
  }
  if (p.lifecycle != ToolPresentationLifecycle::Success) {
    return p;
  }
  rapidjson::Document result;
  if (ParseObject(view.result, result)) {
    p.facts.push_back({"Chunk ID", StringMember(result, "chunk_id")});
    const std::string status = StringMember(result, "status");
    if (!status.empty()) {
      p.facts.push_back({"Status", status});
    }
  }
  return p;
}

ToolPresentation BuildChunkGetPresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::BodyFirstPreview;
  p.subtitle = view.name;
  rapidjson::Document args;
  ParseObject(view.args, args);
  const std::string chunk_id = StringMember(args, "chunk_id");
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = "prepare chunk load";
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = "loading chunk";
  } else {
    p.title = "chunk details";
  }
  if (!chunk_id.empty()) {
    p.facts.push_back({"Chunk ID", chunk_id});
  }
  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "chunk load failed");
    return p;
  }
  if (p.lifecycle != ToolPresentationLifecycle::Success) {
    return p;
  }
  rapidjson::Document result;
  if (!ParseObject(view.result, result)) {
    return p;
  }
  p.facts.push_back({"Title", StringMember(result, "title")});
  p.facts.push_back({"Status", StringMember(result, "status")});
  const std::string goal = StringMember(result, "goal");
  if (!goal.empty()) {
    p.facts.push_back({"Goal", goal});
  }
  auto deps = StringArrayMember(result, "depends_on");
  if (!deps.empty()) {
    p.facts.push_back({"Dependencies", JoinCSV(deps)});
  }
  ToolPresentationSection details;
  details.title = "Execution details";
  AddLinesIfPresent(details, result, "files_to_read", "read: ");
  AddLinesIfPresent(details, result, "files_to_touch", "touch: ");
  const std::string cwd = StringMember(result, "cwd");
  if (!cwd.empty()) {
    details.lines.push_back("cwd: " + cwd);
  }
  const std::string verification = StringMember(result, "verification_condition");
  if (!verification.empty()) {
    details.lines.push_back("verify: " + verification);
  }
  const std::string handoff = StringMember(result, "handoff_notes");
  if (!handoff.empty()) {
    details.lines.push_back("handoff: " + handoff);
  }
  if (!details.lines.empty()) {
    p.body_lines = details.lines;
    p.sections.push_back(std::move(details));
  }
  return p;
}

ToolPresentation BuildChunkListPresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::ResultsList;
  p.subtitle = view.name;
  rapidjson::Document args;
  ParseObject(view.args, args);
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = "prepare chunk listing";
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = "listing chunks";
  } else {
    p.title = "chunks";
  }
  const std::string plan_id = StringMember(args, "plan_id");
  if (!plan_id.empty()) {
    p.facts.push_back({"Plan ID", plan_id});
  }
  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "chunk list failed");
    return p;
  }
  if (p.lifecycle != ToolPresentationLifecycle::Success) {
    return p;
  }
  rapidjson::Document result;
  if (!ParseArray(view.result, result)) {
    return p;
  }
  p.facts.push_back({"Count", std::to_string(result.Size())});
  std::map<std::string, std::vector<std::string>> by_status;
  for (const auto &item : result.GetArray()) {
    const std::string status = StringMember(item, "status");
    const std::string id = StringMember(item, "chunk_id");
    const std::string title = StringMember(item, "title");
    size_t dep_count = 0;
    if (item.IsObject() && item.HasMember("depends_on") && item["depends_on"].IsArray()) {
      dep_count = item["depends_on"].Size();
    }
    by_status[status].push_back(id + " " + title + " (deps:" + std::to_string(dep_count) + ")");
  }
  for (auto &pair : by_status) {
    ToolPresentationSection sec;
    sec.title = pair.first.empty() ? "Unknown" : pair.first;
    sec.lines = std::move(pair.second);
    p.body_lines.insert(p.body_lines.end(), sec.lines.begin(), sec.lines.end());
    p.sections.push_back(std::move(sec));
  }
  return p;
}

ToolPresentation BuildChunkReadyPresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::ResultsList;
  p.subtitle = view.name;
  rapidjson::Document args;
  ParseObject(view.args, args);
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = "prepare readiness check";
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = "checking chunk readiness";
  } else {
    p.title = "chunk readiness";
  }
  const std::string plan_id = StringMember(args, "plan_id");
  if (!plan_id.empty()) {
    p.facts.push_back({"Plan ID", plan_id});
  }
  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "readiness check failed");
    return p;
  }
  if (p.lifecycle != ToolPresentationLifecycle::Success) {
    return p;
  }
  rapidjson::Document result;
  if (!ParseArray(view.result, result)) {
    return p;
  }
  p.facts.push_back({"Ready", std::to_string(result.Size())});
  if (result.Empty()) {
    p.notices.push_back(
        {ToolPresentationNoticeKind::Warning, "No ready chunks; dependencies may still be unmet"});
    return p;
  }
  ToolPresentationSection rows;
  rows.title = "Ready chunks";
  for (const auto &item : result.GetArray()) {
    rows.lines.push_back(StringMember(item, "chunk_id") + " " + StringMember(item, "title"));
    p.body_lines.push_back(rows.lines.back());
  }
  p.sections.push_back(std::move(rows));
  return p;
}

ToolPresentation BuildChunkUpdatePresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::ResultsList;
  p.subtitle = view.name;
  rapidjson::Document args;
  const bool has_args = ParseObject(view.args, args);
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = "prepare chunk update";
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = "updating chunk";
  } else {
    p.title = "chunk updated";
  }
  if (has_args) {
    p.facts.push_back({"Chunk ID", StringMember(args, "chunk_id")});
    const std::string title = StringMember(args, "title");
    if (!title.empty()) {
      p.facts.push_back({"Title", title});
    }
    auto fields = CollectChangedFields(args);
    if (!fields.empty()) {
      p.facts.push_back({"Changed fields", std::to_string(fields.size())});
      ToolPresentationSection diff;
      diff.title = "Updated fields";
      for (const auto &field : fields) {
        diff.lines.push_back(field);
        p.body_lines.push_back(field);
      }
      p.sections.push_back(std::move(diff));
    }
  }
  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "chunk update failed");
    return p;
  }
  if (p.lifecycle != ToolPresentationLifecycle::Success) {
    return p;
  }
  rapidjson::Document result;
  if (ParseObject(view.result, result)) {
    const std::string status = StringMember(result, "status");
    if (!status.empty()) {
      p.facts.push_back({"Status", status});
    }
  }
  return p;
}

ToolPresentation BuildTodoWritePresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::ResultsList;
  p.density = ToolPresentationDensity::CompactSummaryCard;
  p.subtitle = view.name;
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = "prepare todo update";
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = "updating todo list";
  } else {
    p.title = "todo list updated";
  }
  const std::string patch = ExtractTodoPatch(view);

  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "todo write failed");
    return p;
  }
  if (p.lifecycle != ToolPresentationLifecycle::Success) {
    return p;
  }

  int added = 0;
  int updated = 0;
  int completed = 0;
  int cancelled = 0;
  std::vector<std::string> changed_items;
  const auto result_delta = DiffTodoResults(view.previous_result, view.result);
  if (!result_delta.empty()) {
    for (const auto &row : result_delta) {
      changed_items.push_back(row.line);
      if (row.marker == '+') {
        ++added;
      } else if (row.marker == '-') {
        ++cancelled;
      } else if (row.marker == 'x') {
        ++completed;
        ++updated;
      } else {
        ++updated;
      }
    }
  } else if (!patch.empty()) {
    static const std::regex mutation_pattern(
        R"(^\s*[0-9]+\.\s+\[([ +*x\-])\]\s+(.+?)\s*$)");
    std::istringstream ss(patch);
    std::string line;
    while (std::getline(ss, line)) {
      std::smatch match;
      if (!std::regex_match(line, match, mutation_pattern) || match.size() < 3) {
        continue;
      }
      const char marker = match[1].str()[0];
      const std::string text = match[2].str();
      if (marker == '+') {
        ++added;
        changed_items.push_back("+ " + text);
      } else if (marker == '-') {
        ++cancelled;
        changed_items.push_back("- " + text);
      } else if (marker == 'x') {
        ++completed;
        ++updated;
        changed_items.push_back("✓ " + text);
      } else if (marker == '*') {
        ++updated;
        changed_items.push_back("~ " + text);
      } else if (marker == ' ') {
        ++updated;
        changed_items.push_back("○ " + text);
      }
    }
  }
  p.footer_badges.push_back("+" + std::to_string(added));
  p.footer_badges.push_back("~" + std::to_string(updated));
  p.footer_badges.push_back("done " + std::to_string(completed));
  p.footer_badges.push_back("cancelled " + std::to_string(cancelled));

  if (!changed_items.empty()) {
    const size_t max_rows = view.show_result ? std::min<size_t>(changed_items.size(), 12)
                                              : std::min<size_t>(changed_items.size(), 5);
    for (size_t i = 0; i < changed_items.size() && i < max_rows; ++i) {
      p.body_lines.push_back(changed_items[i]);
    }
    if (changed_items.size() > max_rows) {
      p.expandable = true;
      p.expanded = view.show_result;
    }
  }
  return p;
}

} // namespace

bool IsWorkFamilyTool(const std::string &tool_name) {
  return IsMatch(tool_name, "plan_") || IsMatch(tool_name, "chunk_") ||
         IsMatch(tool_name, "todo_write");
}

ToolPresentation BuildWorkToolPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  if (IsMatch(view.name, "plan_create")) {
    presentation = BuildPlanCreatePresentation(view);
  } else if (IsMatch(view.name, "plan_get")) {
    presentation = BuildPlanGetPresentation(view);
  } else if (IsMatch(view.name, "plan_list")) {
    presentation = BuildPlanListPresentation(view);
  } else if (IsMatch(view.name, "plan_set_active")) {
    presentation = BuildPlanSetActivePresentation(view);
  } else if (IsMatch(view.name, "plan_update")) {
    presentation = BuildPlanUpdatePresentation(view);
  } else if (IsMatch(view.name, "chunk_add")) {
    presentation = BuildChunkAddPresentation(view);
  } else if (IsMatch(view.name, "chunk_get")) {
    presentation = BuildChunkGetPresentation(view);
  } else if (IsMatch(view.name, "chunk_list")) {
    presentation = BuildChunkListPresentation(view);
  } else if (IsMatch(view.name, "chunk_ready_for_execution")) {
    presentation = BuildChunkReadyPresentation(view);
  } else if (IsMatch(view.name, "chunk_update")) {
    presentation = BuildChunkUpdatePresentation(view);
  } else {
    presentation = BuildTodoWritePresentation(view);
  }
  if (!IsMatch(view.name, "todo_write") && !IsMatch(view.name, "plan_list")) {
    presentation.density = ToolPresentationDensity::DetailHeavy;
  }
  return presentation;
}

} // namespace firmius::tui
