#include "tools/GenericToolPresentation.hpp"

#include "utils/ErrorCleaner.hpp"
#include "utils/ToolSummaries.hpp"

#include <sstream>

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

int CountLines(const std::string &text) {
  std::istringstream stream(text);
  int count = 0;
  std::string line;
  while (std::getline(stream, line)) {
    count++;
  }
  return count;
}

} // namespace

ToolPresentation BuildGenericToolPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = DeriveLifecycle(view);
  presentation.layout = ToolPresentationLayoutKind::CompactFactCard;
  presentation.density = ToolPresentationDensity::DetailHeavy;
  presentation.title = SummarizeToolCall(view.name, view.args, view.phase);
  presentation.subtitle = view.name.empty() ? "tool" : view.name;
  presentation.compact_summary = presentation.title;
  presentation.facts.push_back(
      {"Tool", view.name.empty() ? std::string("(unnamed)") : view.name});

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    presentation.error_text = firmius::shared::ErrorCleaner::clean(view.result);
    presentation.title += " failed";
    return presentation;
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing ||
      presentation.lifecycle == ToolPresentationLifecycle::Running) {
    return presentation;
  }

  if (view.result.empty()) {
    return presentation;
  }

  const int total_line_count = CountLines(view.result);
  presentation.expandable = false;
  presentation.expanded = false;
  presentation.facts.push_back({"Output lines", std::to_string(total_line_count)});

  ToolPresentationSection section;
  section.title = "Result";
  std::istringstream result_stream(view.result);
  std::string result_line;
  while (std::getline(result_stream, result_line)) {
    section.lines.push_back(result_line);
  }
  presentation.body_lines = section.lines;
  presentation.sections.push_back(std::move(section));

  return presentation;
}

} // namespace firmius::tui
