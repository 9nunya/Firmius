#include "agents/ContextBudget.hpp"
#include "StreamStateManager.hpp"
#include "components/ToolBlock.hpp"
#include "utils/ToolSummaries.hpp"
#include "utils/StringUtil.hpp"
#include "harness/Harness.hpp"
#include "persistence/ThreadManager.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <rapidjson/document.h>

namespace firmius::tui {

namespace {

struct ParsedProcessResult {
  std::string process_id;
  std::string command;
  std::string cwd;
  std::string finish_reason;
  int exit_code = 0;
  bool exit_known = false;
  double duration_ms = 0.0;
};

struct ParsedToolArgs {
  std::string process_id;
  std::string command;
  std::string cwd;
  std::string pattern;
  std::string input;
};

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
  std::string summary;
  std::string error;
  bool fallback_used = false;
  std::string route_category;
  std::vector<std::string> attempted_categories;
  std::vector<std::string> artifacts_created;
  std::vector<std::string> artifacts_updated;
};

std::string ltrimLeadingBlankLines(std::string text) {
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t line_end = text.find('\n', pos);
    const std::string_view line =
        line_end == std::string::npos
            ? std::string_view(text).substr(pos)
            : std::string_view(text).substr(pos, line_end - pos);
    if (!firmius::shared::StringUtil::trim(line).empty()) {
      break;
    }
    if (line_end == std::string::npos) {
      return "";
    }
    pos = line_end + 1;
  }
  return text.substr(pos);
}

ParsedProcessResult parseProcessResult(const std::string &result) {
  ParsedProcessResult parsed;
  rapidjson::Document doc;
  doc.Parse(result.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return parsed;
  }

  if (doc.HasMember("process_id") && doc["process_id"].IsString()) {
    parsed.process_id = doc["process_id"].GetString();
  }
  if (doc.HasMember("command") && doc["command"].IsString()) {
    parsed.command = doc["command"].GetString();
  }
  if (doc.HasMember("cwd") && doc["cwd"].IsString()) {
    parsed.cwd = doc["cwd"].GetString();
  }
  if (doc.HasMember("finish_reason") && doc["finish_reason"].IsString()) {
    parsed.finish_reason = doc["finish_reason"].GetString();
  }
  if (doc.HasMember("exit_code") && doc["exit_code"].IsInt()) {
    parsed.exit_code = doc["exit_code"].GetInt();
    parsed.exit_known = true;
  }
  if (doc.HasMember("duration_ms") && doc["duration_ms"].IsNumber()) {
    parsed.duration_ms = doc["duration_ms"].GetDouble();
  }

  return parsed;
}

ParsedToolArgs parseToolArgs(const std::string &args) {
  ParsedToolArgs parsed;
  rapidjson::Document doc;
  doc.Parse(args.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return parsed;
  }
  if (doc.HasMember("process_id") && doc["process_id"].IsString()) {
    parsed.process_id = doc["process_id"].GetString();
  }
  if (doc.HasMember("command") && doc["command"].IsString()) {
    parsed.command = doc["command"].GetString();
  }
  if (doc.HasMember("cwd") && doc["cwd"].IsString()) {
    parsed.cwd = doc["cwd"].GetString();
  }
  if (doc.HasMember("pattern") && doc["pattern"].IsString()) {
    parsed.pattern = doc["pattern"].GetString();
  }
  if (doc.HasMember("input") && doc["input"].IsString()) {
    parsed.input = doc["input"].GetString();
  }
  if (doc.HasMember("code") && doc["code"].IsString()) {
    const std::string code = doc["code"].GetString();
    std::istringstream stream(code);
    std::string first_line;
    while (std::getline(stream, first_line)) {
      if (!firmius::shared::StringUtil::trim(first_line).empty()) {
        parsed.command = "python: " + first_line;
        break;
      }
    }
    if (parsed.command.empty()) {
      parsed.command = "python";
    }
  }
  return parsed;
}

ParsedSubagentArgs parseSubagentArgs(const std::string &args) {
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

void appendOutputTail(std::string &tail, const std::string &delta,
                      size_t max_chars = 1200) {
  if (delta.empty()) {
    return;
  }
  tail += delta;
  if (tail.size() > max_chars) {
    tail.erase(0, tail.size() - max_chars);
  }
}

ParsedSubagentResult parseSubagentResult(const std::string &result) {
  ParsedSubagentResult parsed;
  parsed.attempted_categories.reserve(4);
  parsed.artifacts_created.reserve(8);
  parsed.artifacts_updated.reserve(8);
  
  rapidjson::Document doc;
  doc.Parse(result.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return parsed;
  }
  if (doc.HasMember("status") && doc["status"].IsString()) {
    parsed.status = doc["status"].GetString();
  }
  if (doc.HasMember("agentId") && doc["agentId"].IsString()) {
    parsed.agent_id = doc["agentId"].GetString();
  }
  if (doc.HasMember("result") && doc["result"].IsString()) {
    parsed.summary = doc["result"].GetString();
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
  auto parseArtifactArray = [](const rapidjson::Value &value) {
    std::vector<std::string> refs;
    if (!value.IsArray()) {
      return refs;
    }
    for (const auto &item : value.GetArray()) {
      if (item.IsString()) {
        refs.push_back(item.GetString());
        continue;
      }
      if (!item.IsObject()) {
        continue;
      }
      if (item.HasMember("reference") && item["reference"].IsString()) {
        refs.push_back(item["reference"].GetString());
      } else if (item.HasMember("filename") && item["filename"].IsString()) {
        refs.push_back(item["filename"].GetString());
      }
    }
    return refs;
  };
  if (doc.HasMember("artifacts_created")) {
    parsed.artifacts_created = parseArtifactArray(doc["artifacts_created"]);
  }
  if (doc.HasMember("artifacts_updated")) {
    parsed.artifacts_updated = parseArtifactArray(doc["artifacts_updated"]);
  }
  return parsed;
}

std::string jsonStringMember(const rapidjson::Value &value, const char *key) {
  if (value.IsObject() && value.HasMember(key) && value[key].IsString()) {
    return value[key].GetString();
  }
  return "";
}

bool isFileEditLikeToolName(const std::string &name) {
  return name == "file_edit" || name == "file_write";
}

void mergeFileEditSignal(shared::ToolCallView &view,
                         const shared::FileEditSignal &signal) {
  if (signal.path.empty()) {
    return;
  }

  auto it = std::find_if(
      view.fileEditEvents.begin(), view.fileEditEvents.end(),
      [&](const shared::FileEditSignal &existing) {
        return existing.path == signal.path;
      });
  if (it == view.fileEditEvents.end()) {
    view.fileEditEvents.push_back(signal);
    return;
  }

  if (!signal.diffPreview.empty()) {
    it->diffPreview = signal.diffPreview;
  }
  if (signal.addedLines > 0 || it->addedLines == 0) {
    it->addedLines = signal.addedLines;
  }
  if (signal.removedLines > 0 || it->removedLines == 0) {
    it->removedLines = signal.removedLines;
  }
}

std::vector<shared::FileEditSignal>
parseFileEditSignalsFromResult(const std::string &result) {
  std::vector<shared::FileEditSignal> signals;

  rapidjson::Document doc;
  doc.Parse(result.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return signals;
  }

  auto appendSignal = [&](const rapidjson::Value &value) {
    if (!value.IsObject()) {
      return;
    }

    shared::FileEditSignal signal;
    if (value.HasMember("path") && value["path"].IsString()) {
      signal.path = value["path"].GetString();
    }
    if (signal.path.empty()) {
      return;
    }
    if (value.HasMember("diff_preview") && value["diff_preview"].IsString()) {
      signal.diffPreview = value["diff_preview"].GetString();
    }
    if (value.HasMember("added_lines") && value["added_lines"].IsInt()) {
      signal.addedLines = value["added_lines"].GetInt();
    }
    if (value.HasMember("removed_lines") && value["removed_lines"].IsInt()) {
      signal.removedLines = value["removed_lines"].GetInt();
    }
    signals.push_back(std::move(signal));
  };

  if (doc.HasMember("files") && doc["files"].IsArray()) {
    for (const auto &entry : doc["files"].GetArray()) {
      appendSignal(entry);
    }
  } else {
    appendSignal(doc);
  }

  return signals;
}

bool shouldRetainCompletedToolCall(const shared::ToolCallView &view) {
  if (view.name == "summon_subagent") {
    return true;
  }

  if (view.phase == shared::ToolPhase::BackgroundRunning ||
      view.phase == shared::ToolPhase::Called ||
      view.phase == shared::ToolPhase::Preparing) {
    return true;
  }

  return isFileEditLikeToolName(view.name) || !view.fileEditEvents.empty();
}

std::string artifactReadSummary(const std::string &args, const std::string &result) {
  rapidjson::Document args_doc;
  std::string label;
  args_doc.Parse(args.c_str());
  if (!args_doc.HasParseError() && args_doc.IsObject()) {
    label = jsonStringMember(args_doc, "reference");
    if (label.empty()) {
      label = jsonStringMember(args_doc, "name");
    }
  }

  rapidjson::Document result_doc;
  result_doc.Parse(result.c_str());
  if (!result_doc.HasParseError() && result_doc.IsObject()) {
    const std::string resolved_reference = jsonStringMember(result_doc, "reference");
    if (!resolved_reference.empty()) {
      label = resolved_reference;
    }
    const rapidjson::Value &artifact =
        result_doc.HasMember("artifact") ? result_doc["artifact"] : result_doc;
    if (label.empty()) {
      label = jsonStringMember(artifact, "filename");
    }
  }

  return label.empty() ? "Loaded artifact" : ("Loaded " + label);
}

bool shouldAppendErrorToLiveChat(const std::string &message) {
  if (message.empty()) {
    return false;
  }

  const std::string lower = firmius::shared::StringUtil::toLower(message);
  const bool has_raw_body =
      message.find("Raw provider body:") != std::string::npos ||
      message.find("Raw body:") != std::string::npos ||
      message.find("Response body:") != std::string::npos;
  const bool rate_limited =
      lower.find("http 429") != std::string::npos ||
      lower.find("rate limit") != std::string::npos ||
      lower.find("rate limited") != std::string::npos ||
      lower.find("quota exhausted") != std::string::npos;

  return has_raw_body && rate_limited;
}

void appendErrorToTimelineIfRelevant(std::vector<TimelineEntry> &timeline,
                                     std::uint64_t &nextSequence,
                                     const std::string &agentId,
                                     const std::string &message,
                                     bool hideErrors = false) {
  if (hideErrors) {
    return;
  }
  if (!shouldAppendErrorToLiveChat(message)) {
    return;
  }

  timeline.push_back(TimelineEntry{
      TimelineEntry::Kind::Error,
      "error-" + std::to_string(++nextSequence), message, agentId});
}

std::string summarizeHistoricalToolEntry(const std::string &name,
                                         const std::string &args,
                                         const std::string &result,
                                         bool success) {
  if (name == "artifact_read" && success) {
    return artifactReadSummary(args, result);
  }
  return shared::SummarizeToolCall(name, args, shared::ToolPhase::Finished);
}

std::vector<shared::SubagentToolLogEntry>
synthesizeHistoricalSubagentLog(const shared::AgentHistory &history,
                                const std::string &task,
                                const NormalizedSubagentState &subagent) {
  std::vector<shared::SubagentToolLogEntry> entries;
  std::unordered_map<std::string, size_t> tool_index_by_id;

  if (!task.empty()) {
    shared::SubagentToolLogEntry entry;
    entry.summary = "Task: " + task;
    entry.phase = shared::ToolPhase::Finished;
    entries.push_back(std::move(entry));
  }

  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      for (const auto &content : msg.content) {
        if (auto *tc = std::get_if<shared::ToolCallContent>(&content)) {
          shared::SubagentToolLogEntry entry;
          entry.name = tc->name;
          entry.args = tc->args;
          entry.toolCallId = tc->id;
          entry.phase = shared::ToolPhase::Finished;
          entry.summary = shared::SummarizeToolCall(
              tc->name, tc->args, shared::ToolPhase::Finished);
          tool_index_by_id[tc->id] = entries.size();
          entries.push_back(std::move(entry));
          continue;
        }

        if (auto *tr = std::get_if<shared::ToolResultContent>(&content)) {
          auto it_entry = tool_index_by_id.find(tr->toolCallId);
          if (it_entry == tool_index_by_id.end()) {
            continue;
          }
          auto &entry = entries[it_entry->second];
          entry.phase = tr->success ? shared::ToolPhase::Finished
                                    : shared::ToolPhase::Error;
          entry.summary = summarizeHistoricalToolEntry(
              entry.name, entry.args, tr->result, tr->success);
          continue;
        }

        if (auto *th = std::get_if<shared::ThinkingContent>(&content)) {
          if (th->thinking.empty()) {
            continue;
          }
          shared::SubagentToolLogEntry entry;
          entry.summary = "Thought";
          entry.phase = shared::ToolPhase::Finished;
          entries.push_back(std::move(entry));
        }
      }
    }
  }

  if (!subagent.wait_state.empty()) {
    shared::SubagentToolLogEntry entry;
    if (subagent.wait_state == "completed_no_summary") {
      entry.summary = "Done (no summary)";
      entry.phase = shared::ToolPhase::Finished;
    } else if (subagent.wait_state == "cancelled") {
      entry.summary = "Cancelled";
      entry.phase = shared::ToolPhase::Finished;
    } else if (subagent.wait_state == "failed") {
      entry.summary = subagent.error_text.empty() ? "Failed"
                                                  : "Failed: " + subagent.error_text;
      entry.phase = shared::ToolPhase::Error;
    } else if (subagent.wait_state == "completed") {
      entry.summary = "Done";
      entry.phase = shared::ToolPhase::Finished;
    }
    if (!entry.summary.empty()) {
      entries.push_back(std::move(entry));
    }
  }

  return entries;
}

std::optional<shared::SubagentToolLogEntry>
terminalSubagentLogEntry(const NormalizedSubagentState &subagent) {
  shared::SubagentToolLogEntry entry;
  if (subagent.wait_state == "completed") {
    entry.summary = "Done";
    entry.phase = shared::ToolPhase::Finished;
  } else if (subagent.wait_state == "completed_no_summary") {
    entry.summary = "Done (no summary)";
    entry.phase = shared::ToolPhase::Finished;
  } else if (subagent.wait_state == "cancelled") {
    entry.summary = "Cancelled";
    entry.phase = shared::ToolPhase::Finished;
  } else if (subagent.wait_state == "failed") {
    entry.summary =
        subagent.error_text.empty() ? "Failed" : "Failed: " + subagent.error_text;
    entry.phase = shared::ToolPhase::Error;
  } else {
    return std::nullopt;
  }
  return entry;
}

bool IsGenericTerminalSubagentState(const std::string &state) {
  return state == "completed" || state == "failed";
}

bool isParentInterruptedWhileWaitingMessage(const std::string &message) {
  return message.find("Parent agent interrupted while waiting for subagent.") !=
         std::string::npos;
}

int SubagentStateSpecificity(const std::string &state) {
  if (state.empty()) {
    return 0;
  }
  if (state == "spawned" || state == "re-tasked") {
    return 1;
  }
  if (state == "completed_no_summary" || state == "cancelled") {
    return 3;
  }
  if (IsGenericTerminalSubagentState(state)) {
    return 2;
  }
  return 2;
}

bool IsInformativeSubagentLogEntry(const shared::SubagentToolLogEntry &entry) {
  if (!entry.toolCallId.empty()) {
    return true;
  }
  if (entry.summary.empty()) {
    return false;
  }
  return entry.summary.rfind("Task: ", 0) != 0 && entry.summary != "Done" &&
         entry.summary.rfind("State: ", 0) != 0;
}

size_t CountInformativeSubagentLogEntries(
    const std::vector<shared::SubagentToolLogEntry> &entries) {
  size_t count = 0;
  for (const auto &entry : entries) {
    if (IsInformativeSubagentLogEntry(entry)) {
      ++count;
    }
  }
  return count;
}

std::vector<shared::SubagentToolLogEntry> ChooseRicherSubagentLog(
    const std::vector<shared::SubagentToolLogEntry> &left,
    const std::vector<shared::SubagentToolLogEntry> &right) {
  const size_t left_informative = CountInformativeSubagentLogEntries(left);
  const size_t right_informative = CountInformativeSubagentLogEntries(right);
  if (right_informative > left_informative) {
    return right;
  }
  if (right_informative == left_informative && right.size() > left.size()) {
    return right;
  }
  return left;
}

void MergeSubagentState(NormalizedSubagentState &target,
                        const NormalizedSubagentState &source) {
  if (target.parent_tool_call_id.empty()) {
    target.parent_tool_call_id = source.parent_tool_call_id;
  }
  if (target.owner_agent_id.empty()) {
    target.owner_agent_id = source.owner_agent_id;
  }
  if (target.child_agent_id.empty()) {
    target.child_agent_id = source.child_agent_id;
  }
  if (target.child_title.empty()) {
    target.child_title = source.child_title;
  }
  if (target.child_friendly_name.empty()) {
    target.child_friendly_name = source.child_friendly_name;
  }
  if (target.task.empty()) {
    target.task = source.task;
  }

  target.provider_waiting = target.provider_waiting || source.provider_waiting;
  target.retrying = target.retrying || source.retrying;
  target.account_switched = target.account_switched || source.account_switched;
  target.fallback_used = target.fallback_used || source.fallback_used;

  const int target_specificity = SubagentStateSpecificity(target.wait_state);
  const int source_specificity = SubagentStateSpecificity(source.wait_state);
  const bool should_take_source_wait_state =
      !source.wait_state.empty() &&
      (target.wait_state.empty() || source_specificity > target_specificity ||
       (source_specificity == target_specificity &&
        IsGenericTerminalSubagentState(target.wait_state) &&
        !IsGenericTerminalSubagentState(source.wait_state)));
  if (should_take_source_wait_state) {
    target.wait_state = source.wait_state;
    target.outcome = source.outcome;
    target.running = source.running;
    target.waiting = source.waiting;
  } else if (!target.running && source.running) {
    target.running = true;
  } else if (!target.waiting && source.waiting) {
    target.waiting = true;
  }
  if (target.outcome == SubagentOutcomeKind::Unknown &&
      source.outcome != SubagentOutcomeKind::Unknown) {
    target.outcome = source.outcome;
  }

  if (target.final_summary.empty()) {
    target.final_summary = source.final_summary;
  }
  if (target.error_text.empty()) {
    target.error_text = source.error_text;
  }
  if (target.route_category.empty()) {
    target.route_category = source.route_category;
  }
  if (target.attempted_categories.empty() && !source.attempted_categories.empty()) {
    target.attempted_categories = source.attempted_categories;
  }
  if (target.artifacts_created.empty() && !source.artifacts_created.empty()) {
    target.artifacts_created = source.artifacts_created;
  }
  if (target.artifacts_updated.empty() && !source.artifacts_updated.empty()) {
    target.artifacts_updated = source.artifacts_updated;
  }

  target.activity_log = ChooseRicherSubagentLog(target.activity_log, source.activity_log);
}

std::string formatCompactCount(uint32_t value) {
  if (value >= 1000000) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << (static_cast<double>(value) / 1000000.0);
    auto text = out.str();
    if (text.size() > 2 && text.substr(text.size() - 2) == ".0") {
      text.erase(text.size() - 2);
    }
    return text + "M";
  }
  if (value >= 1000) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << (static_cast<double>(value) / 1000.0);
    auto text = out.str();
    if (text.size() > 2 && text.substr(text.size() - 2) == ".0") {
      text.erase(text.size() - 2);
    }
    return text + "k";
  }
  return std::to_string(value);
}

std::string formatDurationMs(uint64_t durationMs) {
  const uint64_t totalSeconds = durationMs / 1000;
  const uint64_t minutes = totalSeconds / 60;
  const uint64_t seconds = totalSeconds % 60;
  std::ostringstream out;
  if (minutes > 0) {
    out << minutes << "m" << seconds << "s";
  } else {
    out << seconds << "s";
  }
  return out.str();
}

} // namespace

static std::string formatDuration(float seconds) {
  if (seconds < 0.1f)
    return "<0.1s";
  int tenths = static_cast<int>(seconds * 10 + 0.5f);
  return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "s";
}

void StreamStateManager::clearRetryStatus() {
  retry_status_.clear();
  account_swaps_.clear();
}

void StreamStateManager::handleAgentThinking(const shared::AgentThinking &e) {
  auto &s = streams_[e.agentId];
  if (!s.is_thinking) {
    s.thinking_start = std::chrono::steady_clock::now();
    s.is_thinking = true;
  }
  s.compaction_finished = false;
  s.compaction_completion.clear();
  std::string delta = e.delta;
  if (s.thinking.empty()) {
    delta = ltrimLeadingBlankLines(delta);
  }
  s.thinking += delta;
  appendLiveTimelineDelta(e.agentId, TimelineEntry::Kind::Thinking, delta);
  if (!firmius::shared::StringUtil::trim(delta).empty()) {
    live_quick_clusters_[e.agentId].prose_since_last_tool = true;
  }
  s.provider_waiting = false;
  clearRetryStatus();
}

void StreamStateManager::handleAgentText(const shared::AgentText &e) {
  auto &s = streams_[e.agentId];
  if (s.is_thinking) {
    auto elapsed = std::chrono::steady_clock::now() - s.thinking_start;
    float secs = std::chrono::duration<float>(elapsed).count();
    s.is_thinking = false;
    pushThinkingDuration(e.agentId, secs);
  }
  s.compaction_finished = false;
  s.compaction_completion.clear();
  std::string delta = e.delta;
  if (s.text.empty()) {
    delta = ltrimLeadingBlankLines(delta);
  }
  s.text += delta;
  appendLiveTimelineDelta(e.agentId, TimelineEntry::Kind::Text, delta);
  if (!firmius::shared::StringUtil::trim(delta).empty()) {
    live_quick_clusters_[e.agentId].prose_since_last_tool = true;
  }
  s.provider_waiting = false;
  clearRetryStatus();
}

void StreamStateManager::handleAgentTurnCompleted(
    const shared::AgentTurnCompleted &e) {
  auto &s = streams_[e.agentId];
  s.thinking.clear();
  s.text.clear();
  s.compaction_finished = false;
  s.compaction_completion.clear();
  s.provider_waiting = false;
  s.is_thinking = false;
  s.has_active_live_entry = false;
  s.active_live_entry_id.clear();
  pushTokenUsage(e.agentId, e.aggregateMetrics);
  {
    auto &summaries = completed_run_summaries_[e.agentId];
    CompletedRunSummary summary;
    const std::string title = getAgentTitle(e.agentId);
    const std::string model =
        agent_provider_model_.count(e.agentId) ? agent_provider_model_[e.agentId]
                                               : "";
    const uint64_t durationMs =
        e.turn.metrics.timing.endMs > e.turn.metrics.timing.startMs
            ? (e.turn.metrics.timing.endMs - e.turn.metrics.timing.startMs)
            : 0;
    std::ostringstream row;
    row << "completed";
    if (!title.empty()) {
      row << " · " << title;
    }
    if (!model.empty()) {
      row << " · " << model;
    }
    if (durationMs > 0) {
      row << " · " << formatDurationMs(durationMs);
    }
    if (e.turn.metrics.context.sentTokens > 0 ||
        e.turn.metrics.tokens.completion > 0) {
      row << " · ↑ " << formatCompactCount(e.turn.metrics.context.sentTokens);
      if (e.turn.metrics.context.billedPromptTokens > 0 &&
          e.turn.metrics.context.billedPromptTokens !=
              e.turn.metrics.context.sentTokens) {
        row << "/" << formatCompactCount(
                            e.turn.metrics.context.billedPromptTokens);
      }
      row << " ↓ " << formatCompactCount(e.turn.metrics.tokens.completion);
    }
    const std::string contextSummary =
        firmius::core::summarizeContextWindowMetrics(e.turn.metrics.context, 2);
    if (!contextSummary.empty() && contextSummary != "sent=0") {
      row << " · " << contextSummary;
    }
    summary.text = row.str();
    summaries.push_back(std::move(summary));
    while (summaries.size() > 6) {
      summaries.erase(summaries.begin());
    }
  }

  // Process tool results from the turn to determine success/failure.
  // Tool results may appear under assistant or tool-result roles depending on
  // how the turn was reconstructed, so inspect every content part.
  std::unordered_map<std::string, std::pair<bool, std::string>> toolResultMap;
  for (const auto &msg : e.turn.messages) {
    for (const auto &content : msg.content) {
      if (auto *trc = std::get_if<shared::ToolResultContent>(&content)) {
        toolResultMap[trc->toolCallId] = {trc->success, trc->result};
      }
    }
  }

  // Mark all pending tool calls for this agent as finished with proper success state
  for (auto &[toolId, view] : tool_calls_) {
    if (view && view->agentId == e.agentId &&
        (view->phase != ToolPhase::Error &&
         (view->phase != ToolPhase::Finished ||
          isFileEditLikeToolName(view->name)))) {
      auto it = toolResultMap.find(toolId);
      if (it != toolResultMap.end()) {
        applyToolResult(view, it->second.first, it->second.second);
      }
    }
  }

  // Mark all pending subagent tool log entries as finished and update summaries
  for (auto &[toolId, view] : tool_calls_) {
    if (view && view->name == "summon_subagent") {
      for (auto &entry : view->subagent_tool_log) {
        if (entry.phase != ToolPhase::Finished && !entry.name.empty()) {
          // Regenerate summary with stored name/args
          entry.summary = shared::SummarizeToolCall(
              entry.name, entry.args, ToolPhase::Finished);
          entry.phase = ToolPhase::Finished;
        }
      }
      auto it_state = subagent_state_.find(toolId);
      if (it_state != subagent_state_.end()) {
        it_state->second.activity_log = view->subagent_tool_log;
      }
    }
  }

  for (auto it = tool_calls_.begin(); it != tool_calls_.end();) {
    if (it->second && it->second->agentId == e.agentId &&
        !shouldRetainCompletedToolCall(*it->second)) {
      it = tool_calls_.erase(it);
    } else {
      ++it;
    }
  }
  timeline_.erase(
      std::remove_if(
          timeline_.begin(), timeline_.end(), [&](const TimelineEntry &entry) {
            if (entry.agentId != e.agentId) {
              return false;
            }
            if (entry.kind == TimelineEntry::Kind::Thinking ||
                entry.kind == TimelineEntry::Kind::Text) {
              return true;
            }
            if (entry.kind != TimelineEntry::Kind::ToolCall) {
              return false;
            }
            auto it_tool = tool_calls_.find(entry.id);
            if (it_tool != tool_calls_.end() && it_tool->second &&
                shouldRetainCompletedToolCall(*it_tool->second)) {
                return false;
            }
            return true;
          }),
      timeline_.end());

  for (auto it = tool_call_cluster_ids_.begin();
       it != tool_call_cluster_ids_.end();) {
    if (tool_calls_.count(it->first) == 0) {
      it = tool_call_cluster_ids_.erase(it);
    } else {
      ++it;
    }
  }
  live_quick_clusters_[e.agentId] = {};
}

void StreamStateManager::handleAgentProviderWaiting(
    const shared::AgentProviderWaiting &e) {
  streams_[e.agentId].provider_waiting = true;
  clearRetryStatus();
  reactivateSubagentParentView(e.agentId);
  auto it_parent = subagent_to_parent_tool_.find(e.agentId);
  if (it_parent != subagent_to_parent_tool_.end()) {
    auto &subagent = subagent_state_[it_parent->second];
    subagent.provider_waiting = true;
    subagent.retrying = false;
    subagent.account_switched = false;
    subagent.running = true;
    subagent.wait_state = "provider_waiting";
    shared::SubagentToolLogEntry entry;
    entry.summary = "Provider waiting";
    entry.phase = shared::ToolPhase::Preparing;
    subagent.activity_log.push_back(std::move(entry));
    while (subagent.activity_log.size() > 16) {
      subagent.activity_log.erase(subagent.activity_log.begin());
    }
  }
}

void StreamStateManager::handleAgentToolCallChunk(
    const shared::AgentToolCallChunk &e) {
  auto it_stream = streams_.find(e.agentId);
  if (it_stream != streams_.end() && it_stream->second.is_thinking) {
    auto elapsed =
        std::chrono::steady_clock::now() - it_stream->second.thinking_start;
    float secs = std::chrono::duration<float>(elapsed).count();
    it_stream->second.is_thinking = false;
    pushThinkingDuration(e.agentId, secs);
  }
  clearActiveLiveEntry(e.agentId);

  auto &view = tool_calls_[e.toolCallId];
  if (!view) {
    view = std::make_shared<ToolCallView>();
    view->toolCallId = e.toolCallId;
    view->agentId = e.agentId;
    timeline_.push_back(
        {TimelineEntry::Kind::ToolCall, e.toolCallId, "", e.agentId});
  }
  assignToolCallClusterId(e.agentId, e.toolCallId);
  view->phase = ToolPhase::Preparing;
  if (!e.nameDelta.empty()) {
    view->name += e.nameDelta;
  }
  view->args += e.argsDelta;

  auto it_sub = subagent_to_parent_tool_.find(e.agentId);
  if (it_sub != subagent_to_parent_tool_.end()) {
    auto it_parent = tool_calls_.find(it_sub->second);
    if (it_parent != tool_calls_.end() && it_parent->second) {
      auto phase = view->phase;
      std::string summary =
          firmius::shared::SummarizeToolCall(view->name, view->args, phase);
      auto &log = it_parent->second->subagent_tool_log;
      if (!summary.empty()) {
        // Search for existing entry with this toolCallId (for parallel subagents)
        auto it_entry = std::find_if(log.begin(), log.end(),
            [&e](const shared::SubagentToolLogEntry &entry) {
              return entry.toolCallId == e.toolCallId;
            });

        if (it_entry != log.end()) {
          // Update existing entry
          it_entry->summary = summary;
          it_entry->phase = phase;
          it_entry->name = view->name;
          it_entry->args = view->args;
        } else {
          // Create new entry
          shared::SubagentToolLogEntry entry;
          entry.summary = summary;
          entry.phase = phase;
          entry.toolCallId = e.toolCallId;
          entry.name = view->name;
          entry.args = view->args;
          log.push_back(entry);
          while (log.size() > 8)
            log.erase(log.begin());
        }

        auto it_state = subagent_state_.find(it_sub->second);
        if (it_state != subagent_state_.end()) {
          auto &activity = it_state->second.activity_log;
          auto it_activity = std::find_if(
              activity.begin(), activity.end(),
              [&e](const shared::SubagentToolLogEntry &entry) {
                return entry.toolCallId == e.toolCallId;
              });
          if (it_activity != activity.end()) {
            it_activity->summary = summary;
            it_activity->phase = phase;
            it_activity->name = view->name;
            it_activity->args = view->args;
          } else {
            shared::SubagentToolLogEntry activity_entry;
            activity_entry.summary = summary;
            activity_entry.phase = phase;
            activity_entry.toolCallId = e.toolCallId;
            activity_entry.name = view->name;
            activity_entry.args = view->args;
            activity.push_back(std::move(activity_entry));
            while (activity.size() > 16) {
              activity.erase(activity.begin());
            }
          }
        }
      }
    }
  }

  flushBufferedProcessOutputForToolCall(e.toolCallId);
}

void StreamStateManager::handleAgentToolCall(const shared::AgentToolCall &e) {
  clearActiveLiveEntry(e.agentId);
  auto &view = tool_calls_[e.toolCallId];
  if (!view) {
    view = std::make_shared<ToolCallView>();
    view->toolCallId = e.toolCallId;
    timeline_.push_back(
        {TimelineEntry::Kind::ToolCall, e.toolCallId, "", e.agentId});
  }
  assignToolCallClusterId(e.agentId, e.toolCallId);
  view->agentId = e.agentId;
  if (!e.toolName.empty())
    view->name = e.toolName;
  if (!e.toolArgs.empty())
    view->args = e.toolArgs;
  view->phase = ToolPhase::Called;
  if (!view->args.empty()) {
    ParsedToolArgs parsed_args = parseToolArgs(view->args);
    if (!parsed_args.process_id.empty()) {
      view->process_id = parsed_args.process_id;
      auto &process = process_state_[parsed_args.process_id];
      process.process_id = parsed_args.process_id;
      process.owner_agent_id = view->agentId;
      if (process.origin_tool_call_id.empty()) {
        process.origin_tool_call_id = view->toolCallId;
      }
      if (!parsed_args.command.empty() && process.command.empty()) {
        process.command = parsed_args.command;
      }
      if (!parsed_args.cwd.empty() && process.cwd.empty()) {
        process.cwd = parsed_args.cwd;
      }
      if (view->name == "process_wait") {
        process.waiting = true;
        process.wait_state = "waiting";
        process.waiting_pattern = parsed_args.pattern;
      }
    }
    if (view->name == "summon_subagent" || view->name == "subagent_wait") {
      ParsedSubagentArgs parsed_subagent_args = parseSubagentArgs(view->args);
      std::string parent_tool_id = view->toolCallId;
      if (view->name == "subagent_wait" && !parsed_subagent_args.agent_id.empty()) {
        auto it_parent = subagent_to_parent_tool_.find(parsed_subagent_args.agent_id);
        if (it_parent != subagent_to_parent_tool_.end()) {
          parent_tool_id = it_parent->second;
        }
      }
      auto &subagent = subagent_state_[parent_tool_id];
      subagent.parent_tool_call_id = parent_tool_id;
      subagent.owner_agent_id = view->agentId;
      subagent.waiting = (view->name == "subagent_wait");
      subagent.running = (view->phase == ToolPhase::Called);
      if (!parsed_subagent_args.task.empty()) {
        subagent.task = parsed_subagent_args.task;
      }
      if (!parsed_subagent_args.title.empty()) {
        subagent.child_title = parsed_subagent_args.title;
      } else if (!parsed_subagent_args.name.empty() &&
                 subagent.child_title.empty()) {
        subagent.child_title = parsed_subagent_args.name;
      }
      if (!parsed_subagent_args.agent_id.empty()) {
        subagent.child_agent_id = parsed_subagent_args.agent_id;
        subagent_to_parent_tool_[parsed_subagent_args.agent_id] = parent_tool_id;
      }
      if (!parsed_subagent_args.category.empty() && subagent.route_category.empty()) {
        subagent.route_category = parsed_subagent_args.category;
      }
      subagent.wait_state = "running";
      subagent_tool_to_parent_[view->toolCallId] = parent_tool_id;

      if (view->name == "subagent_wait") {
        view->subagent_id = subagent.child_agent_id;
        if (!subagent.child_title.empty()) {
          view->subagent_title = subagent.child_title;
        }
      }
    }
  }

  auto it_sub = subagent_to_parent_tool_.find(e.agentId);
  if (it_sub != subagent_to_parent_tool_.end()) {
    auto it_parent = tool_calls_.find(it_sub->second);
    if (it_parent != tool_calls_.end() && it_parent->second) {
      std::string summary = firmius::shared::SummarizeToolCall(
          view->name, view->args, view->phase);
      auto &log = it_parent->second->subagent_tool_log;
      if (!summary.empty()) {
        // Search for existing entry with this toolCallId (for parallel subagents)
        auto it_entry = std::find_if(log.begin(), log.end(),
            [&e](const shared::SubagentToolLogEntry &entry) {
              return entry.toolCallId == e.toolCallId;
            });

        if (it_entry != log.end()) {
          // Update existing entry
          it_entry->summary = summary;
          it_entry->phase = view->phase;
          it_entry->name = view->name;
          it_entry->args = view->args;
        } else {
          // Create new entry
          shared::SubagentToolLogEntry entry;
          entry.summary = summary;
          entry.phase = view->phase;
          entry.toolCallId = e.toolCallId;
          entry.name = view->name;
          entry.args = view->args;
          log.push_back(entry);
          while (log.size() > 8)
            log.erase(log.begin());
        }

        auto it_state = subagent_state_.find(it_sub->second);
        if (it_state != subagent_state_.end()) {
          auto &activity = it_state->second.activity_log;
          auto it_activity = std::find_if(
              activity.begin(), activity.end(),
              [&e](const shared::SubagentToolLogEntry &entry) {
                return entry.toolCallId == e.toolCallId;
              });
          if (it_activity != activity.end()) {
            it_activity->summary = summary;
            it_activity->phase = view->phase;
            it_activity->name = view->name;
            it_activity->args = view->args;
          } else {
            shared::SubagentToolLogEntry activity_entry;
            activity_entry.summary = summary;
            activity_entry.phase = view->phase;
            activity_entry.toolCallId = e.toolCallId;
            activity_entry.name = view->name;
            activity_entry.args = view->args;
            activity.push_back(std::move(activity_entry));
            while (activity.size() > 16) {
              activity.erase(activity.begin());
            }
          }
        }
      }
    }
  }

  flushBufferedProcessOutputForToolCall(e.toolCallId);
}

void StreamStateManager::handleAgentFileEdited(
    const shared::AgentFileEdited &e) {
  const std::string tool_call_id =
      !e.toolCallId.empty()
          ? e.toolCallId
          : ("file-edit-" + e.agentId + "-" +
             std::to_string(++next_live_entry_sequence_));

  auto &view = tool_calls_[tool_call_id];
  if (!view) {
    view = std::make_shared<ToolCallView>();
    view->toolCallId = tool_call_id;
    timeline_.push_back(
        {TimelineEntry::Kind::ToolCall, tool_call_id, "", e.agentId});
  }

  assignToolCallClusterId(e.agentId, tool_call_id);
  view->agentId = e.agentId;
  if (view->name.empty()) {
    view->name = "file_edit";
  }
  if (view->phase != ToolPhase::Error) {
    view->phase = ToolPhase::Called;
    view->success = true;
  }

  mergeFileEditSignal(*view, shared::FileEditSignal{
                                 e.path, e.diffPreview, e.addedLines,
                                 e.removedLines});
}

void StreamStateManager::handleAgentCompacting(
    const shared::AgentCompacting &e) {
  auto &s = streams_[e.agentId];
  s.compaction_active = true;
  s.compaction_finished = false;
  s.compaction_thinking.clear();
  s.compaction_text.clear();
  s.compaction_completion.clear();
  (void)e;
}

void StreamStateManager::handleAgentCompactionThinking(
    const shared::AgentCompactionThinking &e) {
  auto &s = streams_[e.agentId];
  s.compaction_active = true;
  s.compaction_finished = false;
  s.compaction_completion.clear();
  s.compaction_thinking += e.delta;
}

void StreamStateManager::handleAgentCompactionText(
    const shared::AgentCompactionText &e) {
  auto &s = streams_[e.agentId];
  s.compaction_active = true;
  s.compaction_finished = false;
  s.compaction_completion.clear();
  s.compaction_text += e.delta;
}

void StreamStateManager::handleContextCompacted(
    const shared::ContextCompacted &e) {
  auto &s = streams_[e.agentId];
  s.compaction_active = false;
  s.compaction_finished = false;
  s.compaction_thinking.clear();
  s.compaction_text.clear();
  s.compaction_completion.clear();
}

void StreamStateManager::handleAgentProcessSpawned(
    const shared::AgentProcessSpawned &e) {
  if (!e.toolCallId.empty()) {
    process_to_toolcall_[e.processId] = e.toolCallId;
  }
  process_to_agent_[e.processId] = e.agentId;

  auto &process = process_state_[e.processId];
  process.process_id = e.processId;
  process.owner_agent_id = e.agentId;
  process.origin_tool_call_id = e.toolCallId;
  process.command = e.command;
  process.running = true;
  process.finished = false;
  process.exit_code_known = false;
  process.exit_code = 0;
  process.duration_ms = 0.0;
  process.waiting = false;
  process.wait_state.clear();

  auto it_tool = tool_calls_.find(e.toolCallId);
  if (it_tool != tool_calls_.end() && it_tool->second) {
    auto &view = it_tool->second;
    if (view->process_id.empty()) {
      view->process_id = e.processId;
    }
    if (view->name == "process_spawn") {
      view->process_is_background = true;
      process.is_background = true;
      process.is_blocking = false;
      if (view->phase == ToolPhase::Called || view->phase == ToolPhase::Preparing) {
        view->phase = ToolPhase::BackgroundRunning;
      }
    } else {
      process.is_background = false;
      process.is_blocking = true;
    }
  }

  flushBufferedProcessOutputForProcess(e.processId);
}

void StreamStateManager::handleAgentProcessOutput(
    const shared::AgentProcessOutput &e) {
  if (process_to_agent_.count(e.processId) == 0 && !e.agentId.empty()) {
    process_to_agent_[e.processId] = e.agentId;
  }
  auto &process = process_state_[e.processId];
  process.process_id = e.processId;
  if (!e.agentId.empty()) {
    process.owner_agent_id = e.agentId;
  }
  if (e.finished) {
    process.running = false;
    process.finished = true;
    process.exit_code_known = true;
    process.exit_code = e.exitCode;
    process.duration_ms = e.durationMs;
    process.waiting = false;
    if (process.wait_state.empty()) {
      process.wait_state = "completed";
    }
  }

  if (!applyProcessOutputToToolView(e)) {
    pending_process_output_[e.processId].push_back(e);
  }
}

void StreamStateManager::handleAgentSpawned(
    const shared::AgentSpawned &e, const std::string &focused_agent_id) {
  if (!e.providerId.empty() || !e.modelId.empty()) {
    agent_provider_model_[e.agentId] = e.providerId + "/" + e.modelId;
  }
  if (!e.title.empty()) {
    agent_titles_[e.agentId] = e.title;
  }

  // Link spawned agent to parent tool call
  for (auto &pair : tool_calls_) {
    if (!pair.second || pair.second->agentId != e.parentId)
      continue;
    if (pair.second->name != "summon_subagent")
      continue;

    // Check if this tool call is likely the one that spawned this agent
    // We can use the agentId if the tool call reported it (async mode)
    // or check name/slug matches.
    bool id_match = (!pair.second->subagent_id.empty() &&
                     pair.second->subagent_id == e.agentId);
    bool name_match = (!pair.second->subagent_slug.empty() &&
                       pair.second->subagent_slug == e.friendlyName);

    // If we haven't linked a subagent_id yet, and it's a name match, take it.
    if (pair.second->subagent_id.empty() || id_match || name_match) {
      subagent_to_parent_tool_[e.agentId] = pair.first;
      subagent_tool_to_parent_[pair.first] = pair.first;
      pair.second->subagent_running = true;
      pair.second->phase = ToolPhase::Called;
      pair.second->subagent_id = e.agentId;
      if (!e.title.empty()) {
        pair.second->subagent_title = e.title;
      }
      if (!e.friendlyName.empty())
        pair.second->subagent_slug = e.friendlyName;

      auto &subagent = subagent_state_[pair.first];
      subagent.parent_tool_call_id = pair.first;
      subagent.owner_agent_id = pair.second->agentId;
      subagent.child_agent_id = e.agentId;
      subagent.child_friendly_name = e.friendlyName;
      subagent.running = true;
      subagent.waiting = false;
      subagent.provider_waiting = false;
      subagent.retrying = false;
      subagent.account_switched = false;
      subagent.wait_state = "running";
      if (!e.title.empty()) {
        subagent.child_title = e.title;
      }
      if (!e.friendlyName.empty()) {
        subagent.activity_log.push_back(
            {"Spawned " + e.friendlyName, shared::ToolPhase::Finished, "", "", ""});
      }
      while (subagent.activity_log.size() > 16) {
        subagent.activity_log.erase(subagent.activity_log.begin());
      }
      break;
    }
  }
  (void)focused_agent_id;
}

void StreamStateManager::pushThinkingDuration(const std::string &agentId,
                                              float seconds) {
  auto it_sub = subagent_to_parent_tool_.find(agentId);
  if (it_sub == subagent_to_parent_tool_.end())
    return;
  auto it_parent = tool_calls_.find(it_sub->second);
  if (it_parent == tool_calls_.end() || !it_parent->second)
    return;

  std::string label = "Thought for " + formatDuration(seconds);
  auto &log = it_parent->second->subagent_tool_log;
  shared::SubagentToolLogEntry entry;
  entry.summary = label;
  entry.phase = shared::ToolPhase::Finished;
  entry.toolCallId = "";
  log.push_back(entry);
  while (log.size() > 8)
    log.erase(log.begin());
  auto it_state = subagent_state_.find(it_sub->second);
  if (it_state != subagent_state_.end()) {
    it_state->second.activity_log.push_back(entry);
    while (it_state->second.activity_log.size() > 16) {
      it_state->second.activity_log.erase(it_state->second.activity_log.begin());
    }
  }
}

void StreamStateManager::pushTokenUsage(const std::string &agentId,
                                        const shared::AgentMetrics &metrics) {
  latest_metrics_[agentId] = metrics;
}

void StreamStateManager::reactivateSubagentParentView(
    const std::string &agentId) {
  auto it_sub = subagent_to_parent_tool_.find(agentId);
  if (it_sub == subagent_to_parent_tool_.end()) {
    return;
  }
  auto it_parent = tool_calls_.find(it_sub->second);
  if (it_parent == tool_calls_.end() || !it_parent->second) {
    return;
  }

  auto &parentView = it_parent->second;
  parentView->subagent_running = true;
  parentView->phase = ToolPhase::Called;
  auto it_state = subagent_state_.find(it_sub->second);
  if (it_state != subagent_state_.end()) {
    it_state->second.running = true;
    it_state->second.provider_waiting = true;
    it_state->second.wait_state = "provider_waiting";
  }
}

void StreamStateManager::handleAgentFinished(const shared::AgentFinished &e) {
  auto it_sub = subagent_to_parent_tool_.find(e.agentId);
  if (it_sub != subagent_to_parent_tool_.end()) {
    auto it_parent = tool_calls_.find(it_sub->second);
    if (it_parent != tool_calls_.end() && it_parent->second) {
      auto &parentView = it_parent->second;
      if (e.outcome.kind == shared::AgentOutcome::Kind::Failed &&
          parentView->phase == ToolPhase::Error) {
        parentView->subagent_running = false;
        return;
      }

      shared::SubagentToolLogEntry entry;
      switch (e.outcome.kind) {
      case shared::AgentOutcome::Kind::Response:
        entry.summary = "Done";
        break;
      case shared::AgentOutcome::Kind::NoSummary:
        entry.summary = "Done (no summary)";
        break;
      case shared::AgentOutcome::Kind::Cancelled:
        entry.summary = "Cancelled";
        break;
      case shared::AgentOutcome::Kind::Failed:
        entry.summary = e.outcome.text.empty()
                             ? "Failed"
                             : "Failed: " + e.outcome.text;
        break;
      }
      entry.phase = e.outcome.kind == shared::AgentOutcome::Kind::Failed
                        ? shared::ToolPhase::Error
                        : shared::ToolPhase::Finished;
      entry.toolCallId = "";
      parentView->subagent_tool_log.push_back(entry);
      parentView->subagent_running = false;
      parentView->phase =
          e.outcome.kind == shared::AgentOutcome::Kind::Failed
              ? ToolPhase::Error
              : ToolPhase::Finished;

      auto &subagent = subagent_state_[it_sub->second];
      subagent.parent_tool_call_id = it_sub->second;
      subagent.owner_agent_id = parentView->agentId;
      subagent.running = false;
      subagent.waiting = false;
      subagent.provider_waiting = false;
      subagent.retrying = false;
      subagent.account_switched = false;
      subagent.activity_log.push_back(entry);
      switch (e.outcome.kind) {
      case shared::AgentOutcome::Kind::Response:
        subagent.outcome = SubagentOutcomeKind::Response;
        subagent.wait_state = "completed";
        break;
      case shared::AgentOutcome::Kind::NoSummary:
        subagent.outcome = SubagentOutcomeKind::NoSummary;
        subagent.wait_state = "completed_no_summary";
        break;
      case shared::AgentOutcome::Kind::Cancelled:
        subagent.outcome = SubagentOutcomeKind::Cancelled;
        subagent.wait_state = "cancelled";
        break;
      case shared::AgentOutcome::Kind::Failed:
        subagent.outcome = SubagentOutcomeKind::Failed;
        subagent.wait_state = "failed";
        break;
      }
      subagent.final_summary = e.outcome.text;
      subagent.error_text =
          e.outcome.kind == shared::AgentOutcome::Kind::Failed ? e.outcome.text : "";
      subagent.artifacts_created.clear();
      subagent.artifacts_updated.clear();
      for (const auto &artifact : e.outcome.artifacts_created) {
        if (!artifact.filename.empty()) {
          const std::string owner =
              artifact.ownerFriendlyName.empty() ? artifact.ownerAgentId
                                                 : artifact.ownerFriendlyName;
          subagent.artifacts_created.push_back("@artifact:" + owner + "/" +
                                               artifact.filename);
        } else if (!artifact.storagePath.empty()) {
          subagent.artifacts_created.push_back(artifact.storagePath);
        }
      }
      for (const auto &artifact : e.outcome.artifacts_updated) {
        if (!artifact.filename.empty()) {
          const std::string owner =
              artifact.ownerFriendlyName.empty() ? artifact.ownerAgentId
                                                 : artifact.ownerFriendlyName;
          subagent.artifacts_updated.push_back("@artifact:" + owner + "/" +
                                               artifact.filename);
        } else if (!artifact.storagePath.empty()) {
          subagent.artifacts_updated.push_back(artifact.storagePath);
        }
      }
      while (subagent.activity_log.size() > 16) {
        subagent.activity_log.erase(subagent.activity_log.begin());
      }
    }
  }
}

void StreamStateManager::handleAgentInterrupted(
    const shared::AgentInterrupted &e) {
  auto &s = streams_[e.agentId];
  s.thinking.clear();
  s.text.clear();
  s.compaction_finished = false;
  s.compaction_completion.clear();
  s.provider_waiting = false;
  s.is_thinking = false;
  clearActiveLiveEntry(e.agentId);

  std::unordered_set<std::string> erased_tool_ids;
  for (auto it = tool_calls_.begin(); it != tool_calls_.end();) {
    auto &view = it->second;
    if (!view || view->agentId != e.agentId ||
        (view->phase != ToolPhase::Preparing &&
         view->phase != ToolPhase::Called &&
         view->phase != ToolPhase::BackgroundRunning)) {
      ++it;
      continue;
    }

    const bool hasMeaningfulToolIdentity =
        !view->toolCallId.empty() && (!view->name.empty() || !view->args.empty());
    const bool isPreparationOnly = view->phase == ToolPhase::Preparing &&
                                   view->args.empty() && view->result.empty();
    if (!hasMeaningfulToolIdentity || isPreparationOnly) {
      erased_tool_ids.insert(it->first);
      it = tool_calls_.erase(it);
      continue;
    }

    applyToolResult(view, false, "User aborted tool manually.");
    ++it;
  }

  timeline_.erase(
      std::remove_if(
          timeline_.begin(), timeline_.end(), [&](const TimelineEntry &entry) {
            if (entry.agentId != e.agentId) {
              return false;
            }
            if (entry.kind == TimelineEntry::Kind::Thinking ||
                entry.kind == TimelineEntry::Kind::Text) {
              return true;
            }
            return entry.kind == TimelineEntry::Kind::ToolCall &&
                   erased_tool_ids.count(entry.id) > 0;
          }),
      timeline_.end());
  for (auto it = tool_call_cluster_ids_.begin();
       it != tool_call_cluster_ids_.end();) {
    if (erased_tool_ids.count(it->first) > 0) {
      it = tool_call_cluster_ids_.erase(it);
    } else {
      ++it;
    }
  }
  live_quick_clusters_[e.agentId] = {};
  clearRetryStatus();
}

void StreamStateManager::handleAgentError(const shared::AgentError &e) {
  auto &s = streams_[e.agentId];
  s.thinking.clear();
  s.text.clear();
  s.compaction_finished = false;
  s.compaction_completion.clear();
  s.provider_waiting = false;
  s.is_thinking = false;
  clearActiveLiveEntry(e.agentId);
  timeline_.erase(
      std::remove_if(
          timeline_.begin(), timeline_.end(), [&](const TimelineEntry &entry) {
            return entry.agentId == e.agentId &&
                   (entry.kind == TimelineEntry::Kind::Thinking ||
                    entry.kind == TimelineEntry::Kind::Text);
          }),
      timeline_.end());
  live_quick_clusters_[e.agentId] = {};
  clearRetryStatus();

  const bool hideErrors = firmius::core::Harness::instance().getConfig().hideErrors;
  appendErrorToTimelineIfRelevant(timeline_, next_live_entry_sequence_,
                                  e.agentId, e.message, hideErrors);

  auto it_sub = subagent_to_parent_tool_.find(e.agentId);
  if (it_sub == subagent_to_parent_tool_.end()) {
    return;
  }
  auto it_parent = tool_calls_.find(it_sub->second);
  if (it_parent == tool_calls_.end() || !it_parent->second) {
    return;
  }

  auto &parentView = it_parent->second;
  parentView->subagent_running = false;
  parentView->phase = ToolPhase::Error;
  shared::SubagentToolLogEntry entry;
  entry.summary = "Failed: " + e.message;
  entry.phase = shared::ToolPhase::Error;
  const auto activityEntry = entry;
  parentView->subagent_tool_log.push_back(std::move(entry));
  while (parentView->subagent_tool_log.size() > 8) {
    parentView->subagent_tool_log.erase(parentView->subagent_tool_log.begin());
  }

  auto &subagent = subagent_state_[it_sub->second];
  subagent.parent_tool_call_id = it_sub->second;
  subagent.owner_agent_id = parentView->agentId;
  subagent.running = false;
  subagent.waiting = false;
  subagent.provider_waiting = false;
  subagent.retrying = false;
  subagent.account_switched = false;
  subagent.outcome = SubagentOutcomeKind::Failed;
  subagent.wait_state = "failed";
  subagent.error_text = e.message;
  subagent.activity_log.push_back(activityEntry);
  while (subagent.activity_log.size() > 16) {
    subagent.activity_log.erase(subagent.activity_log.begin());
  }
}

const shared::AgentMetrics *
StreamStateManager::getLatestMetrics(const std::string &agentId) const {
  auto it = latest_metrics_.find(agentId);
  if (it == latest_metrics_.end()) {
    return nullptr;
  }
  return &it->second;
}

const std::vector<CompletedRunSummary> *
StreamStateManager::getCompletedRunSummaries(const std::string &agentId) const {
  auto it = completed_run_summaries_.find(agentId);
  if (it == completed_run_summaries_.end()) {
    return nullptr;
  }
  return &it->second;
}

const StreamState *
StreamStateManager::getStream(const std::string &agentId) const {
  auto it = streams_.find(agentId);
  if (it == streams_.end())
    return nullptr;
  return &it->second;
}

const std::vector<TimelineEntry> &StreamStateManager::getTimeline() const {
  return timeline_;
}

const std::unordered_map<std::string, std::shared_ptr<ToolCallView>> &
StreamStateManager::getToolCalls() const {
  return tool_calls_;
}

std::shared_ptr<ToolCallView>
StreamStateManager::getToolView(const std::string &toolCallId) const {
  auto it = tool_calls_.find(toolCallId);
  if (it != tool_calls_.end())
    return it->second;
  return nullptr;
}

const NormalizedProcessState *
StreamStateManager::getProcessState(const std::string &processId) const {
  auto it = process_state_.find(processId);
  if (it == process_state_.end()) {
    return nullptr;
  }
  return &it->second;
}

const NormalizedProcessState *
StreamStateManager::getProcessStateForToolCall(const std::string &toolCallId) const {
  if (toolCallId.empty()) {
    return nullptr;
  }
  for (const auto &[_, state] : process_state_) {
    if (state.origin_tool_call_id == toolCallId) {
      return &state;
    }
  }

  auto it_view = tool_calls_.find(toolCallId);
  if (it_view != tool_calls_.end() && it_view->second) {
    const auto &view = it_view->second;
    if (!view->process_id.empty()) {
      auto it_process = process_state_.find(view->process_id);
      if (it_process != process_state_.end()) {
        return &it_process->second;
      }
    }
    ParsedToolArgs parsed_args = parseToolArgs(view->args);
    if (!parsed_args.process_id.empty()) {
      auto it_process = process_state_.find(parsed_args.process_id);
      if (it_process != process_state_.end()) {
        return &it_process->second;
      }
    }
  }
  return nullptr;
}

const NormalizedSubagentState *
StreamStateManager::getSubagentState(
    const std::string &parentToolCallId) const {
  auto it = subagent_state_.find(parentToolCallId);
  if (it == subagent_state_.end()) {
    return nullptr;
  }
  return &it->second;
}

const NormalizedSubagentState *
StreamStateManager::getSubagentStateForToolCall(
    const std::string &toolCallId) const {
  if (toolCallId.empty()) {
    return nullptr;
  }
  auto it_direct = subagent_tool_to_parent_.find(toolCallId);
  if (it_direct != subagent_tool_to_parent_.end()) {
    auto it_state = subagent_state_.find(it_direct->second);
    if (it_state != subagent_state_.end()) {
      return &it_state->second;
    }
  }

  auto it_state = subagent_state_.find(toolCallId);
  if (it_state != subagent_state_.end()) {
    return &it_state->second;
  }

  auto it_view = tool_calls_.find(toolCallId);
  if (it_view != tool_calls_.end() && it_view->second) {
    const auto &view = it_view->second;
    if (!view->subagent_id.empty()) {
      auto it_parent = subagent_to_parent_tool_.find(view->subagent_id);
      if (it_parent != subagent_to_parent_tool_.end()) {
        auto it_state2 = subagent_state_.find(it_parent->second);
        if (it_state2 != subagent_state_.end()) {
          return &it_state2->second;
        }
      }
    }
    ParsedSubagentArgs parsed_args = parseSubagentArgs(view->args);
    if (!parsed_args.agent_id.empty()) {
      auto it_parent = subagent_to_parent_tool_.find(parsed_args.agent_id);
      if (it_parent != subagent_to_parent_tool_.end()) {
        auto it_state2 = subagent_state_.find(it_parent->second);
        if (it_state2 != subagent_state_.end()) {
          return &it_state2->second;
        }
      }
    }
  }
  return nullptr;
}

std::string StreamStateManager::getAgentTitle(
    const std::string &agentId) const {
  auto it = agent_titles_.find(agentId);
  if (it != agent_titles_.end())
    return it->second;
  return "";
}

ProcessCounts StreamStateManager::getProcessCounts(
    const std::string &agentId) const {
  return getProcessCounts(agentId, nullptr, {});
}

ProcessCounts StreamStateManager::getProcessCounts(
    const std::string &agentId, const ProcessRuntimeSnapshot *runtime_snapshot,
    const std::function<bool(const std::string &)> &is_process_running) const {
  ProcessCounts counts;
  std::unordered_map<std::string, NormalizedProcessState> effective;

  for (const auto &[process_id, state] : process_state_) {
    if (state.owner_agent_id == agentId) {
      effective[process_id] = state;
    }
  }

  if (runtime_snapshot) {
    std::unordered_set<std::string> blocking(
        runtime_snapshot->blocking_process_ids.begin(),
        runtime_snapshot->blocking_process_ids.end());
    for (const auto &process_id : runtime_snapshot->owned_process_ids) {
      auto &state = effective[process_id];
      state.process_id = process_id;
      state.owner_agent_id = agentId;
      state.running = true;
      state.finished = false;
      state.is_blocking = blocking.count(process_id) > 0;
      state.is_background = !state.is_blocking;
    }
    for (const auto &process_id : blocking) {
      auto &state = effective[process_id];
      state.process_id = process_id;
      state.owner_agent_id = agentId;
      state.running = true;
      state.finished = false;
      state.is_blocking = true;
      state.is_background = false;
    }
  }

  for (auto &[process_id, state] : effective) {
    if (is_process_running) {
      const bool running = is_process_running(process_id);
      state.running = running;
      state.finished = !running;
    }
    if (!state.running || state.finished) {
      continue;
    }
    if (state.is_background && !state.is_blocking) {
      ++counts.background;
    } else {
      ++counts.live;
    }
  }
  return counts;
}

// Transient error rendering is disabled; errors are now rendered
// persistently via ChatWindow using AgentHistory ErrorContent.

void StreamStateManager::handleAgentRetrying(const shared::AgentRetrying &e) {
  retry_status_ = "Retrying (" + std::to_string(e.attempt) + "/" +
                  std::to_string(e.maxAttempts) + ", HTTP " +
                  std::to_string(e.httpStatus) + ", " + e.reason + ", ~" +
                  std::to_string(e.delayMs / 1000) + "s)";
  if (!e.accountLocator.empty()) {
    retry_status_ += " [Account: " + e.accountLocator + "]";
  }
  appendErrorToTimelineIfRelevant(timeline_, next_live_entry_sequence_,
                                  e.agentId, e.details);
  reactivateSubagentParentView(e.agentId);
  auto it_parent = subagent_to_parent_tool_.find(e.agentId);
  if (it_parent != subagent_to_parent_tool_.end()) {
    auto &subagent = subagent_state_[it_parent->second];
    subagent.retrying = true;
    subagent.provider_waiting = false;
    subagent.account_switched = false;
    subagent.running = true;
    subagent.wait_state = "retrying";
    shared::SubagentToolLogEntry entry;
    entry.summary = "Retrying (" + std::to_string(e.attempt) + "/" +
                    std::to_string(e.maxAttempts) + ")";
    entry.phase = shared::ToolPhase::Preparing;
    subagent.activity_log.push_back(std::move(entry));
    while (subagent.activity_log.size() > 16) {
      subagent.activity_log.erase(subagent.activity_log.begin());
    }
  }
}

void StreamStateManager::handleAgentAccountSwitched(
    const shared::AgentAccountSwitched &e) {
  account_swaps_.push_back("[Account Switch] -> " + e.accountLocator);
  reactivateSubagentParentView(e.agentId);
  auto it_parent = subagent_to_parent_tool_.find(e.agentId);
  if (it_parent != subagent_to_parent_tool_.end()) {
    auto &subagent = subagent_state_[it_parent->second];
    subagent.account_switched = true;
    subagent.provider_waiting = false;
    subagent.retrying = false;
    subagent.running = true;
    subagent.wait_state = "account_switched";
    shared::SubagentToolLogEntry entry;
    entry.summary = "Account switched to " + e.accountLocator;
    entry.phase = shared::ToolPhase::Finished;
    subagent.activity_log.push_back(std::move(entry));
    while (subagent.activity_log.size() > 16) {
      subagent.activity_log.erase(subagent.activity_log.begin());
    }
  }
}

void StreamStateManager::handleAgentRetryFailed(
    const shared::AgentRetryFailed &e) {
  clearRetryStatus();
  auto it_parent = subagent_to_parent_tool_.find(e.agentId);
  if (it_parent != subagent_to_parent_tool_.end()) {
    auto &subagent = subagent_state_[it_parent->second];
    subagent.retrying = false;
    subagent.provider_waiting = false;
    subagent.running = false;
    subagent.wait_state = "failed";
    if (!e.reason.empty()) {
      subagent.error_text = e.reason;
      shared::SubagentToolLogEntry entry;
      entry.summary = "Retry failed: " + e.reason;
      entry.phase = shared::ToolPhase::Error;
      subagent.activity_log.push_back(std::move(entry));
      while (subagent.activity_log.size() > 16) {
        subagent.activity_log.erase(subagent.activity_log.begin());
      }
    }
  }
  // Transient error rendering is disabled.
}

const std::string &StreamStateManager::getRetryStatus() const {
  return retry_status_;
}

const std::vector<std::string> &StreamStateManager::getAccountSwaps() const {
  return account_swaps_;
}

void StreamStateManager::handleMessageQueued(const shared::MessageQueued &e) {
  queued_messages_.push_back(
      {e.messageId, e.text, e.threadId, e.agentId});
}

void StreamStateManager::handleMessageDequeued(
    const shared::MessageDequeued &e) {
  queued_messages_.erase(std::remove_if(queued_messages_.begin(),
                                        queued_messages_.end(),
                                        [&](const QueuedMessageEntry &entry) {
                                          return entry.message_id == e.messageId;
                                        }),
                         queued_messages_.end());
}

void StreamStateManager::handleInternalMessageQueued(
    const shared::InternalMessageQueued &e) {
  queued_internal_messages_.push_back(
      {e.messageId, e.text, e.threadId, e.agentId});
}

void StreamStateManager::handleInternalMessageDequeued(
    const shared::InternalMessageDequeued &e) {
  queued_internal_messages_.erase(std::remove_if(queued_internal_messages_.begin(),
                                        queued_internal_messages_.end(),
                                        [&](const QueuedMessageEntry &entry) {
                                          return entry.message_id == e.messageId;
                                        }),
                         queued_internal_messages_.end());
}

void StreamStateManager::handleThreadChanged() {
  queued_messages_.clear();
  queued_internal_messages_.clear();
  // Clear live tool calls - they will be rebuilt from history
  tool_calls_.clear();
  timeline_.clear();
  subagent_to_parent_tool_.clear();
  subagent_state_.clear();
  subagent_tool_to_parent_.clear();
  streams_.clear();
  process_to_toolcall_.clear();
  process_to_agent_.clear();
  process_state_.clear();
  last_todo_result_by_agent_.clear();
  pending_process_output_.clear();
  live_quick_clusters_.clear();
  tool_call_cluster_ids_.clear();
}

void StreamStateManager::rebuildToolCallsFromHistory(
    const std::string &agentId, const shared::AgentHistory *history,
    const std::string &threadId, bool populate_subagent_log) {
  if (!history)
    return;

  // First pass: collect all tool results to know success/failure.
  // Historical turns may embed ToolResultContent under different message roles,
  // so inspect every message part rather than relying on msg.role.
  std::unordered_map<std::string, std::pair<bool, std::string>> toolResults;
  for (const auto &turn : history->turns) {
    for (const auto &msg : turn.messages) {
      for (const auto &content : msg.content) {
        if (auto *tr = std::get_if<shared::ToolResultContent>(&content)) {
          toolResults[tr->toolCallId] = {tr->success, tr->result};
        }
      }
    }
  }

  // Second pass: extract tool calls and set state based on results
  // For historical data, assume success unless we have explicit error
  for (const auto &turn : history->turns) {
    for (const auto &msg : turn.messages) {
      if (msg.role == shared::Role::Assistant) {
        for (const auto &content : msg.content) {
          if (auto *tc = std::get_if<shared::ToolCallContent>(&content)) {
            auto &view = tool_calls_[tc->id];
            if (!view) {
              view = std::make_shared<ToolCallView>();
              view->toolCallId = tc->id;
              view->agentId = agentId;
              view->name = tc->name;
              view->args = tc->args;
              view->subagent_running = false;

              // Check if we have a result for this tool call
              auto it = toolResults.find(tc->id);
              if (it != toolResults.end()) {
                // We have explicit result
                applyToolResult(view, it->second.first, it->second.second);
              } else {
                // No explicit result - for historical data, assume success
                // This handles summon_subagent and other tools that may not have
                // explicit ToolResultContent but completed successfully
                view->success = true;
                view->phase = ToolPhase::Finished;
              }

              if (view->name == "summon_subagent" || view->name == "subagent_wait") {
                ParsedSubagentArgs parsed_args = parseSubagentArgs(view->args);
                std::string parent_tool_id = tc->id;
                if (view->name == "subagent_wait" && !parsed_args.agent_id.empty()) {
                  auto it_parent = subagent_to_parent_tool_.find(parsed_args.agent_id);
                  if (it_parent != subagent_to_parent_tool_.end()) {
                    parent_tool_id = it_parent->second;
                  }
                }
                auto &subagent = subagent_state_[parent_tool_id];
                subagent.parent_tool_call_id = parent_tool_id;
                subagent.owner_agent_id = agentId;
                subagent_tool_to_parent_[tc->id] = parent_tool_id;
                if (!parsed_args.title.empty()) {
                  subagent.child_title = parsed_args.title;
                } else if (!parsed_args.name.empty() && subagent.child_title.empty()) {
                  subagent.child_title = parsed_args.name;
                }
                if (!parsed_args.task.empty()) {
                  subagent.task = parsed_args.task;
                }
                if (!parsed_args.agent_id.empty()) {
                  subagent.child_agent_id = parsed_args.agent_id;
                  subagent_to_parent_tool_[parsed_args.agent_id] = parent_tool_id;
                  view->subagent_id = parsed_args.agent_id;
                }
                if (!subagent.child_title.empty()) {
                  view->subagent_title = subagent.child_title;
                }

                // Only backfill generic terminal state when no typed state was
                // established from parsed tool result.
                if (subagent.wait_state.empty()) {
                  subagent.running = false;
                  subagent.waiting = false;
                  subagent.provider_waiting = false;
                  subagent.retrying = false;
                  subagent.account_switched = false;
                  subagent.wait_state = view->success ? "completed" : "failed";
                  if (view->success) {
                    subagent.outcome = SubagentOutcomeKind::Response;
                  } else {
                    subagent.outcome = SubagentOutcomeKind::Failed;
                  }
                }
              }

              auto it_sub = subagent_to_parent_tool_.find(agentId);
              if (it_sub != subagent_to_parent_tool_.end()) {
                auto it_parent = tool_calls_.find(it_sub->second);
                if (it_parent != tool_calls_.end() && it_parent->second) {
                  auto it_result = toolResults.find(tc->id);
                  const bool success =
                      it_result != toolResults.end() ? it_result->second.first : true;
                  const std::string result =
                      it_result != toolResults.end() ? it_result->second.second : "";
                  const std::string summary = summarizeHistoricalToolEntry(
                      tc->name, tc->args, result, success);

                  if (!summary.empty()) {
                    shared::SubagentToolLogEntry entry;
                    entry.summary = summary;
                    entry.phase = success ? shared::ToolPhase::Finished
                                          : shared::ToolPhase::Error;
                    entry.toolCallId = tc->id;
                    entry.name = tc->name;
                    entry.args = tc->args;
                    it_parent->second->subagent_tool_log.push_back(entry);
                    while (it_parent->second->subagent_tool_log.size() > 8) {
                      it_parent->second->subagent_tool_log.erase(
                          it_parent->second->subagent_tool_log.begin());
                    }

                    auto &activity = subagent_state_[it_sub->second].activity_log;
                    activity.push_back(std::move(entry));
                    while (activity.size() > 16) {
                      activity.erase(activity.begin());
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  auto relink_subagent_parent = [&](const std::string &from_parent_tool_id,
                                    const std::string &to_parent_tool_id) {
    if (from_parent_tool_id.empty() || to_parent_tool_id.empty() ||
        from_parent_tool_id == to_parent_tool_id) {
      return;
    }
    auto source_it = subagent_state_.find(from_parent_tool_id);
    if (source_it == subagent_state_.end()) {
      return;
    }

    auto &target = subagent_state_[to_parent_tool_id];
    if (target.parent_tool_call_id.empty()) {
      target.parent_tool_call_id = to_parent_tool_id;
    }
    if (target.owner_agent_id.empty()) {
      target.owner_agent_id = agentId;
    }
    MergeSubagentState(target, source_it->second);

    for (auto &[tool_call_id, parent_tool_id] : subagent_tool_to_parent_) {
      if (parent_tool_id == from_parent_tool_id) {
        parent_tool_id = to_parent_tool_id;
      }
    }
    for (auto &[child_agent_id, parent_tool_id] : subagent_to_parent_tool_) {
      if (parent_tool_id == from_parent_tool_id) {
        parent_tool_id = to_parent_tool_id;
      }
    }

    subagent_state_.erase(source_it);
  };

  // Re-link subagent_wait calls to their summon_subagent parent once all
  // historical results have been parsed. This keeps one normalized subagent
  // truth record even when wait calls appear before summon calls in history.
  for (const auto &[tool_call_id, view] : tool_calls_) {
    if (!view || view->name != "subagent_wait") {
      continue;
    }
    ParsedSubagentArgs parsed_args = parseSubagentArgs(view->args);
    const std::string child_agent_id =
        !view->subagent_id.empty() ? view->subagent_id : parsed_args.agent_id;
    if (child_agent_id.empty()) {
      continue;
    }
    auto parent_it = subagent_to_parent_tool_.find(child_agent_id);
    if (parent_it == subagent_to_parent_tool_.end()) {
      continue;
    }
    const std::string canonical_parent_tool_id = parent_it->second;
    const std::string current_parent_tool_id =
        subagent_tool_to_parent_.count(tool_call_id) > 0
            ? subagent_tool_to_parent_[tool_call_id]
            : tool_call_id;
    relink_subagent_parent(current_parent_tool_id, canonical_parent_tool_id);
    subagent_tool_to_parent_[tool_call_id] = canonical_parent_tool_id;
  }

  // Third pass: populate subagent_tool_log for summon_subagent tool calls
  // by analyzing the history of spawned subagents (only if requested)
  if (!populate_subagent_log)
    return;
    
  for (auto &[toolCallId, view] : tool_calls_) {
    if (!view || view->name != "summon_subagent" || view->args.empty())
      continue;

    // Parse args to extract agent_id or agent name
    rapidjson::Document doc;
    doc.Parse(view->args.c_str());
    if (doc.HasParseError() || !doc.IsObject())
      continue;

    std::string subagentId;
    std::string subagentTitle;
    std::string subagentTask;

    if (doc.HasMember("title") && doc["title"].IsString()) {
      subagentTitle = doc["title"].GetString();
    }
    if (doc.HasMember("task") && doc["task"].IsString()) {
      subagentTask = doc["task"].GetString();
    }
    if (doc.HasMember("name") && doc["name"].IsString()) {
      if (subagentTitle.empty())
        subagentTitle = doc["name"].GetString();
    }

    // Also check the tool result for agentId
    auto it_result = toolResults.find(toolCallId);
    if (it_result != toolResults.end() && !it_result->second.second.empty()) {
      rapidjson::Document resDoc;
      resDoc.Parse(it_result->second.second.c_str());
      if (!resDoc.HasParseError() && resDoc.IsObject() &&
          resDoc.HasMember("agentId") && resDoc["agentId"].IsString()) {
        subagentId = resDoc["agentId"].GetString();
      }
    }

    // Store title and subagent ID in view
    if (!subagentTitle.empty()) {
      view->subagent_title = subagentTitle;
    }
    if (!subagentId.empty()) {
      view->subagent_id = subagentId;
    }
    auto &subagent = subagent_state_[toolCallId];
    subagent.parent_tool_call_id = toolCallId;
    subagent.owner_agent_id = view->agentId;
    if (!subagentTitle.empty()) {
      subagent.child_title = subagentTitle;
    }
    if (!subagentTask.empty()) {
      subagent.task = subagentTask;
    }
    if (!subagentId.empty()) {
      subagent.child_agent_id = subagentId;
      subagent_to_parent_tool_[subagentId] = toolCallId;
    }
    subagent_tool_to_parent_[toolCallId] = toolCallId;

    // If we have a subagent ID, try to get its history and synthesize the log
    if (!subagentId.empty()) {
      // Register the mapping from subagent to parent tool call
      subagent_to_parent_tool_[subagentId] = toolCallId;

      std::unique_ptr<shared::AgentHistory> fallbackHistory;
      const shared::AgentHistory* subHistory = nullptr;
      if (!threadId.empty()) {
        auto persisted =
            firmius::core::ThreadManager(
                firmius::core::ThreadManager::defaultBasePath())
                .loadAgentHistory(threadId, subagentId);
        if (!persisted.turns.empty()) {
          fallbackHistory =
              std::make_unique<shared::AgentHistory>(std::move(persisted));
          subHistory = fallbackHistory.get();
        }
      }
      if (!subHistory) {
        auto &harness = firmius::core::Harness::instance();
        auto subHistoryPtr = harness.getAgentHistoryPtr(subagentId);
        if (subHistoryPtr) {
          subHistory = subHistoryPtr.get();
        }
      }

      if (subHistory && !subHistory->turns.empty()) {
        auto logEntries =
            synthesizeHistoricalSubagentLog(*subHistory, subagentTask, subagent);
        while (logEntries.size() > 8) {
          logEntries.erase(logEntries.begin());
        }

        auto choose_richer_log = [&](std::vector<shared::SubagentToolLogEntry> generated)
            -> std::vector<shared::SubagentToolLogEntry> {
          const auto is_informative = [](const shared::SubagentToolLogEntry &entry) {
            if (!entry.toolCallId.empty()) {
              return true;
            }
            return entry.summary.rfind("Task: ", 0) != 0 &&
                   entry.summary != "Done";
          };
          size_t generated_informative = 0;
          for (const auto &entry : generated) {
            if (is_informative(entry)) {
              ++generated_informative;
            }
          }
          size_t existing_informative = 0;
          for (const auto &entry : subagent.activity_log) {
            if (is_informative(entry)) {
              ++existing_informative;
            }
          }
          if (existing_informative > generated_informative ||
              (existing_informative == generated_informative &&
               subagent.activity_log.size() > generated.size())) {
            return subagent.activity_log;
          }
          return generated;
        };

        auto merged_log = choose_richer_log(std::move(logEntries));
        if (auto terminal = terminalSubagentLogEntry(subagent);
            terminal.has_value()) {
          const bool has_terminal =
              !merged_log.empty() &&
              merged_log.back().summary == terminal->summary;
          if (!has_terminal) {
            merged_log.push_back(*terminal);
          }
        }
        while (merged_log.size() > 8) {
          merged_log.erase(merged_log.begin());
        }
        view->subagent_tool_log = merged_log;
        view->subagent_running = false;
        subagent.activity_log = std::move(merged_log);
        subagent.running = false;
        subagent.waiting = false;
        subagent.provider_waiting = false;
        subagent.retrying = false;
        subagent.account_switched = false;
        if (subagent.wait_state.empty()) {
          subagent.wait_state = view->success ? "completed" : "failed";
          if (view->success) {
            subagent.outcome = SubagentOutcomeKind::Response;
          } else {
            subagent.outcome = SubagentOutcomeKind::Failed;
          }
        }
      } else {
        // No subagent history available - create minimal log
        std::vector<shared::SubagentToolLogEntry> logEntries;
        
        if (!subagentTask.empty()) {
          shared::SubagentToolLogEntry entry;
          entry.summary = "Task: " + subagentTask;
          entry.phase = shared::ToolPhase::Finished;
          entry.toolCallId = "";
          logEntries.push_back(std::move(entry));
        }
        
        if (!subagent.wait_state.empty()) {
          shared::SubagentToolLogEntry doneEntry;
          if (subagent.wait_state == "completed") {
            doneEntry.summary = "Done";
            doneEntry.phase = shared::ToolPhase::Finished;
          } else if (subagent.wait_state == "completed_no_summary") {
            doneEntry.summary = "Done (no summary)";
            doneEntry.phase = shared::ToolPhase::Finished;
          } else if (subagent.wait_state == "cancelled") {
            doneEntry.summary = "Cancelled";
            doneEntry.phase = shared::ToolPhase::Finished;
          } else if (subagent.wait_state == "failed") {
            doneEntry.summary =
                subagent.error_text.empty() ? "Failed"
                                            : "Failed: " + subagent.error_text;
            doneEntry.phase = shared::ToolPhase::Error;
          }
          if (!doneEntry.summary.empty()) {
            logEntries.push_back(std::move(doneEntry));
          }
        }
        
        auto choose_richer_log = [&](std::vector<shared::SubagentToolLogEntry> generated)
            -> std::vector<shared::SubagentToolLogEntry> {
          const auto is_informative = [](const shared::SubagentToolLogEntry &entry) {
            if (!entry.toolCallId.empty()) {
              return true;
            }
            return entry.summary.rfind("Task: ", 0) != 0 &&
                   entry.summary != "Done";
          };
          size_t generated_informative = 0;
          for (const auto &entry : generated) {
            if (is_informative(entry)) {
              ++generated_informative;
            }
          }
          size_t existing_informative = 0;
          for (const auto &entry : subagent.activity_log) {
            if (is_informative(entry)) {
              ++existing_informative;
            }
          }
          if (existing_informative > generated_informative ||
              (existing_informative == generated_informative &&
               subagent.activity_log.size() > generated.size())) {
            return subagent.activity_log;
          }
          return generated;
        };

        auto merged_log = choose_richer_log(std::move(logEntries));
        if (auto terminal = terminalSubagentLogEntry(subagent);
            terminal.has_value()) {
          const bool has_terminal =
              !merged_log.empty() &&
              merged_log.back().summary == terminal->summary;
          if (!has_terminal) {
            merged_log.push_back(*terminal);
          }
        }
        while (merged_log.size() > 8) {
          merged_log.erase(merged_log.begin());
        }
        view->subagent_tool_log = merged_log;
        view->subagent_running = false;
        subagent.activity_log = std::move(merged_log);
        subagent.running = false;
        subagent.waiting = false;
        subagent.provider_waiting = false;
        subagent.retrying = false;
        subagent.account_switched = false;
        if (subagent.wait_state.empty()) {
          subagent.wait_state = view->success ? "completed" : "failed";
          if (view->success) {
            subagent.outcome = SubagentOutcomeKind::Response;
          } else {
            subagent.outcome = SubagentOutcomeKind::Failed;
          }
        }
      }
    }
  }
}

const std::vector<QueuedMessageEntry> &StreamStateManager::getQueuedMessages() const {
  return queued_messages_;
}

const std::vector<QueuedMessageEntry> &StreamStateManager::getQueuedInternalMessages() const {
  return queued_internal_messages_;
}

int StreamStateManager::getToolCallClusterId(
    const std::string &toolCallId) const {
  auto it = tool_call_cluster_ids_.find(toolCallId);
  if (it == tool_call_cluster_ids_.end()) {
    return -1;
  }
  return it->second;
}

void StreamStateManager::assignToolCallClusterId(
    const std::string &agentId, const std::string &toolCallId) {
  if (tool_call_cluster_ids_.count(toolCallId) > 0) {
    return;
  }
  auto &cluster_state = live_quick_clusters_[agentId];
  if (cluster_state.prose_since_last_tool) {
    cluster_state.current_cluster++;
    cluster_state.prose_since_last_tool = false;
  }
  tool_call_cluster_ids_[toolCallId] = cluster_state.current_cluster;
}

void StreamStateManager::appendLiveTimelineDelta(const std::string &agentId,
                                                 TimelineEntry::Kind kind,
                                                 const std::string &delta) {
  if (firmius::shared::StringUtil::trim(delta).empty()) {
    return;
  }

  auto &stream = streams_[agentId];
  if (stream.has_active_live_entry &&
      stream.active_live_entry_kind == kind &&
      !stream.active_live_entry_id.empty()) {
    if (auto *entry = findTimelineEntry(stream.active_live_entry_id)) {
      entry->message += delta;
      return;
    }
    stream.has_active_live_entry = false;
    stream.active_live_entry_id.clear();
  }

  TimelineEntry entry;
  entry.kind = kind;
  entry.agentId = agentId;
  entry.message = delta;
  entry.id =
      "live:" + agentId + ":" + std::to_string(++next_live_entry_sequence_);
  timeline_.push_back(entry);
  stream.active_live_entry_id = entry.id;
  stream.active_live_entry_kind = kind;
  stream.has_active_live_entry = true;
}

void StreamStateManager::clearActiveLiveEntry(const std::string &agentId) {
  auto it = streams_.find(agentId);
  if (it == streams_.end()) {
    return;
  }
  it->second.has_active_live_entry = false;
  it->second.active_live_entry_id.clear();
}

TimelineEntry *StreamStateManager::findTimelineEntry(
    const std::string &entryId) {
  for (auto &entry : timeline_) {
    if (entry.id == entryId) {
      return &entry;
    }
  }
  return nullptr;
}

void StreamStateManager::applyToolResult(
    const std::shared_ptr<ToolCallView> &view, bool success,
    const std::string &result) {
  if (!view) {
    return;
  }

  if (view->name == "todo_write") {
    auto it_previous = last_todo_result_by_agent_.find(view->agentId);
    view->previous_result =
        it_previous != last_todo_result_by_agent_.end() ? it_previous->second : "";
  }
  view->success = success;
  view->result = result;
  const bool file_edit_like = isFileEditLikeToolName(view->name);
  if (success && file_edit_like) {
    for (const auto &signal : parseFileEditSignalsFromResult(result)) {
      mergeFileEditSignal(*view, signal);
    }
  }
  if (view->name == "subagent_wait" || view->name == "summon_subagent") {
    ParsedSubagentArgs parsed_args = parseSubagentArgs(view->args);
    ParsedSubagentResult parsedSubagent = parseSubagentResult(result);
    std::string parent_tool_id = view->toolCallId;
    if (view->name == "subagent_wait" && !parsed_args.agent_id.empty()) {
      auto it_parent = subagent_to_parent_tool_.find(parsed_args.agent_id);
      if (it_parent != subagent_to_parent_tool_.end()) {
        parent_tool_id = it_parent->second;
      }
    }
    auto &subagent = subagent_state_[parent_tool_id];
    subagent.parent_tool_call_id = parent_tool_id;
    subagent.owner_agent_id = view->agentId;
    subagent.waiting = (view->name == "subagent_wait");
    subagent.running = !subagent.waiting;
    subagent.provider_waiting = false;
    subagent.retrying = false;
    subagent.account_switched = false;
    if (!parsed_args.task.empty()) {
      subagent.task = parsed_args.task;
    }
    if (!parsed_args.title.empty()) {
      subagent.child_title = parsed_args.title;
    } else if (!parsed_args.name.empty() && subagent.child_title.empty()) {
      subagent.child_title = parsed_args.name;
    }
    if (!parsed_args.agent_id.empty()) {
      subagent.child_agent_id = parsed_args.agent_id;
      subagent_to_parent_tool_[parsed_args.agent_id] = parent_tool_id;
    }
    if (!parsedSubagent.agent_id.empty()) {
      subagent.child_agent_id = parsedSubagent.agent_id;
      subagent_to_parent_tool_[parsedSubagent.agent_id] = parent_tool_id;
    }
    subagent_tool_to_parent_[view->toolCallId] = parent_tool_id;

    const bool preserve_async_parent_state =
        view->name == "subagent_wait" && !success &&
        parsedSubagent.status.empty() &&
        isParentInterruptedWhileWaitingMessage(result) &&
        (subagent.wait_state == "spawned" || subagent.wait_state == "re-tasked" ||
         subagent.wait_state == "running" || subagent.wait_state == "retrying" ||
         subagent.wait_state == "account_switched" ||
         subagent.provider_waiting || subagent.retrying || subagent.running);

    if (preserve_async_parent_state) {
      view->subagent_wait_state = "cancelled";
      view->subagent_wait_message = result;
      view->subagent_fallback_used = false;
      view->subagent_route_category.clear();
      view->subagent_attempted_categories.clear();
      view->phase = ToolPhase::Error;
      return;
    }

    if (!parsedSubagent.status.empty()) {
      view->subagent_wait_state = parsedSubagent.status;
      view->subagent_wait_message = !parsedSubagent.error.empty()
                                        ? parsedSubagent.error
                                        : parsedSubagent.summary;
      view->subagent_fallback_used = parsedSubagent.fallback_used;
      view->subagent_route_category = parsedSubagent.route_category;
      view->subagent_attempted_categories =
          std::move(parsedSubagent.attempted_categories);
      subagent.wait_state = view->subagent_wait_state;
      subagent.final_summary = parsedSubagent.summary;
      subagent.error_text = parsedSubagent.error;
      subagent.fallback_used = parsedSubagent.fallback_used;
      subagent.route_category = parsedSubagent.route_category;
      subagent.attempted_categories = view->subagent_attempted_categories;
      subagent.artifacts_created = parsedSubagent.artifacts_created;
      subagent.artifacts_updated = parsedSubagent.artifacts_updated;
    } else {
      view->subagent_wait_state = success ? "completed" : "failed";
      view->subagent_wait_message = result;
      view->subagent_fallback_used = false;
      view->subagent_route_category.clear();
      view->subagent_attempted_categories.clear();
      subagent.wait_state = view->subagent_wait_state;
      subagent.final_summary = success ? result : "";
      subagent.error_text = success ? "" : result;
      subagent.fallback_used = false;
      subagent.route_category.clear();
      subagent.attempted_categories.clear();
      subagent.artifacts_created.clear();
      subagent.artifacts_updated.clear();
    }
    if (subagent.wait_state == "completed") {
      subagent.outcome = SubagentOutcomeKind::Response;
      subagent.running = false;
      subagent.waiting = false;
    } else if (subagent.wait_state == "completed_no_summary") {
      subagent.outcome = SubagentOutcomeKind::NoSummary;
      subagent.running = false;
      subagent.waiting = false;
    } else if (subagent.wait_state == "cancelled") {
      subagent.outcome = SubagentOutcomeKind::Cancelled;
      subagent.running = false;
      subagent.waiting = false;
    } else if (subagent.wait_state == "failed") {
      subagent.outcome = SubagentOutcomeKind::Failed;
      subagent.running = false;
      subagent.waiting = false;
    } else if (subagent.wait_state == "spawned" ||
               subagent.wait_state == "re-tasked") {
      subagent.running = true;
      subagent.waiting = false;
      subagent.outcome = SubagentOutcomeKind::Unknown;
    }
    if (!subagent.wait_state.empty()) {
      shared::SubagentToolLogEntry entry;
      entry.summary = "State: " + subagent.wait_state;
      entry.phase = success ? shared::ToolPhase::Finished : shared::ToolPhase::Error;
      subagent.activity_log.push_back(std::move(entry));
      while (subagent.activity_log.size() > 16) {
        subagent.activity_log.erase(subagent.activity_log.begin());
      }
    }
  }
  if (!success) {
    if (view->name == "process_wait" || view->name == "process_status" ||
        view->name == "process_input") {
      ParsedToolArgs parsed_args = parseToolArgs(view->args);
      if (!parsed_args.process_id.empty()) {
        auto &process = process_state_[parsed_args.process_id];
        process.process_id = parsed_args.process_id;
        if (!view->agentId.empty()) {
          process.owner_agent_id = view->agentId;
        }
        if (view->name == "process_wait") {
          process.waiting = false;
          process.wait_state = "failed";
          if (!parsed_args.pattern.empty()) {
            process.waiting_pattern = parsed_args.pattern;
          }
        }
      }
    }
    view->phase = ToolPhase::Error;
    return;
  }

  if (file_edit_like) {
    view->phase = ToolPhase::Finished;
    return;
  }

  ParsedToolArgs parsed_args = parseToolArgs(view->args);
  ParsedProcessResult parsed = parseProcessResult(result);
  std::string process_id = !parsed.process_id.empty() ? parsed.process_id : parsed_args.process_id;
  if (!process_id.empty()) {
    view->process_id = process_id;
    auto &process = process_state_[process_id];
    process.process_id = process_id;
    process.owner_agent_id = view->agentId;
    process.origin_tool_call_id =
        process.origin_tool_call_id.empty() ? view->toolCallId : process.origin_tool_call_id;
    if (!parsed.command.empty()) {
      process.command = parsed.command;
    } else if (!parsed_args.command.empty() && process.command.empty()) {
      process.command = parsed_args.command;
    }
    if (!parsed.cwd.empty()) {
      process.cwd = parsed.cwd;
    } else if (!parsed_args.cwd.empty() && process.cwd.empty()) {
      process.cwd = parsed_args.cwd;
    }
    if (view->name == "process_wait" && !parsed_args.pattern.empty()) {
      process.waiting_pattern = parsed_args.pattern;
      process.waiting = false;
      process.wait_state = "completed";
    }
    if (view->name == "process_wait" || view->name == "process_status") {
      rapidjson::Document doc;
      doc.Parse(result.c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        if (doc.HasMember("isRunning") && doc["isRunning"].IsBool()) {
          process.running = doc["isRunning"].GetBool();
          process.finished = !process.running;
        }
        if (doc.HasMember("exitCode") && doc["exitCode"].IsInt()) {
          process.exit_code = doc["exitCode"].GetInt();
          process.exit_code_known = true;
        }
        if (doc.HasMember("duration_ms") && doc["duration_ms"].IsNumber()) {
          process.duration_ms = doc["duration_ms"].GetDouble();
        }
        if (doc.HasMember("stdout") && doc["stdout"].IsString()) {
          appendOutputTail(process.latest_output_tail, doc["stdout"].GetString());
        }
        if (doc.HasMember("stderr") && doc["stderr"].IsString()) {
          const std::string stderr_str = doc["stderr"].GetString();
          if (!stderr_str.empty()) {
            appendOutputTail(process.latest_output_tail, "\n[stderr]\n" + stderr_str);
          }
        }
      }
    }
    flushBufferedProcessOutputForProcess(process_id);
  }
  if (parsed.exit_known) {
    view->process_exit_known = true;
    view->process_exit_code = parsed.exit_code;
  }
  view->process_duration_ms = parsed.duration_ms;

  if (view->name == "process_execute" && parsed.finish_reason == "Timeout" &&
      !process_id.empty()) {
    view->phase = ToolPhase::BackgroundRunning;
    view->process_is_background = true;
    auto &process = process_state_[process_id];
    process.running = true;
    process.finished = false;
    process.is_background = true;
    process.is_blocking = false;
    process.waiting = false;
    process.wait_state.clear();
    if (parsed.exit_known) {
      process.exit_code_known = true;
      process.exit_code = parsed.exit_code;
    }
    return;
  }

  if (view->name == "process_spawn" && !process_id.empty()) {
    view->process_is_background = true;
    auto &process = process_state_[process_id];
    process.is_background = true;
    process.is_blocking = false;
    process.running = !parsed.exit_known;
    process.finished = parsed.exit_known;
    if (parsed.exit_known) {
      process.exit_code_known = true;
      process.exit_code = parsed.exit_code;
    }
    if (parsed.exit_known) {
      view->phase = ToolPhase::Finished;
    } else {
      view->phase = ToolPhase::BackgroundRunning;
      return;
    }
  } else if (!process_id.empty()) {
    auto &process = process_state_[process_id];
    process.running = false;
    process.finished = true;
    process.is_background = false;
    process.is_blocking = false;
    if (parsed.exit_known) {
      process.exit_code_known = true;
      process.exit_code = parsed.exit_code;
    }
  }

  view->phase = ToolPhase::Finished;
  if (view->name == "todo_write") {
    last_todo_result_by_agent_[view->agentId] = result;
  }
}

bool StreamStateManager::applyProcessOutputToToolView(
    const shared::AgentProcessOutput &e) {
  auto it_pid = process_to_toolcall_.find(e.processId);
  if (it_pid == process_to_toolcall_.end()) {
    return false;
  }

  auto it_tool = tool_calls_.find(it_pid->second);
  if (it_tool == tool_calls_.end() || !it_tool->second) {
    return false;
  }

  auto &view = it_tool->second;
  auto &process = process_state_[e.processId];
  process.process_id = e.processId;
  process.origin_tool_call_id = it_pid->second;
  if (!view->agentId.empty()) {
    process.owner_agent_id = view->agentId;
  }
  appendOutputTail(process.latest_output_tail, e.output);
  if (!e.output.empty() &&
      (view->phase == ToolPhase::Called ||
       view->phase == ToolPhase::BackgroundRunning)) {
    view->live_process_output += e.output;
  }
  if (view->process_id.empty()) {
    view->process_id = e.processId;
  }
  if (e.finished) {
    view->process_exit_known = true;
    view->process_exit_code = e.exitCode;
    view->process_duration_ms = e.durationMs;
    process.running = false;
    process.finished = true;
    process.exit_code_known = true;
    process.exit_code = e.exitCode;
    process.duration_ms = e.durationMs;
    process.waiting = false;
    process.wait_state = "completed";
    if (view->phase == ToolPhase::BackgroundRunning ||
        view->name == "process_spawn") {
      view->phase = ToolPhase::Finished;
      view->success = (e.exitCode == 0);
    }
  } else {
    process.running = true;
    process.finished = false;
  }
  return true;
}

void StreamStateManager::flushBufferedProcessOutputForProcess(
    const std::string &processId) {
  auto it = pending_process_output_.find(processId);
  if (it == pending_process_output_.end()) {
    return;
  }

  std::vector<shared::AgentProcessOutput> still_pending;
  still_pending.reserve(it->second.size());
  for (const auto &event : it->second) {
    if (!applyProcessOutputToToolView(event)) {
      still_pending.push_back(event);
    }
  }

  if (still_pending.empty()) {
    pending_process_output_.erase(it);
  } else {
    it->second = std::move(still_pending);
  }
}

void StreamStateManager::flushBufferedProcessOutputForToolCall(
    const std::string &toolCallId) {
  if (toolCallId.empty()) {
    return;
  }
  for (const auto &[process_id, mapped_tool_call] : process_to_toolcall_) {
    if (mapped_tool_call == toolCallId) {
      flushBufferedProcessOutputForProcess(process_id);
    }
  }
}

} // namespace firmius::tui
