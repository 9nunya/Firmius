#include "tools/ProcessPresenter.hpp"
#include "tools/ToolArgsParser.hpp"
#include "items/ToolCallItem.hpp"
#include "AnsiOutputParser.hpp"
#include "Terminal.hpp"

#include <rapidjson/document.h>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace firmius::tui2 {

bool ProcessPresenter::matches(const std::string& toolName) const {
  return toolName == "Process" || toolName == "Python";
}

namespace {

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

std::string formatDuration(std::chrono::milliseconds ms) {
  double secs = static_cast<double>(ms.count()) / 1000.0;
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(1) << secs << "s";
  return oss.str();
}

struct ParsedArgs {
  std::string action;
  std::string command;
  std::string processId;
  std::string code;
  std::string cwd;
  std::string pattern;
  std::string input;
  int timeoutMs = 0;
  int tailLines = 0;
  int maxBytes = 0;
};

ParsedArgs parseArgs(const std::string& json) {
  ParsedArgs p;
  if (json.empty()) return p;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return p;

  auto getString = [&](const char* key) -> std::string {
    if (doc.HasMember(key) && doc[key].IsString()) return doc[key].GetString();
    return "";
  };
  auto getInt = [&](const char* key, int def = 0) -> int {
    if (doc.HasMember(key) && doc[key].IsInt()) return doc[key].GetInt();
    return def;
  };

  p.action = getString("action");
  p.command = getString("command");
  p.processId = getString("process_id");
  p.code = getString("code");
  p.cwd = getString("cwd");
  p.pattern = getString("pattern");
  p.input = getString("input");
  p.timeoutMs = getInt("timeout_ms");
  p.tailLines = getInt("tail_lines");
  p.maxBytes = getInt("max_bytes");
  return p;
}

struct ParsedResult {
  int exitCode = -1;
  double durationMs = 0;
  std::string stdoutData;
  std::string stderrData;
  std::string processId;
  std::string finishReason;
  std::string output;
  bool isRunning = false;
  bool patternFound = false;
  int linesRead = 0;
  int entryCount = 0;
};

ParsedResult parseResult(const std::string& json) {
  ParsedResult r;
  if (json.empty()) return r;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return r;

  auto getString = [&](const char* key) -> std::string {
    if (doc.HasMember(key) && doc[key].IsString()) return doc[key].GetString();
    return "";
  };
  auto getInt = [&](const char* key, int def = 0) -> int {
    if (doc.HasMember(key) && doc[key].IsInt()) return doc[key].GetInt();
    return def;
  };
  auto getDouble = [&](const char* key, double def = 0) -> double {
    if (doc.HasMember(key) && doc[key].IsNumber()) return doc[key].GetDouble();
    return def;
  };
  auto getBool = [&](const char* key, bool def = false) -> bool {
    if (doc.HasMember(key) && doc[key].IsBool()) return doc[key].GetBool();
    return def;
  };

  r.exitCode = getInt("exit_code", -1);
  r.durationMs = getDouble("duration_ms");
  r.stdoutData = getString("stdout");
  r.stderrData = getString("stderr");
  r.processId = getString("process_id");
  r.finishReason = getString("finish_reason");
  r.output = getString("output");
  r.isRunning = getBool("is_running");
  r.patternFound = getBool("pattern_found");
  r.linesRead = getInt("lines_read");
  r.entryCount = getInt("count");
  return r;
}

std::string elapsedStr(const ToolCallItem& item) {
  return formatDuration(item.elapsed());
}

std::vector<std::string> renderWaitPreparing(const ParsedArgs& args) {
  std::string text = "  \xe2\x9f\xb3 Waiting on " + args.processId;
  if (!args.pattern.empty()) text += " \xe2\x80\x94 pattern: \"" + args.pattern + "\"";
  if (args.timeoutMs > 0) text += " \xe2\x80\x94 timeout: " + std::to_string(args.timeoutMs / 1000) + "s";
  if (args.tailLines > 0) text += " \xe2\x80\x94 tail: " + std::to_string(args.tailLines) + " lines";
  text += "...";
  return {ansi::fgRgb(220, 180, 80, text)};
}

std::vector<std::string> renderPreparing(const std::string& toolName,
                                          const std::string& args) {
  if (toolName == "Python") {
    return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Python")};
  }
  // If args are already available (e.g. Wait action), show details immediately
  if (!args.empty()) {
    auto parsed = parseArgs(args);
    if (parsed.action == "Wait" && !parsed.processId.empty()) {
      return renderWaitPreparing(parsed);
    }
    if (parsed.action == "Execute" && !parsed.command.empty()) {
      return {ansi::fgRgb(220, 180, 80, "  \xe2\x98\x98 Bash ") +
              ansi::bold(ansi::fgRgb(220, 220, 230, parsed.command))};
    }
    if (parsed.action == "Spawn" && !parsed.command.empty()) {
      return {ansi::fgRgb(220, 180, 80, "  \xe2\x98\x98 Bash ") +
              ansi::bold(ansi::fgRgb(220, 220, 230, parsed.command))};
    }
  }
  return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Process")};
}

std::vector<std::string> renderExecuteCalled(const ParsedArgs& args, const ToolCallItem& item,
                                              int width) {
  std::vector<std::string> result;
  // Header
  std::string cmd = args.command.empty() ? "..." : args.command;
  result.push_back(ansi::fgRgb(220, 180, 80, "  \xe2\x98\x98 Bash ") +
                   ansi::bold(ansi::fgRgb(220, 220, 230, cmd)));

  // Show live output if available
  const auto& stdout = item.processStdout();
  const auto& stderr = item.processStderr();

  if (stdout.empty() && stderr.empty()) {
    result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140, "  \xe2\x94\x82 (waiting for output...)")));
  } else {
    // Show last N lines of output as live preview
    std::string combined = stdout;
    if (!stderr.empty()) {
      if (!combined.empty() && combined.back() != '\n') combined += '\n';
      combined += stderr;
    }
    auto lines = splitLines(combined);
    // Show last 8 lines of live output
    const int maxPreviewLines = 8;
    int start = std::max(0, static_cast<int>(lines.size()) - maxPreviewLines);
    for (int i = start; i < static_cast<int>(lines.size()); ++i) {
      std::string line = lines[i];
      if (static_cast<int>(line.size()) > width - 4) {
        line = line.substr(0, width - 7) + "...";
      }
      result.push_back(ansi::dim(ansi::fgRgb(140, 140, 160, "  \xe2\x94\x82 ")) +
                       ansi::dim(ansi::fgRgb(180, 180, 200, line)));
    }
  }

  // Footer with live timer
  result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140,
      "  running \xe2\x80\xa2 " + elapsedStr(item))));
  return result;
}

std::vector<std::string> renderExecuteFinished(const ParsedArgs& args, const ParsedResult& res,
                                                const ToolCallItem& item, int width) {
  std::vector<std::string> result;
  bool success = item.success();
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";
  auto iconColor = success ? ansi::fgRgb(100, 200, 120, "  " + icon + " ")
                           : ansi::fgRgb(220, 80, 80, "  " + icon + " ");

  std::string cmd = args.command.empty() ? "Bash" : args.command;
  result.push_back(iconColor + ansi::bold(ansi::fgRgb(220, 220, 230, cmd)));

  // Output body
  std::string output = res.stdoutData;
  if (!res.stderrData.empty()) {
    if (!output.empty()) output += "\n";
    output += res.stderrData;
  }
  if (output.empty() && !res.output.empty()) {
    output = res.output;
  }
  if (!output.empty()) {
    auto lines = AnsiOutputParser::toLines(output, width, item.isExpanded() ? -1 : 4);
    result.insert(result.end(), lines.begin(), lines.end());
    if (!item.isExpanded() && lines.size() > 4) {
      result.push_back(ansi::dim(ansi::fgRgb(150, 150, 170,
          "  expand (Ctrl+E) to see more")));
    }
  }

  // Footer
  std::vector<std::string> footerParts;
  footerParts.push_back("exit " + std::to_string(res.exitCode));
  if (res.durationMs > 0) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << (res.durationMs / 1000.0) << "s";
    footerParts.push_back(oss.str());
  }
  if (res.finishReason == "Timeout") {
    footerParts.push_back("background");
  }
  std::string footer;
  for (size_t i = 0; i < footerParts.size(); ++i) {
    if (i > 0) footer += " \xe2\x80\xa2 ";
    footer += footerParts[i];
  }
  auto footerColor = success ? ansi::fgRgb(100, 200, 120, footer)
                             : ansi::fgRgb(220, 80, 80, footer);
  result.push_back(ansi::dim("  " + footerColor));

  // Timeout notice
  if (res.finishReason == "Timeout") {
    result.push_back(ansi::fgRgb(220, 160, 60, "  ! Still running in background"));
  }
  return result;
}

std::vector<std::string> renderSpawnCalled(const ParsedArgs& args, const ToolCallItem& item,
                                            int width) {
  std::vector<std::string> result;
  std::string cmd = args.command.empty() ? "..." : args.command;
  result.push_back(ansi::fgRgb(220, 180, 80, "  \xe2\x98\x98 Bash ") +
                   ansi::bold(ansi::fgRgb(220, 220, 230, cmd)));

  // Show live output if available (same as Execute)
  const auto& stdout = item.processStdout();
  const auto& stderr = item.processStderr();

  if (stdout.empty() && stderr.empty()) {
    result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140, "  \xe2\x94\x82 (waiting for output...)")));
  } else {
    std::string combined = stdout;
    if (!stderr.empty()) {
      if (!combined.empty() && combined.back() != '\n') combined += '\n';
      combined += stderr;
    }
    auto outLines = splitLines(combined);
    const int maxPreviewLines = 8;
    int start = std::max(0, static_cast<int>(outLines.size()) - maxPreviewLines);
    for (int i = start; i < static_cast<int>(outLines.size()); ++i) {
      std::string line = outLines[i];
      if (static_cast<int>(line.size()) > width - 4) {
        line = line.substr(0, width - 7) + "...";
      }
      result.push_back(ansi::dim(ansi::fgRgb(140, 140, 160, "  \xe2\x94\x82 ")) +
                       ansi::dim(ansi::fgRgb(180, 180, 200, line)));
    }
  }

  result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140,
      "  background \xe2\x80\xa2 " + elapsedStr(item))));
  return result;
}

std::vector<std::string> renderSpawnFinished(const ParsedArgs& args, const ParsedResult& res,
                                              const ToolCallItem& item, int width) {
  std::vector<std::string> result;
  bool success = item.success();
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";
  auto iconColor = success ? ansi::fgRgb(100, 200, 120, "  " + icon + " ")
                           : ansi::fgRgb(220, 80, 80, "  " + icon + " ");
  std::string cmd = args.command.empty() ? "Bash" : args.command;
  result.push_back(iconColor + ansi::bold(ansi::fgRgb(220, 220, 230, cmd)));

  // Output body
  std::string output = res.stdoutData;
  if (!res.stderrData.empty()) {
    if (!output.empty()) output += "\n";
    output += res.stderrData;
  }
  if (output.empty() && !res.output.empty()) {
    output = res.output;
  }
  if (!output.empty()) {
    auto lines = AnsiOutputParser::toLines(output, width, item.isExpanded() ? -1 : 4);
    result.insert(result.end(), lines.begin(), lines.end());
    if (!item.isExpanded() && lines.size() > 4) {
      result.push_back(ansi::dim(ansi::fgRgb(150, 150, 170,
          "  expand (Ctrl+E) to see more")));
    }
  }

  std::string footer = "background";
  if (!res.processId.empty()) footer += " \xe2\x80\xa2 pid " + res.processId;
  if (res.durationMs > 0) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << (res.durationMs / 1000.0) << "s";
    footer += " \xe2\x80\xa2 " + oss.str();
  }
  auto footerColor = success ? ansi::fgRgb(100, 200, 120, footer)
                             : ansi::fgRgb(220, 80, 80, footer);
  result.push_back(ansi::dim("  " + footerColor));
  return result;
}

std::vector<std::string> renderStatusFinished(const ParsedResult& res) {
  std::string statusText;
  if (res.isRunning) {
    statusText = "running";
  } else {
    statusText = "exited (" + std::to_string(res.exitCode) + ")";
    if (res.durationMs > 0) {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(1) << (res.durationMs / 1000.0) << "s";
      statusText += " \xe2\x80\xa2 " + oss.str();
    }
  }
  return {ansi::fgRgb(100, 200, 120, "  \xe2\x9c\x93 Process \xe2\x80\x94 " + statusText)};
}

std::vector<std::string> renderWaitCalled(const ParsedArgs& args, const ToolCallItem& item) {
  std::string text = "  \xe2\x9f\xb3 Waiting on " + args.processId;
  if (!args.pattern.empty()) text += " \xe2\x80\x94 pattern: \"" + args.pattern + "\"";
  if (args.timeoutMs > 0) text += " \xe2\x80\x94 timeout: " + std::to_string(args.timeoutMs / 1000) + "s";
  if (args.tailLines > 0) text += " \xe2\x80\x94 tail: " + std::to_string(args.tailLines) + " lines";
  text += "...";
  return {ansi::fgRgb(220, 180, 80, text),
          ansi::dim(ansi::fgRgb(120, 120, 140, "  " + elapsedStr(item)))};
}

std::vector<std::string> renderWaitFinished(const ParsedArgs& args, const ParsedResult& res) {
  std::string text = "  \xe2\x9c\x93 Process " + args.processId;
  if (res.patternFound) {
    text += " \xe2\x80\x94 pattern matched";
  } else {
    text += " \xe2\x80\x94 exited (" + std::to_string(res.exitCode) + ")";
    if (res.durationMs > 0) {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(1) << (res.durationMs / 1000.0) << "s";
      text += " \xe2\x80\xa2 " + oss.str();
    }
  }
  return {ansi::fgRgb(100, 200, 120, text)};
}

std::vector<std::string> renderInputCalled(const ParsedArgs& args) {
  return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Process.input \xe2\x86\x92 " + args.processId)};
}

std::vector<std::string> renderInputFinished(const ParsedArgs& args, const ParsedResult& /*res*/) {
  int chars = static_cast<int>(args.input.size());
  return {ansi::fgRgb(100, 200, 120,
      "  \xe2\x9c\x93 Sent " + std::to_string(chars) + " chars to process")};
}

std::vector<std::string> renderOutputCalled(const ParsedArgs& args) {
  return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Process.output " + args.processId)};
}

std::vector<std::string> renderOutputFinished(const ParsedResult& res) {
  int bytes = static_cast<int>(res.output.size());
  return {ansi::fgRgb(100, 200, 120,
      "  \xe2\x9c\x93 Output read \xe2\x80\x94 " + std::to_string(bytes) + " bytes")};
}

std::vector<std::string> renderKillCalled(const ParsedArgs& args) {
  return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Process.kill " + args.processId)};
}

std::vector<std::string> renderKillFinished(const ParsedArgs& args, const ParsedResult& res) {
  if (res.isRunning) {
    return {ansi::fgRgb(100, 200, 120, "  \xe2\x9c\x93 Killed process " + args.processId)};
  }
  return {ansi::fgRgb(100, 200, 120, "  \xe2\x9c\x93 Process " + args.processId + " already dead")};
}

std::vector<std::string> renderListFinished(const ParsedResult& res) {
  return {ansi::fgRgb(100, 200, 120,
      "  \xe2\x9c\x93 " + std::to_string(res.entryCount) + " processes")};
}

std::vector<std::string> renderPythonCalled(const ParsedArgs& args, const ToolCallItem& item,
                                             int /*width*/) {
  std::vector<std::string> result;
  result.push_back(ansi::fgRgb(220, 180, 80, "  \xe2\x98\x98 Python"));

  // Show code block
  if (!args.code.empty()) {
    result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140, "  \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 code \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80")));
    std::istringstream stream(args.code);
    std::string line;
    while (std::getline(stream, line)) {
      result.push_back(ansi::bgRgb(25, 25, 32, ansi::fgRgb(210, 210, 220, "  " + line)));
    }
    result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140, "  \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 output \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80")));
  }

  result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140, "  \xe2\x94\x82 (waiting for output...)")));
  result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140,
      "  running \xe2\x80\xa2 " + elapsedStr(item))));
  return result;
}

std::vector<std::string> renderPythonFinished(const ParsedArgs& args, const ParsedResult& res,
                                               const ToolCallItem& item, int width) {
  std::vector<std::string> result;
  bool success = item.success();
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";
  auto iconColor = success ? ansi::fgRgb(100, 200, 120, "  " + icon + " ")
                           : ansi::fgRgb(220, 80, 80, "  " + icon + " ");
  result.push_back(iconColor + ansi::bold(ansi::fgRgb(220, 220, 230, "Python")));

  // Code block
  if (!args.code.empty()) {
    result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140, "  \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 code \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80")));
    std::istringstream stream(args.code);
    std::string line;
    while (std::getline(stream, line)) {
      result.push_back(ansi::bgRgb(25, 25, 32, ansi::fgRgb(210, 210, 220, "  " + line)));
    }
  }

  // Output
  std::string output = res.stdoutData;
  if (!res.stderrData.empty()) {
    if (!output.empty()) output += "\n";
    output += res.stderrData;
  }
  if (!output.empty()) {
    result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140, "  \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 output \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80")));
    auto lines = AnsiOutputParser::toLines(output, width, item.isExpanded() ? -1 : 10);
    result.insert(result.end(), lines.begin(), lines.end());
  }

  // Footer
  std::ostringstream oss;
  oss << "exit " << res.exitCode;
  if (res.durationMs > 0) {
    oss << " \xe2\x80\xa2 " << std::fixed << std::setprecision(1) << (res.durationMs / 1000.0) << "s";
  }
  auto footerColor = success ? ansi::fgRgb(100, 200, 120, oss.str())
                             : ansi::fgRgb(220, 80, 80, oss.str());
  result.push_back(ansi::dim("  " + footerColor));
  return result;
}

} // namespace

std::vector<std::string> ProcessPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int width) const {
  // Preparing phase
  if (item.phase() == ToolPhase::Preparing) {
    return renderPreparing(item.toolName(), item.args());
  }

  auto args = parseArgs(item.args());

  // Python tool — separate rendering
  if (item.toolName() == "Python") {
    if (item.phase() == ToolPhase::Called) {
      return renderPythonCalled(args, item, width);
    }
    auto res = parseResult(item.result());
    return renderPythonFinished(args, res, item, width);
  }

  // Process tool — dispatch by action
  if (args.action == "Execute") {
    if (item.phase() == ToolPhase::Called) return renderExecuteCalled(args, item, width);
    auto res = parseResult(item.result());
    return renderExecuteFinished(args, res, item, width);
  }

  if (args.action == "Spawn") {
    if (item.phase() == ToolPhase::Called) return renderSpawnCalled(args, item, width);
    auto res = parseResult(item.result());
    return renderSpawnFinished(args, res, item, width);
  }

  if (args.action == "Status") {
    if (item.phase() == ToolPhase::Called) return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Process.status " + args.processId)};
    auto res = parseResult(item.result());
    return renderStatusFinished(res);
  }

  if (args.action == "Wait") {
    if (item.phase() == ToolPhase::Called) return renderWaitCalled(args, item);
    auto res = parseResult(item.result());
    return renderWaitFinished(args, res);
  }

  if (args.action == "Input") {
    if (item.phase() == ToolPhase::Called) return renderInputCalled(args);
    return renderInputFinished(args, parseResult(item.result()));
  }

  if (args.action == "Output") {
    if (item.phase() == ToolPhase::Called) return renderOutputCalled(args);
    return renderOutputFinished(parseResult(item.result()));
  }

  if (args.action == "Kill") {
    if (item.phase() == ToolPhase::Called) return renderKillCalled(args);
    return renderKillFinished(args, parseResult(item.result()));
  }

  if (args.action == "List") {
    if (item.phase() == ToolPhase::Called) return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Process.list")};
    return renderListFinished(parseResult(item.result()));
  }

  // Unknown action fallback
  if (item.phase() == ToolPhase::Called) {
    return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 " + item.toolName())};
  }
  bool success = item.success();
  auto color = success ? ansi::fgRgb(100, 200, 120, "  \xe2\x9c\x93 " + item.toolName())
                       : ansi::fgRgb(220, 80, 80, "  \xe2\x9c\x97 " + item.toolName());
  return {color};
}

} // namespace firmius::tui2
