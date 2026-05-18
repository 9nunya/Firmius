#include "tools/ProcessPresenter.hpp"
#include "tools/ToolArgsParser.hpp"
#include "items/ToolCallItem.hpp"
#include "AnsiOutputParser.hpp"
#include "SyntaxHighlighter.hpp"
#include "Terminal.hpp"
#include "ThemeManager.hpp"
#include "ThemeAnsi.hpp"

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

// Word-wrap an ANSI-colored string into a vector of strings each bounded by
// `maxWidth` visible columns. Preserves color/attribute escape sequences
// across wraps (any open SGR state on the input stays applied to the
// continuation line).
//
// `indent` is plain text added to the start of every continuation line.
// Returns at least one line (possibly empty) so callers don't need to guard.
std::vector<std::string> wrapAnsi(const std::string& text, int maxWidth,
                                  const std::string& indent = "") {
  std::vector<std::string> result;
  if (maxWidth <= 0) {
    result.push_back(text);
    return result;
  }

  std::string current;
  int currentVis = 0;
  bool inEscape = false;
  std::string escAccum;

  auto flush = [&](bool addIndent) {
    result.push_back(current);
    current.clear();
    currentVis = 0;
    if (addIndent && !indent.empty()) {
      current = indent;
      currentVis = ansi::visibleWidth(indent);
    }
  };

  for (size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch == '\x1b') {
      inEscape = true;
      escAccum.clear();
      escAccum += ch;
      continue;
    }
    if (inEscape) {
      escAccum += ch;
      if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
        inEscape = false;
        current += escAccum;
        escAccum.clear();
      }
      continue;
    }
    // UTF-8 continuation byte: append without bumping width.
    if ((static_cast<unsigned char>(ch) & 0xC0) == 0x80) {
      current += ch;
      continue;
    }
    if (ch == '\n') {
      flush(true);
      continue;
    }
    if (currentVis >= maxWidth) {
      flush(true);
    }
    current += ch;
    ++currentVis;
  }
  if (!current.empty() || result.empty()) {
    result.push_back(current);
  }
  return result;
}

// Pad `text` to exactly `targetWidth` visible columns with spaces; no-op
// when already wide enough. ANSI sequences and UTF-8 multi-bytes don't
// count toward the visible width.
inline std::string padToWidth_(const std::string& text, int targetWidth) {
  const int visible = ansi::visibleWidth(text);
  if (visible >= targetWidth) return text;
  return text + std::string(targetWidth - visible, ' ');
}

// Render a Python code block: per-line syntax highlighting on top of a
// constant separator background, padded to full width so the bg extends to
// the right edge. A leading "  " indent keeps it visually aligned with the
// rest of the tool block. Long lines are wrapped with the same indent so
// the code block stays bounded.
std::vector<std::string> renderPythonCodeBlock(const std::string& code,
                                              int width) {
  const auto& theme = ThemeManager::instance().currentTheme();
  std::vector<std::string> result;
  if (code.empty()) return result;

  // Highlight the entire code block in one parser pass — keeps multi-line
  // strings and triple-quoted blocks coherent across lines.
  auto highlighted = SyntaxHighlighter::instance().highlightLines(
      code, "python", theme.base.fg);

  const std::string indent = "  ";
  const int indentVis = ansi::visibleWidth(indent);
  const int contentWidth = std::max(8, width - indentVis);

  for (const auto& hline : highlighted) {
    auto wrapped = wrapAnsi(hline, contentWidth, "");
    for (auto& wl : wrapped) {
      std::string row = indent + wl;
      // Pad to full width, then apply bg color to the whole row.
      row = padToWidth_(row, width);
      row = ansi::bgRgb(theme.base.separator.r, theme.base.separator.g,
                        theme.base.separator.b, row);
      result.push_back(std::move(row));
    }
  }
  return result;
}

// Render a Bash command with syntax highlighting and wrap it so long or
// multi-line commands flow onto continuation rows at `width`. The first
// line gets `prefix`; subsequent lines indent to align under the command
// body. Embedded newlines in the command (e.g. `python -c "a\nb\nc"`) are
// honored so each source line becomes its own row before width-wrapping
// kicks in — previously highlightLine() returned only the first line and
// the rest of the command was silently dropped.
std::vector<std::string> renderBashCommandLines(const std::string& prefix,
                                                const std::string& command,
                                                int width) {
  const auto& theme = ThemeManager::instance().currentTheme();
  std::string highlighted;
  if (SyntaxHighlighter::instance().hasGrammar("bash")) {
    auto highlightedLines = SyntaxHighlighter::instance().highlightLines(
        command, "bash", theme.base.fg);
    for (size_t i = 0; i < highlightedLines.size(); ++i) {
      if (i > 0) highlighted += '\n';
      highlighted += highlightedLines[i];
    }
  } else {
    highlighted = ansi::fgRgb(theme.base.fg.r, theme.base.fg.g, theme.base.fg.b,
                              command);
  }
  highlighted = ansi::bold(highlighted);

  const int prefixVis = ansi::visibleWidth(prefix);
  const int contentWidth = std::max(8, width - prefixVis);
  const std::string indent(prefixVis, ' ');

  auto wrapped = wrapAnsi(highlighted, contentWidth, indent);
  if (wrapped.empty()) wrapped.push_back("");
  wrapped[0] = prefix + wrapped[0];
  return wrapped;
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
  return {theme_ansi::warning(text)};
}

std::vector<std::string> renderPreparing(const std::string& toolName,
                                          const std::string& args, int width) {
  if (toolName == "Python") {
    return {theme_ansi::warning("  \xe2\x9a\x99 Python")};
  }
  // If args are already available (e.g. Wait action), show details immediately
  if (!args.empty()) {
    auto parsed = parseArgs(args);
    if (parsed.action == "Wait" && !parsed.processId.empty()) {
      return renderWaitPreparing(parsed);
    }
    if (parsed.action == "Execute" && !parsed.command.empty()) {
      return renderBashCommandLines(theme_ansi::warning("  \xe2\x98\x98 Bash "),
                                    parsed.command, width);
    }
    if (parsed.action == "Spawn" && !parsed.command.empty()) {
      return renderBashCommandLines(theme_ansi::warning("  \xe2\x98\x98 Bash "),
                                    parsed.command, width);
    }
  }
  return {theme_ansi::warning("  \xe2\x9a\x99 Process")};
}

std::vector<std::string> renderExecuteCalled(const ParsedArgs& args, const ToolCallItem& item,
                                              int width) {
  std::vector<std::string> result;
  // Header — bash-highlighted command, auto-wrapped to width.
  std::string cmd = args.command.empty() ? "..." : args.command;
  auto headerLines = renderBashCommandLines(
      theme_ansi::warning("  \xe2\x98\x98 Bash "), cmd, width);
  result.insert(result.end(), headerLines.begin(), headerLines.end());

  // Show live output if available
  const auto& stdout = item.processStdout();
  const auto& stderr = item.processStderr();

  if (stdout.empty() && stderr.empty()) {
    result.push_back(theme_ansi::dim("  \xe2\x94\x82 (waiting for output...)"));
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
      result.push_back(theme_ansi::dim("  \xe2\x94\x82 ") + theme_ansi::dim(line));
    }
  }

  // Footer with live timer
  result.push_back(theme_ansi::dim("  running \xe2\x80\xa2 " + elapsedStr(item)));
  return result;
}

std::vector<std::string> renderExecuteFinished(const ParsedArgs& args, const ParsedResult& res,
                                                const ToolCallItem& item, int width) {
  std::vector<std::string> result;
  bool success = item.success();
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";
  auto iconStr = "  " + icon + " ";
  std::string iconColored =
      success ? theme_ansi::success(iconStr) : theme_ansi::error(iconStr);

  std::string cmd = args.command.empty() ? "Bash" : args.command;
  auto headerLines = renderBashCommandLines(iconColored, cmd, width);
  result.insert(result.end(), headerLines.begin(), headerLines.end());

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
      result.push_back(theme_ansi::dim("  expand (Ctrl+E) to see more"));
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
  auto footerColor = success ? theme_ansi::success(footer)
                             : theme_ansi::error(footer);
  result.push_back(ansi::dim("  " + footerColor));

  // Timeout notice
  if (res.finishReason == "Timeout") {
    result.push_back(theme_ansi::warning("  ! Still running in background"));
  }
  return result;
}

std::vector<std::string> renderSpawnCalled(const ParsedArgs& args, const ToolCallItem& item,
                                            int width) {
  std::vector<std::string> result;
  std::string cmd = args.command.empty() ? "..." : args.command;
  auto headerLines = renderBashCommandLines(
      theme_ansi::warning("  \xe2\x98\x98 Bash "), cmd, width);
  result.insert(result.end(), headerLines.begin(), headerLines.end());

  // Show live output if available (same as Execute)
  const auto& stdout = item.processStdout();
  const auto& stderr = item.processStderr();

  if (stdout.empty() && stderr.empty()) {
    result.push_back(theme_ansi::dim("  \xe2\x94\x82 (waiting for output...)"));
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
      result.push_back(theme_ansi::dim("  \xe2\x94\x82 ") + theme_ansi::dim(line));
    }
  }

  result.push_back(theme_ansi::dim("  background \xe2\x80\xa2 " + elapsedStr(item)));
  return result;
}

std::vector<std::string> renderSpawnFinished(const ParsedArgs& args, const ParsedResult& res,
                                              const ToolCallItem& item, int width) {
  std::vector<std::string> result;
  bool success = item.success();
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";
  auto iconStr = "  " + icon + " ";
  std::string iconColored =
      success ? theme_ansi::success(iconStr) : theme_ansi::error(iconStr);
  std::string cmd = args.command.empty() ? "Bash" : args.command;
  auto headerLines = renderBashCommandLines(iconColored, cmd, width);
  result.insert(result.end(), headerLines.begin(), headerLines.end());

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
      result.push_back(theme_ansi::dim("  expand (Ctrl+E) to see more"));
    }
  }

  std::string footer = "background";
  if (!res.processId.empty()) footer += " \xe2\x80\xa2 pid " + res.processId;
  if (res.durationMs > 0) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << (res.durationMs / 1000.0) << "s";
    footer += " \xe2\x80\xa2 " + oss.str();
  }
  auto footerColor = success ? theme_ansi::success(footer)
                             : theme_ansi::error(footer);
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
  return {theme_ansi::success("  \xe2\x9c\x93 Process \xe2\x80\x94 " + statusText)};
}

std::vector<std::string> renderWaitCalled(const ParsedArgs& args, const ToolCallItem& item) {
  std::string text = "  \xe2\x9f\xb3 Waiting on " + args.processId;
  if (!args.pattern.empty()) text += " \xe2\x80\x94 pattern: \"" + args.pattern + "\"";
  if (args.timeoutMs > 0) text += " \xe2\x80\x94 timeout: " + std::to_string(args.timeoutMs / 1000) + "s";
  if (args.tailLines > 0) text += " \xe2\x80\x94 tail: " + std::to_string(args.tailLines) + " lines";
  text += "...";
  return {theme_ansi::warning(text),
          theme_ansi::dim("  " + elapsedStr(item))};
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
  return {theme_ansi::success(text)};
}

std::vector<std::string> renderInputCalled(const ParsedArgs& args) {
  return {theme_ansi::warning("  \xe2\x9a\x99 Process.input \xe2\x86\x92 " + args.processId)};
}

std::vector<std::string> renderInputFinished(const ParsedArgs& args, const ParsedResult& /*res*/) {
  int chars = static_cast<int>(args.input.size());
  return {theme_ansi::success(
      "  \xe2\x9c\x93 Sent " + std::to_string(chars) + " chars to process")};
}

std::vector<std::string> renderOutputCalled(const ParsedArgs& args) {
  return {theme_ansi::warning("  \xe2\x9a\x99 Process.output " + args.processId)};
}

std::vector<std::string> renderOutputFinished(const ParsedResult& res) {
  int bytes = static_cast<int>(res.output.size());
  return {theme_ansi::success(
      "  \xe2\x9c\x93 Output read \xe2\x80\x94 " + std::to_string(bytes) + " bytes")};
}

std::vector<std::string> renderKillCalled(const ParsedArgs& args) {
  return {theme_ansi::warning("  \xe2\x9a\x99 Process.kill " + args.processId)};
}

std::vector<std::string> renderKillFinished(const ParsedArgs& args, const ParsedResult& res) {
  if (res.isRunning) {
    return {theme_ansi::success("  \xe2\x9c\x93 Killed process " + args.processId)};
  }
  return {theme_ansi::success("  \xe2\x9c\x93 Process " + args.processId + " already dead")};
}

std::vector<std::string> renderListFinished(const ParsedResult& res) {
  return {theme_ansi::success(
      "  \xe2\x9c\x93 " + std::to_string(res.entryCount) + " processes")};
}

std::vector<std::string> renderPythonCalled(const ParsedArgs& args, const ToolCallItem& item,
                                             int width) {
  std::vector<std::string> result;
  result.push_back(theme_ansi::warning("  \xe2\x98\x98 Python"));

  // Show code block — bg-tinted, syntax-highlighted, full-width rows.
  if (!args.code.empty()) {
    result.push_back(theme_ansi::dim("  \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 code \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"));
    auto codeRows = renderPythonCodeBlock(args.code, width);
    result.insert(result.end(), codeRows.begin(), codeRows.end());
    result.push_back(theme_ansi::dim("  \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 output \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"));
  }

  result.push_back(theme_ansi::dim("  \xe2\x94\x82 (waiting for output...)"));
  result.push_back(theme_ansi::dim("  running \xe2\x80\xa2 " + elapsedStr(item)));
  return result;
}

std::vector<std::string> renderPythonFinished(const ParsedArgs& args, const ParsedResult& res,
                                               const ToolCallItem& item, int width) {
  std::vector<std::string> result;
  bool success = item.success();
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";
  auto iconColor = success ? theme_ansi::success("  " + icon + " ")
                           : theme_ansi::error("  " + icon + " ");
  result.push_back(iconColor + ansi::bold(theme_ansi::foreground("Python")));

  // Code block (highlighted)
  if (!args.code.empty()) {
    result.push_back(theme_ansi::dim("  \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 code \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"));
    auto codeRows = renderPythonCodeBlock(args.code, width);
    result.insert(result.end(), codeRows.begin(), codeRows.end());
  }

  // Output
  std::string output = res.stdoutData;
  if (!res.stderrData.empty()) {
    if (!output.empty()) output += "\n";
    output += res.stderrData;
  }
  if (!output.empty()) {
    result.push_back(theme_ansi::dim("  \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 output \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"));
    auto lines = AnsiOutputParser::toLines(output, width, item.isExpanded() ? -1 : 10);
    result.insert(result.end(), lines.begin(), lines.end());
  }

  // Footer
  std::ostringstream oss;
  oss << "exit " << res.exitCode;
  if (res.durationMs > 0) {
    oss << " \xe2\x80\xa2 " << std::fixed << std::setprecision(1) << (res.durationMs / 1000.0) << "s";
  }
  auto footerColor = success ? theme_ansi::success(oss.str())
                             : theme_ansi::error(oss.str());
  result.push_back(ansi::dim("  " + footerColor));
  return result;
}

} // namespace

std::vector<std::string> ProcessPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int width) const {
  // Preparing phase
  if (item.phase() == ToolPhase::Preparing) {
    return renderPreparing(item.toolName(), item.args(), width);
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
    if (item.phase() == ToolPhase::Called) return {theme_ansi::warning("  \xe2\x9a\x99 Process.status " + args.processId)};
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
    if (item.phase() == ToolPhase::Called) return {theme_ansi::warning("  \xe2\x9a\x99 Process.list")};
    return renderListFinished(parseResult(item.result()));
  }

  // Unknown action fallback
  if (item.phase() == ToolPhase::Called) {
    return {theme_ansi::warning("  \xe2\x9a\x99 " + item.toolName())};
  }
  bool success = item.success();
  auto color = success ? theme_ansi::success("  \xe2\x9c\x93 " + item.toolName())
                       : theme_ansi::error("  \xe2\x9c\x97 " + item.toolName());
  return {color};
}

} // namespace firmius::tui2
