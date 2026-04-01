#include "tools/PythonToolPresentation.hpp"

#include "components/SyntaxHighlighter.hpp"
#include "utils/ErrorCleaner.hpp"
#include <rapidjson/document.h>

namespace firmius::tui {

namespace {

struct ParsedPythonArgs {
  std::string code;
};

ParsedPythonArgs ParseArgs(const std::string &args) {
  ParsedPythonArgs parsed;
  rapidjson::Document doc;
  doc.Parse(args.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return parsed;
  }
  if (doc.HasMember("code") && doc["code"].IsString()) {
    parsed.code = doc["code"].GetString();
  }
  return parsed;
}

struct ParsedPythonResult {
  std::string process_id;
  std::string finish_reason;
  bool has_exit_code = false;
  int exit_code = 0;
  bool has_duration = false;
  double duration_ms = 0.0;
  std::string stdout_data;
  std::string stderr_data;
};

ParsedPythonResult ParseResult(const std::string &result) {
  ParsedPythonResult parsed;
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
  return parsed;
}

ToolPresentationLifecycle DeriveLifecycle(const firmius::shared::ToolCallView &view) {
  if (view.phase == firmius::shared::ToolPhase::Preparing) {
    return ToolPresentationLifecycle::Preparing;
  }
  if (view.phase == firmius::shared::ToolPhase::Called ||
      view.phase == firmius::shared::ToolPhase::BackgroundRunning) {
    return ToolPresentationLifecycle::Running;
  }
  if (view.phase == firmius::shared::ToolPhase::Error ||
      (view.phase == firmius::shared::ToolPhase::Finished && !view.success)) {
    return ToolPresentationLifecycle::Error;
  }
  return ToolPresentationLifecycle::Success;
}

std::vector<std::string> BuildOutputLines(const std::string &output) {
  if (output.empty()) {
    return {};
  }
  std::vector<std::string> lines;
  lines.reserve(32);  // Reserve for common case
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

} // namespace

ToolPresentation BuildPythonToolPresentation(
    const firmius::shared::ToolCallView &view,
    const NormalizedProcessState *process_state) {
  ToolPresentation presentation;
  presentation.lifecycle = DeriveLifecycle(view);
  presentation.layout = ToolPresentationLayoutKind::BodyFirstStream;
  presentation.ansi_aware = true;
  presentation.density = ToolPresentationDensity::BodyFirstSummary;
  presentation.title.clear(); // We want python executions to be inline/no title

  const ParsedPythonArgs args = ParseArgs(view.args);
  const ParsedPythonResult result = ParseResult(view.result);

  if (!args.code.empty()) {
    if (SyntaxHighlighter::instance().hasGrammar("python")) {
      presentation.custom_body_elements =
          SyntaxHighlighter::instance().highlightRenderLines(args.code, "python");
    } else {
      auto lines = BuildOutputLines(args.code);
      for (const auto& line : lines) {
        presentation.custom_body_elements.push_back(ftxui::text(line));
      }
    }
  }

  const bool running = process_state ? process_state->running
                                     : (view.phase == firmius::shared::ToolPhase::Called ||
                                        view.phase == firmius::shared::ToolPhase::BackgroundRunning);
  const bool finished = process_state ? process_state->finished
                                      : (view.phase == firmius::shared::ToolPhase::Finished);
  const bool exit_code_known =
      process_state ? process_state->exit_code_known : view.process_exit_known;
  const int exit_code =
      process_state ? process_state->exit_code : view.process_exit_code;

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

  auto output_lines = BuildOutputLines(output);
  if (!output_lines.empty()) {
    presentation.body_lines.insert(presentation.body_lines.end(), output_lines.begin(),
                                   output_lines.end());
  }

  std::vector<std::string> status_parts;
  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    status_parts.push_back("fail");
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

  if (status_parts.empty() &&
      presentation.lifecycle == ToolPresentationLifecycle::Success) {
    status_parts.push_back("done");
  }
  
  std::string status_footer;
  for (size_t i = 0; i < status_parts.size(); ++i) {
    if (i > 0) status_footer += " • ";
    status_footer += status_parts[i];
  }
  if (!status_footer.empty()) {
    presentation.status_footer = status_footer;
  }

  const size_t visible_body_lines = 10;
  const size_t stream_line_count = presentation.body_lines.size();
  
  // We want to be expandable if there's too much output, OR too much code.
  if (stream_line_count > visible_body_lines || presentation.custom_body_elements.size() > visible_body_lines) {
    presentation.expandable = true;
    presentation.expanded = view.show_result;
  }

  return presentation;
}

} // namespace firmius::tui
