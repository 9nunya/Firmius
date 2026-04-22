#include "tools/McpToolPresentation.hpp"

#include "utils/ErrorCleaner.hpp"
#include "utils/ToolSummaries.hpp"

#include <cctype>
#include <rapidjson/document.h>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <algorithm>

namespace firmius::tui {

namespace {

using firmius::shared::SummarizeToolCall;
using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

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

bool ParseObject(const std::string &json, rapidjson::Document &doc) {
  doc.Parse(json.c_str());
  return !doc.HasParseError() && doc.IsObject();
}

std::string StringMember(const rapidjson::Value &value, const char *key) {
  if (value.IsObject() && value.HasMember(key) && value[key].IsString()) {
    return value[key].GetString();
  }
  return "";
}

std::vector<std::string> SplitLines(const std::string &text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  if (lines.empty() && !text.empty()) {
    lines.push_back(text);
  }
  return lines;
}

std::string SummarizeValueShape(const rapidjson::Value &value) {
  if (value.IsObject()) {
    return "object(" + std::to_string(static_cast<int>(value.MemberCount())) +
           " fields)";
  }
  if (value.IsArray()) {
    return "array(" + std::to_string(static_cast<int>(value.Size())) + ")";
  }
  if (value.IsString()) {
    return "string";
  }
  if (value.IsBool()) {
    return "bool";
  }
  if (value.IsNumber()) {
    return "number";
  }
  if (value.IsNull()) {
    return "null";
  }
  return "value";
}

std::string DecodeDynamicMcpNamePart(const std::string &value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t i = 0; i < value.size();) {
    if (i + 4 < value.size() && value[i] == '_' && value[i + 1] == 'x' &&
        std::isxdigit(static_cast<unsigned char>(value[i + 2])) &&
        std::isxdigit(static_cast<unsigned char>(value[i + 3])) &&
        value[i + 4] == '_') {
      const std::string hex = value.substr(i + 2, 2);
      const int parsed = std::stoi(hex, nullptr, 16);
      decoded.push_back(static_cast<char>(parsed));
      i += 5;
      continue;
    }
    decoded.push_back(value[i]);
    ++i;
  }
  return decoded;
}

bool ParseDynamicMcpToolName(const std::string &name, std::string &server,
                             std::string &tool) {
  constexpr std::string_view kPrefix = "mcp__";
  if (name.rfind(kPrefix.data(), 0) != 0) {
    return false;
  }
  const std::string encoded = name.substr(kPrefix.size());
  const std::size_t delim = encoded.find("__");
  if (delim == std::string::npos || delim == 0 || delim + 2 >= encoded.size()) {
    return false;
  }
  server = DecodeDynamicMcpNamePart(encoded.substr(0, delim));
  tool = DecodeDynamicMcpNamePart(encoded.substr(delim + 2));
  return !server.empty() && !tool.empty();
}

void AddRawToggleContract(ToolPresentation &presentation,
                          const ToolCallView &view) {
  presentation.toggle_labels.collapsed = "show raw";
  presentation.toggle_labels.expanded = "hide raw";
  if (view.result.empty()) {
    return;
  }

  presentation.expandable = true;
  presentation.expanded = view.show_result;
  if (view.show_result) {
    presentation.body_lines = SplitLines(view.result);
    if (presentation.body_lines.empty()) {
      presentation.body_lines.push_back(view.result);
    }
    return;
  }

  ToolPresentationNotice notice;
  notice.kind = ToolPresentationNoticeKind::Info;
  notice.text = "Raw payload hidden; use show raw to expand";
  presentation.notices.push_back(std::move(notice));
}

} // namespace

bool IsMcpFamilyTool(const std::string &tool_name) {
  return tool_name.rfind("mcp__", 0) == 0;
}

ToolPresentation BuildMcpToolPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = DeriveLifecycle(view);
  presentation.layout = ToolPresentationLayoutKind::BodyFirstPreview;
  presentation.density = ToolPresentationDensity::DetailHeavy;
  presentation.title = SummarizeToolCall(view.name, view.args, view.phase);
  presentation.subtitle = view.name.empty() ? "mcp" : view.name;
  presentation.compact_summary = presentation.title;

  rapidjson::Document args_doc;
  const bool has_args = ParseObject(view.args, args_doc);
  rapidjson::Document result_doc;
  const bool has_result_object = ParseObject(view.result, result_doc);

  std::string server_name = has_args ? StringMember(args_doc, "server_name") : "";
  std::string tool_name = has_args ? StringMember(args_doc, "tool_name") : "";
  std::string resource_uri = has_args ? StringMember(args_doc, "uri") : "";
  std::string prompt_name = has_args ? StringMember(args_doc, "prompt_name") : "";

  std::string dynamic_server;
  std::string dynamic_tool;
  if (ParseDynamicMcpToolName(view.name, dynamic_server, dynamic_tool)) {
    if (server_name.empty()) {
      server_name = dynamic_server;
    }
    if (tool_name.empty()) {
      tool_name = dynamic_tool;
    }
  }

  if (has_result_object) {
    if (server_name.empty()) {
      server_name = StringMember(result_doc, "server_name");
    }
    if (tool_name.empty()) {
      tool_name = StringMember(result_doc, "tool_name");
    }
    if (resource_uri.empty()) {
      resource_uri = StringMember(result_doc, "uri");
    }
    if (prompt_name.empty()) {
      prompt_name = StringMember(result_doc, "prompt_name");
    }
  }

  const auto add_identity = [&]() {
    if (!server_name.empty()) {
      presentation.facts.push_back({"Server", server_name});
      presentation.footer_badges.push_back(server_name);
    }
    if (!tool_name.empty()) {
      presentation.facts.push_back({"Tool", tool_name});
      presentation.footer_badges.push_back(tool_name);
    }
    if (!resource_uri.empty()) {
      presentation.facts.push_back({"Resource", resource_uri});
      presentation.footer_badges.push_back("resource");
    }
    if (!prompt_name.empty()) {
      presentation.facts.push_back({"Prompt", prompt_name});
      presentation.footer_badges.push_back(prompt_name);
    }
  };

  if (!tool_name.empty() && !server_name.empty()) {
    presentation.title = "MCP tool " + tool_name + " @ " + server_name;
  } else {
    presentation.title = "MCP dynamic tool call";
  }
  if (has_result_object && result_doc.HasMember("remote_result")) {
    presentation.facts.push_back(
        {"Remote result", SummarizeValueShape(result_doc["remote_result"])});
  }
  add_identity();

  presentation.compact_summary = presentation.title;

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    presentation.error_text = firmius::shared::ErrorCleaner::clean(view.result);
    AddRawToggleContract(presentation, view);
    return presentation;
  }

  AddRawToggleContract(presentation, view);
  return presentation;
}

} // namespace firmius::tui
