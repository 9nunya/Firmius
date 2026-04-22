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

std::string ExtractAction(const ToolCallView &view) {
  rapidjson::Document doc;
  if (!ParseObject(view.args, doc)) {
    return "";
  }
  if (doc.HasMember("action") && doc["action"].IsString()) {
    return doc["action"].GetString();
  }
  return "";
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
      "handoff_notes", "completion", "notes", "assigned_agent_id", "tasks"};
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

std::string PreferPlanLabel(const rapidjson::Value &args,
                            const rapidjson::Value *result = nullptr) {
  const std::string title = StringMember(args, "title");
  if (!title.empty()) {
    return title;
  }
  if (result != nullptr) {
    const std::string result_title = StringMember(*result, "title");
    if (!result_title.empty()) {
      return result_title;
    }
    const std::string plan_id = StringMember(*result, "plan_id");
    if (!plan_id.empty()) {
      return plan_id;
    }
  }
  return StringMember(args, "plan_id");
}

std::string PreferChunkLabel(const rapidjson::Value &args,
                             const rapidjson::Value *result = nullptr) {
  const std::string title = StringMember(args, "title");
  if (!title.empty()) {
    return title;
  }
  if (result != nullptr) {
    const std::string result_title = StringMember(*result, "title");
    if (!result_title.empty()) {
      return result_title;
    }
    const std::string chunk_id = StringMember(*result, "chunk_id");
    if (!chunk_id.empty()) {
      return chunk_id;
    }
  }
  return StringMember(args, "chunk_id");
}

std::string WithFallbackLabel(const std::string &prefix,
                              const std::string &label,
                              const std::string &fallback) {
  return prefix + (label.empty() ? fallback : label);
}

std::string CompactCountBadge(const char *icon, size_t count) {
  return std::string(icon) + std::to_string(count);
}

ToolPresentation BuildPlanCreatePresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::CompactFactCard;
  p.density = ToolPresentationDensity::OneLineSummary;
  p.subtitle = view.name;

  rapidjson::Document args;
  const bool has_args = ParseObject(view.args, args);
  const std::string title = has_args ? StringMember(args, "title") : "";
  const std::string objective = has_args ? StringMember(args, "objective") : "";
  const std::string strategy = has_args ? StringMember(args, "strategy") : "";
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = WithFallbackLabel(" 󰒓 ", title, "plan");
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = WithFallbackLabel(" 󰒓 ", title, "plan");
  } else {
    p.title = WithFallbackLabel("󰒓 ", title, "plan");
  }
  p.compact_summary = p.title;
  
  if (has_args) {
    // Show objective/goal prominently
    if (!objective.empty()) {
      std::string goalDisplay = objective;
      if (goalDisplay.size() > 80) {
        goalDisplay = goalDisplay.substr(0, 77) + "...";
      }
      p.body_lines.push_back("󰍩 " + goalDisplay);
    }
    
    // Show strategy if present
    if (!strategy.empty()) {
      std::string stratDisplay = strategy;
      if (stratDisplay.size() > 80) {
        stratDisplay = stratDisplay.substr(0, 77) + "...";
      }
      p.body_lines.push_back("󰠞 " + stratDisplay);
    }
    
    // Initial state
    const std::string status = StringMember(args, "status");
    if (!status.empty()) {
      p.footer_badges.push_back(status);
    }
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
      p.footer_badges.push_back(plan_id);
    }
    const std::string status = StringMember(result, "status");
    if (!status.empty()) {
      p.footer_badges.push_back(status);
    }
    if (result.HasMember("active") && result["active"].IsBool()) {
      p.footer_badges.push_back(result["active"].GetBool() ? "󰐃" : "󰓛");
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
  p.density = ToolPresentationDensity::OneLineSummary;
  p.subtitle = view.name;
  rapidjson::Document args;
  ParseObject(view.args, args);
  const std::string requested_plan = StringMember(args, "plan_id");
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = WithFallbackLabel(" 󰘳 ", requested_plan,
                                "change");
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = WithFallbackLabel(" 󰘳 ", requested_plan,
                                "change");
  } else {
    p.title = WithFallbackLabel("󰘳 ", requested_plan, "change");
  }
  p.compact_summary = p.title;
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
      p.footer_badges.push_back(plan_id);
    }
    const std::string status = StringMember(result, "status");
    if (!status.empty()) {
      p.footer_badges.push_back(status);
    }
  }
  return p;
}

ToolPresentation BuildPlanUpdatePresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::CompactFactCard;
  p.density = ToolPresentationDensity::OneLineSummary;
  p.subtitle = view.name;
  rapidjson::Document args;
  const bool has_args = ParseObject(view.args, args);
  const std::string plan_label = has_args ? PreferPlanLabel(args) : "plan";
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = " 󰒓 " + plan_label;
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = " 󰒓 " + plan_label;
  } else {
    p.title = "󰒓 " + plan_label;
  }
  p.compact_summary = p.title;
  if (has_args) {
    auto fields = CollectChangedFields(args);
    if (!fields.empty()) {
      p.footer_badges.push_back(CompactCountBadge("", fields.size()));
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
      p.footer_badges.push_back(status);
    }
    const std::string plan_id = StringMember(result, "plan_id");
    if (!plan_id.empty()) {
      p.footer_badges.push_back(plan_id);
    }
  }
  return p;
}

ToolPresentation BuildChunkAddPresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::CompactFactCard;
  p.density = ToolPresentationDensity::OneLineSummary;
  p.subtitle = view.name;
  rapidjson::Document args;
  const bool has_args = ParseObject(view.args, args);
  const std::string chunk_label = has_args ? PreferChunkLabel(args) : "chunk";
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = " 󰐃 " + chunk_label;
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = " 󰐃 " + chunk_label;
  } else {
    p.title = "󰐃 " + chunk_label;
  }
  p.compact_summary = p.title;
  if (has_args) {
    const std::string plan_id = StringMember(args, "plan_id");
    if (!plan_id.empty()) {
      p.footer_badges.push_back(plan_id);
    }
    auto deps = StringArrayMember(args, "depends_on");
    if (!deps.empty()) {
      p.footer_badges.push_back(CompactCountBadge("", deps.size()));
    }
    if (args.HasMember("planning_gate") && args["planning_gate"].IsBool() &&
        args["planning_gate"].GetBool()) {
      p.footer_badges.push_back("󰐗");
    }
    
    // Files to touch/read
    auto filesToTouch = StringArrayMember(args, "files_to_touch");
    auto filesToRead = StringArrayMember(args, "files_to_read");
    if (!filesToTouch.empty()) {
      p.footer_badges.push_back(CompactCountBadge("", filesToTouch.size()));
      for (const auto &f : filesToTouch) {
        p.body_lines.push_back(" " + f);
      }
    }
    if (!filesToRead.empty() && filesToTouch.empty()) {
      p.footer_badges.push_back(CompactCountBadge("", filesToRead.size()));
    }
    
    // Subtasks
    if (args.HasMember("tasks") && args["tasks"].IsArray() &&
        !args["tasks"].Empty()) {
      p.footer_badges.push_back(
          CompactCountBadge("", static_cast<size_t>(args["tasks"].Size())));
      
      ToolPresentationSection subtasks;
      subtasks.title = "Subtasks";
      for (const auto &task : args["tasks"].GetArray()) {
        if (!task.IsObject()) continue;
        std::string taskTitle = StringMember(task, "title");
        std::string taskGoal = StringMember(task, "goal");
        auto taskFiles = StringArrayMember(task, "files_to_touch");
        
        std::string line = "• " + (taskTitle.empty() ? "Task" : taskTitle);
        if (!taskGoal.empty()) {
          if (taskGoal.size() > 60) {
            taskGoal = taskGoal.substr(0, 57) + "...";
          }
          line += ": " + taskGoal;
        }
        if (!taskFiles.empty()) {
          line += " ×" + std::to_string(taskFiles.size());
        }
        subtasks.lines.push_back(line);
        p.body_lines.push_back(line);
      }
      if (!subtasks.lines.empty()) {
        p.sections.push_back(std::move(subtasks));
      }
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
    const std::string chunk_id = StringMember(result, "chunk_id");
    if (!chunk_id.empty()) {
      p.footer_badges.push_back(chunk_id);
    }
    const std::string status = StringMember(result, "status");
    if (!status.empty()) {
      p.footer_badges.push_back(status);
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
    const size_t task_count =
        item.IsObject() && item.HasMember("task_count") &&
                item["task_count"].IsUint64()
            ? static_cast<size_t>(item["task_count"].GetUint64())
            : 0u;
    size_t dep_count = 0;
    if (item.IsObject() && item.HasMember("depends_on") && item["depends_on"].IsArray()) {
      dep_count = item["depends_on"].Size();
    }
    std::string row =
        id + " " + title + " (deps:" + std::to_string(dep_count) + ")";
    if (task_count > 0) {
      row += " (" + std::to_string(task_count) + " tasks)";
    }
    by_status[status].push_back(std::move(row));
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
  p.layout = ToolPresentationLayoutKind::CompactFactCard;
  p.density = ToolPresentationDensity::OneLineSummary;
  p.subtitle = view.name;
  rapidjson::Document args;
  const bool has_args = ParseObject(view.args, args);
  const std::string chunk_label = has_args ? PreferChunkLabel(args) : "chunk";
  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = " 󰐃 " + chunk_label;
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = " 󰐃 " + chunk_label;
  } else {
    p.title = "󰐃 " + chunk_label;
  }
  p.compact_summary = p.title;
  
  if (has_args) {
    // Detect what changed
    std::vector<std::string> changes;
    
    // Status change
    if (args.HasMember("status") && args["status"].IsString()) {
      changes.push_back("status → " + std::string(args["status"].GetString()));
    }
    
    // Attempt count
    if (args.HasMember("attempt_count") && args["attempt_count"].IsInt()) {
      changes.push_back("attempt #" + std::to_string(args["attempt_count"].GetInt()));
    }
    
    // Result summary
    if (args.HasMember("result_summary") && args["result_summary"].IsString()) {
      std::string summary = args["result_summary"].GetString();
      if (summary.size() > 50) {
        summary = summary.substr(0, 47) + "...";
      }
      changes.push_back("result: " + summary);
    }
    
    // Dependencies changed
    if (args.HasMember("depends_on") && args["depends_on"].IsArray()) {
      std::string depList;
      for (const auto &d : args["depends_on"].GetArray()) {
        if (!depList.empty()) depList += ", ";
        depList += d.GetString();
      }
      changes.push_back("depends: " + depList);
    }
    
    // Tasks added/modified
    if (args.HasMember("tasks") && args["tasks"].IsArray()) {
      changes.push_back("tasks: " + std::to_string(args["tasks"].Size()) + " subtask(s)");
    }
    
    
    if (!changes.empty()) {
      for (const auto &c : changes) {
        p.body_lines.push_back("• " + c);
      }
      p.footer_badges.push_back(CompactCountBadge("", changes.size()));
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
      p.footer_badges.push_back(status);
    }
    const std::string chunk_id = StringMember(result, "chunk_id");
    if (!chunk_id.empty()) {
      p.footer_badges.push_back(chunk_id);
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

ToolPresentation BuildFleetLockPresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::CompactFactCard;
  p.density = ToolPresentationDensity::CompactSummaryCard;
  p.subtitle = view.name;

  rapidjson::Document args;
  const bool has_args = ParseObject(view.args, args);
  const std::string mode = has_args ? StringMember(args, "mode") : "acquire";
  const std::string reason = has_args ? StringMember(args, "reason") : "";
  const std::string lockId = has_args ? StringMember(args, "lock_id") : "";
  auto paths = StringArrayMember(args, "paths");
  const std::string targetAgent = has_args ? StringMember(args, "target_agent_id") : "";
  const int timeout = has_args && args.HasMember("timeout_ms") && args["timeout_ms"].IsInt()
                      ? args["timeout_ms"].GetInt() : 0;

  // Set title based on mode
  if (mode == "acquire") {
    if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
      p.title = " 󰒙 acquire lock";
    } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
      p.title = " 󰒙 acquiring lock";
    } else {
      p.title = "󰒙 lock acquired";
    }
  } else if (mode == "release") {
    p.title = "󰓙 lock released";
  } else if (mode == "request") {
    if (p.lifecycle == ToolPresentationLifecycle::Preparing || p.lifecycle == ToolPresentationLifecycle::Running) {
      p.title = " 󰒙 requesting lock";
    } else {
      p.title = "󰒙 lock request fulfilled";
    }
  } else if (mode == "wait") {
    if (p.lifecycle == ToolPresentationLifecycle::Preparing || p.lifecycle == ToolPresentationLifecycle::Running) {
      p.title = " 󰒙 waiting for lock";
    } else {
      p.title = "󰒙 lock released";
    }
  } else if (mode == "check") {
    p.title = "󰊳 lock status";
  } else {
    p.title = "󰒙 lock " + mode;
  }

  // Show details based on mode
  if (has_args) {
    if (!reason.empty() && mode != "release" && mode != "check") {
      std::string reasonDisplay = reason;
      if (reasonDisplay.size() > 60) {
        reasonDisplay = reasonDisplay.substr(0, 57) + "...";
      }
      p.body_lines.push_back("Reason: " + reasonDisplay);
    }

    if (!paths.empty() && mode != "release" && mode != "wait") {
      p.footer_badges.push_back(CompactCountBadge("", paths.size()));
      if (mode == "check") {
        // For check, just show count
      } else {
        for (const auto &path : paths) {
          p.body_lines.push_back("   " + path);
        }
      }
    }

    if (!lockId.empty() && (mode == "release" || mode == "wait")) {
      p.footer_badges.push_back(lockId);
    }

    if (!targetAgent.empty()) {
      p.footer_badges.push_back("→ " + targetAgent.substr(0, 8));
    }

    if (timeout > 0) {
      p.footer_badges.push_back(std::to_string(timeout) + "ms");
    }
  }

  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "lock " + mode + " failed");
    return p;
  }
  if (p.lifecycle != ToolPresentationLifecycle::Success) {
    return p;
  }

  // Parse result
  rapidjson::Document result;
  if (ParseObject(view.result, result)) {
    const std::string resultLockId = StringMember(result, "lock_id");
    if (!resultLockId.empty()) {
      p.footer_badges.push_back(resultLockId);
    }
    const std::string status = StringMember(result, "status");
    if (!status.empty()) {
      p.footer_badges.push_back(status);
    }
    if (result.HasMember("has_conflicts") && result["has_conflicts"].IsBool()) {
      if (result["has_conflicts"].GetBool()) {
        p.notices.push_back({ToolPresentationNoticeKind::Warning, "File conflicts detected"});
        p.footer_badges.push_back("⚠ conflicts");
      } else {
        p.footer_badges.push_back("✓ no conflicts");
      }
    }
  }
  return p;
}

ToolPresentation BuildFleetLockRespondPresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::CompactFactCard;
  p.density = ToolPresentationDensity::OneLineSummary;
  p.subtitle = view.name;

  rapidjson::Document args;
  const bool has_args = ParseObject(view.args, args);
  const bool accept = has_args && args.HasMember("accept") && args["accept"].IsBool() && args["accept"].GetBool();
  const std::string requestId = has_args ? StringMember(args, "request_id") : "";
  const std::string denyReason = has_args ? StringMember(args, "deny_reason") : "";

  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = " respond to lock request";
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = " responding to lock request";
  } else if (accept) {
    p.title = "✓ lock request accepted";
  } else {
    p.title = "✗ lock request denied";
  }

  if (!requestId.empty()) {
    p.footer_badges.push_back(requestId);
  }

  if (!denyReason.empty()) {
    p.body_lines.push_back("Deny reason: " + denyReason);
  }

  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "lock respond failed");
    return p;
  }
  return p;
}

ToolPresentation BuildFleetStatusPresentation(const ToolCallView &view) {
  ToolPresentation p;
  p.lifecycle = LifecycleFromPhase(view);
  p.layout = ToolPresentationLayoutKind::ResultsList;
  p.density = ToolPresentationDensity::CompactSummaryCard;
  p.subtitle = view.name;

  if (p.lifecycle == ToolPresentationLifecycle::Preparing) {
    p.title = " 󰒙 fleet status";
  } else if (p.lifecycle == ToolPresentationLifecycle::Running) {
    p.title = " 󰒙 fetching fleet status";
  } else {
    p.title = "󰒙 fleet locks";
  }

  if (p.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(p, view, "fleet status failed");
    return p;
  }
  if (p.lifecycle != ToolPresentationLifecycle::Success) {
    return p;
  }

  rapidjson::Document result;
  if (ParseObject(view.result, result)) {
    if (result.HasMember("locks") && result["locks"].IsArray()) {
      p.footer_badges.push_back(CompactCountBadge("󰒙", result["locks"].Size()));

      ToolPresentationSection locks;
      locks.title = "Active Locks";
      for (const auto &lock : result["locks"].GetArray()) {
        if (!lock.IsObject()) continue;
        std::string lockId = StringMember(lock, "lock_id");
        std::string status = StringMember(lock, "status");
        std::string owner = StringMember(lock, "owner_agent_id");
        std::string reason = StringMember(lock, "reason");
        auto paths = StringArrayMember(lock, "paths");

        std::string line = "󰒙 " + (lockId.empty() ? "lock" : lockId);
        if (!status.empty()) {
          line += " [" + status + "]";
        }
        if (!owner.empty()) {
          line += " → " + owner.substr(0, 8);
        }
        if (!reason.empty()) {
          if (reason.size() > 40) {
            reason = reason.substr(0, 37) + "...";
          }
          line += ": " + reason;
        }
        if (!paths.empty()) {
          line += " ×" + std::to_string(paths.size());
        }
        locks.lines.push_back(line);
        p.body_lines.push_back(line);
      }
      if (!locks.lines.empty()) {
        p.sections.push_back(std::move(locks));
      }
    }
  }
  return p;
}

} // namespace

bool IsWorkFamilyTool(const std::string &tool_name) {
  return tool_name == "Work" || tool_name == "Todo" || tool_name == "Fleet" || tool_name == "todo_write" || tool_name == "chunk_add" || tool_name == "chunk_get" || tool_name == "plan_get" || tool_name == "plan_list";
}

ToolPresentation BuildWorkToolPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  std::string action = ExtractAction(view);
  if (action.empty()) {
    if (view.name == "todo_write") action = "TodoWrite";
    else if (view.name == "chunk_add") action = "AddChunk";
    else if (view.name == "chunk_get") action = "GetChunk";
    else if (view.name == "plan_get") action = "GetPlan";
    else if (view.name == "plan_list") action = "ListPlans";
  }
  if (action == "CreatePlan") {
    presentation = BuildPlanCreatePresentation(view);
  } else if (action == "GetPlan") {
    presentation = BuildPlanGetPresentation(view);
  } else if (action == "ListPlans") {
    presentation = BuildPlanListPresentation(view);
  } else if (action == "ActivatePlan") {
    presentation = BuildPlanSetActivePresentation(view);
  } else if (action == "UpdatePlan") {
    presentation = BuildPlanUpdatePresentation(view);
  } else if (action == "AddChunk") {
    presentation = BuildChunkAddPresentation(view);
  } else if (action == "GetChunk") {
    presentation = BuildChunkGetPresentation(view);
  } else if (action == "ListChunks") {
    presentation = BuildChunkListPresentation(view);
  } else if (action == "ReadyChunk") {
    presentation = BuildChunkReadyPresentation(view);
  } else if (action == "UpdateChunk") {
    presentation = BuildChunkUpdatePresentation(view);
  } else if (action == "Lock") {
    presentation = BuildFleetLockPresentation(view);
  } else if (action == "Respond") {
    presentation = BuildFleetLockRespondPresentation(view);
  } else if (action == "Status") {
    presentation = BuildFleetStatusPresentation(view);
  } else {
    presentation = BuildTodoWritePresentation(view);
  }
  if (view.name != "Todo" && action != "ListPlans" &&
      action != "CreatePlan" && action != "ActivatePlan" &&
      action != "UpdatePlan" && action != "AddChunk" &&
      action != "UpdateChunk" && action != "Lock" &&
      action != "Status") {
    presentation.density = ToolPresentationDensity::DetailHeavy;
  }
  return presentation;
}

} // namespace firmius::tui
