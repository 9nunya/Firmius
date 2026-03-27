#include "tools/ProcessToolPresentation.hpp"

#include "utils/ErrorCleaner.hpp"
#include "utils/ToolSummaries.hpp"
#include <rapidjson/document.h>
#include <iomanip>
#include <sstream>

namespace firmius::tui {

using firmius::shared::SummarizeToolCall;
using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

namespace {

struct ParsedProcessArgs {
  std::string process_id;
  std::string command;
  std::string cwd;
  std::string pattern;
  std::string input;
  std::string code;
};

struct ParsedProcessResult {
  std::string process_id;
  std::string finish_reason;
  bool has_is_running = false;
  bool is_running = false;
  bool has_pattern_found = false;
  bool pattern_found = false;
  bool has_exit_code = false;
  int exit_code = 0;
  bool has_duration = false;
  double duration_ms = 0.0;
  std::string stdout_data;
  std::string stderr_data;
};

bool IsMatch(const std::string &actual, const std::string &expected) {
  if (actual.empty() || expected.empty()) {
    return false;
  }
  return actual.find(expected) != std::string::npos;
}

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

ParsedProcessArgs ParseArgs(const std::string &args) {
  ParsedProcessArgs parsed;
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
    parsed.code = doc["code"].GetString();
  }
  return parsed;
}

ParsedProcessResult ParseResult(const std::string &result) {
  ParsedProcessResult parsed;
  rapidjson::Document doc;
  doc.Parse(result.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return parsed;
  }

  if (doc.HasMember("process_id") && doc["process_id"].IsString()) {
    parsed.process_id = doc["process_id"].GetString();
  }
  if (doc.HasMember("finish_reason") && doc["finish_reason"].IsString()) {
    parsed.finish_reason = doc["finish_reason"].GetString();
  }
  if (doc.HasMember("stdout") && doc["stdout"].IsString()) {
    parsed.stdout_data = doc["stdout"].GetString();
  }
  if (doc.HasMember("stderr") && doc["stderr"].IsString()) {
    parsed.stderr_data = doc["stderr"].GetString();
  }
  if (doc.HasMember("exit_code") && doc["exit_code"].IsInt()) {
    parsed.has_exit_code = true;
    parsed.exit_code = doc["exit_code"].GetInt();
  }
  if (doc.HasMember("exitCode") && doc["exitCode"].IsInt()) {
    parsed.has_exit_code = true;
    parsed.exit_code = doc["exitCode"].GetInt();
  }
  if (doc.HasMember("duration_ms") && doc["duration_ms"].IsNumber()) {
    parsed.has_duration = true;
    parsed.duration_ms = doc["duration_ms"].GetDouble();
  }
  if (doc.HasMember("isRunning") && doc["isRunning"].IsBool()) {
    parsed.has_is_running = true;
    parsed.is_running = doc["isRunning"].GetBool();
  }
  if (doc.HasMember("patternFound") && doc["patternFound"].IsBool()) {
    parsed.has_pattern_found = true;
    parsed.pattern_found = doc["patternFound"].GetBool();
  }
  return parsed;
}

std::string PreviewPythonCode(const std::string &code) {
  std::istringstream stream(code);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) {
      return "python " + line;
    }
  }
  return "python";
}

std::vector<std::string> BuildOutputLines(const std::string &output) {
  if (output.empty()) {
    return {};
  }
  std::vector<std::string> lines;
  std::istringstream stream(output);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  if (lines.empty() && !output.empty()) {
    lines.push_back(output);
  }
  return lines;
}

std::string FormatDuration(double duration_ms) {
  if (duration_ms <= 0.0) {
    return "";
  }
  if (duration_ms >= 1000.0) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << (duration_ms / 1000.0) << "s";
    return os.str();
  }
  return std::to_string(static_cast<int>(duration_ms)) + "ms";
}

std::string JoinParts(const std::vector<std::string> &parts) {
  std::string joined;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (parts[i].empty()) {
      continue;
    }
    if (!joined.empty()) {
      joined += " • ";
    }
    joined += parts[i];
  }
  return joined;
}

} // namespace

bool IsProcessFamilyTool(const std::string &tool_name) {
  return IsMatch(tool_name, "process_execute") ||
         IsMatch(tool_name, "process_spawn") || IsMatch(tool_name, "process_wait") ||
         IsMatch(tool_name, "process_input") || IsMatch(tool_name, "process_status") ||
         IsMatch(tool_name, "python_execute");
}

ToolPresentation BuildProcessToolPresentation(
    const ToolCallView &view, const NormalizedProcessState *process_state) {
  ToolPresentation presentation;
  presentation.lifecycle = DeriveLifecycle(view);
  presentation.layout = ToolPresentationLayoutKind::BodyFirstStream;
  presentation.ansi_aware = true;
  presentation.density = ToolPresentationDensity::BodyFirstSummary;

  const ParsedProcessArgs args = ParseArgs(view.args);
  const ParsedProcessResult result = ParseResult(view.result);
  const std::string process_id =
      !args.process_id.empty()
          ? args.process_id
          : (!view.process_id.empty()
                 ? view.process_id
                 : (!result.process_id.empty()
                        ? result.process_id
                        : ((process_state && !process_state->process_id.empty())
                               ? process_state->process_id
                               : std::string{})));
  const std::string command =
      process_state && !process_state->command.empty()
          ? process_state->command
          : (!args.command.empty()
                 ? args.command
                 : (!args.code.empty() ? PreviewPythonCode(args.code) : ""));
  const std::string cwd = process_state && !process_state->cwd.empty()
                              ? process_state->cwd
                              : args.cwd;

  const bool running = process_state ? process_state->running
                                     : (view.phase == ToolPhase::Called ||
                                        view.phase == ToolPhase::BackgroundRunning);
  const bool finished = process_state ? process_state->finished
                                      : (view.phase == ToolPhase::Finished);
  const bool is_background = process_state
                                 ? process_state->is_background
                                 : (view.phase == ToolPhase::BackgroundRunning ||
                                    view.process_is_background);
  const bool exit_code_known =
      process_state ? process_state->exit_code_known : view.process_exit_known;
  const int exit_code =
      process_state ? process_state->exit_code : view.process_exit_code;
  const double duration_ms =
      process_state && process_state->duration_ms > 0.0 ? process_state->duration_ms
                                                        : view.process_duration_ms;
  std::string output = process_state ? process_state->latest_output_tail : "";
  if (output.empty()) {
    output = view.live_process_output;
  }
  if (output.empty()) {
    output = result.stdout_data;
    if (!result.stderr_data.empty()) {
      if (!output.empty()) {
        output += "\n";
      }
      output += "[stderr]\n" + result.stderr_data;
    }
  }
  if (output.empty() && presentation.lifecycle == ToolPresentationLifecycle::Error) {
    output = firmius::shared::ErrorCleaner::clean(view.result);
  }

  if (IsMatch(view.name, "python_execute")) {
    presentation.title.clear();
  } else if (IsMatch(view.name, "process_execute")) {
    presentation.title.clear();
  } else if (IsMatch(view.name, "process_spawn")) {
    presentation.title.clear();
  } else if (IsMatch(view.name, "process_wait")) {
    presentation.title = process_id.empty() ? "wait" : ("wait " + process_id);
  } else if (IsMatch(view.name, "process_input")) {
    presentation.title = process_id.empty() ? "input" : ("input " + process_id);
  } else if (IsMatch(view.name, "process_status")) {
    presentation.title = process_id.empty() ? "status" : ("status " + process_id);
  } else {
    presentation.title = SummarizeToolCall(view.name, view.args, view.phase);
  }
  presentation.subtitle.clear();
  presentation.compact_summary.clear();
  if (!command.empty()) {
    presentation.body_lines.push_back("$ " + command);
  }

  if (!process_id.empty()) {
    presentation.facts.push_back({"Process", process_id});
  }

  std::vector<std::string> status_parts;
  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    status_parts.push_back("fail");
  } else if (running && is_background) {
    status_parts.push_back("background");
    if (!process_id.empty()) {
      status_parts.push_back("pid " + process_id);
    }
  } else if (running) {
    status_parts.push_back("running");
  } else if (finished) {
    status_parts.push_back("finished");
  }

  if (exit_code_known) {
    status_parts.push_back("exit " + std::to_string(exit_code));
  } else if (result.has_exit_code) {
    status_parts.push_back("exit " + std::to_string(result.exit_code));
  }
  const std::string duration =
      duration_ms > 0.0 ? FormatDuration(duration_ms)
                        : (result.has_duration ? FormatDuration(result.duration_ms) : "");
  if (!duration.empty()) {
    status_parts.push_back(duration);
  }

  if (IsMatch(view.name, "process_wait")) {
    presentation.layout = ToolPresentationLayoutKind::BodyFirstStream;
    if (!args.pattern.empty()) {
      presentation.facts.push_back({"Pattern", args.pattern});
      status_parts.push_back("pattern " + args.pattern);
    }
    if (process_state && process_state->waiting) {
      status_parts.push_back("waiting");
    } else if (result.has_pattern_found && result.pattern_found) {
      status_parts.push_back("matched");
    } else if (result.has_is_running && !result.is_running) {
      status_parts.push_back("completed");
    }
  }

  if (IsMatch(view.name, "process_input")) {
    presentation.layout = ToolPresentationLayoutKind::BodyFirstStream;
    if (!args.input.empty()) {
      presentation.body_lines.push_back(args.input);
      presentation.facts.push_back({"Input", args.input});
    }
  }

  if (IsMatch(view.name, "process_execute") &&
      view.phase == ToolPhase::BackgroundRunning &&
      result.finish_reason == "Timeout") {
    ToolPresentationNotice notice;
    notice.kind = ToolPresentationNoticeKind::Info;
    notice.text = "Timed out and continues in background";
    presentation.notices.push_back(std::move(notice));
  }

  auto output_lines = BuildOutputLines(output);
  if (!output_lines.empty()) {
    presentation.body_lines.insert(presentation.body_lines.end(), output_lines.begin(),
                                   output_lines.end());
  }
  if (!cwd.empty()) {
    presentation.facts.push_back({"cwd", cwd});
  }
  const size_t visible_body_lines = 6;
  const size_t prefix_lines = !command.empty() ? 1u : 0u;
  const size_t visible_stream_lines =
      visible_body_lines > prefix_lines ? visible_body_lines - prefix_lines : 1u;
  const size_t stream_line_count =
      presentation.body_lines.size() > prefix_lines
          ? presentation.body_lines.size() - prefix_lines
          : 0u;
  if (stream_line_count > visible_stream_lines) {
    presentation.expandable = true;
    presentation.expanded = view.show_result;
  }
  if (status_parts.empty() &&
      presentation.lifecycle == ToolPresentationLifecycle::Success) {
    status_parts.push_back("done");
  }
  if (!status_parts.empty()) {
    presentation.status_footer = JoinParts(status_parts);
  }
  return presentation;
}

} // namespace firmius::tui
