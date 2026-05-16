#include "tools/ArtifactToolPresentation.hpp"
#include "tools/FileToolPresentation.hpp"
#include "tools/ProcessToolPresentation.hpp"
#include "tools/SubagentToolPresentation.hpp"
#include "tools/ToolPresentation.hpp"
#include "tools/PythonToolPresentation.hpp"
#include "tools/McpToolPresentation.hpp"
#include "tools/ModeSwitchToolPresentation.hpp"
#include "tools/SearchToolPresentation.hpp"
#include "tools/SemanticToolPresentation.hpp"
#include "tools/WebSearchToolPresentation.hpp"
#include "tools/GenericToolPresentation.hpp"
#include <rapidjson/document.h>

namespace firmius::tui {

using firmius::shared::ToolCallView;

namespace {

bool IsMatch(const std::string &actual, const std::string &expected) {
  if (actual.empty() || expected.empty()) {
    return false;
  }
  return actual.find(expected) != std::string::npos;
}

std::string ExtractAction(const ToolCallView &view) {
  rapidjson::Document doc;
  doc.Parse(view.args.c_str());
  if (!view.args.empty() && !doc.HasParseError() && doc.IsObject() && doc.HasMember("action") &&
      doc["action"].IsString()) {
    return doc["action"].GetString();
  }
  return "";
}

std::optional<ToolPresentation>
TryBuildSpecializedPresentation(const ToolCallView &view,
                                const NormalizedProcessState *process_state,
                                const NormalizedSubagentState *subagent_state) {
  // ModeSwitch dispatches first — its result is a small JSON envelope and
  // the generic fact-card renderer turns it into a wall of text. We want
  // a single inline status row instead.
  if (IsModeSwitchTool(view.name)) {
    return BuildModeSwitchToolPresentation(view);
  }
  if (view.name == "Python" || IsMatch(view.name, "python_execute")) {
    return BuildPythonToolPresentation(view, process_state);
  }
  if (IsProcessFamilyTool(view.name)) {
    return BuildProcessToolPresentation(view, process_state);
  }
  if (IsSubagentFamilyTool(view.name)) {
    return BuildSubagentToolPresentation(view, subagent_state);
  }
  const std::string action = ExtractAction(view);
  if (view.name == "Files" && (action == "Grep" || action == "Glob" || action == "Search")) {
    return BuildSearchToolPresentation(view);
  }
  if (IsArtifactFamilyTool(view.name)) {
    return BuildArtifactToolPresentation(view);
  }
  if (IsWebSearchFamilyTool(view.name)) {
    return (IsMatch(view.name, "web_fetch") || action == "Fetch" || action == "Web")
               ? BuildWebFetchToolPresentation(view)
               : BuildWebSearchToolPresentation(view);
  }
  if (IsMcpFamilyTool(view.name)) {
    return BuildMcpToolPresentation(view);
  }
  if (IsFileFamilyTool(view.name)) {
    return BuildFileToolPresentation(view);
  }
  if (IsSearchFamilyTool(view.name)) {
    return BuildSearchToolPresentation(view);
  }
  if (IsSemanticFamilyTool(view.name)) {
    return BuildSemanticToolPresentation(view);
  }
  return std::nullopt;
}


} // namespace

ToolPresentation BuildToolPresentation(
    const firmius::shared::ToolCallView &view,
    const NormalizedProcessState *process_state,
    const NormalizedSubagentState *subagent_state) {
  if (auto specialized =
          TryBuildSpecializedPresentation(view, process_state, subagent_state)) {
    return *specialized;
  }
  return BuildGenericToolPresentation(view);
}

} // namespace firmius::tui
