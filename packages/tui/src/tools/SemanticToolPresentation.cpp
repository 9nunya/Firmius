#include "tools/SemanticToolPresentation.hpp"

#include "utils/ErrorCleaner.hpp"
#include "utils/ToolSummaries.hpp"

namespace firmius::tui {
namespace {

using firmius::shared::SummarizeToolCall;
using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

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

void AddCollapsedRawNotice(ToolPresentation &presentation) {
  ToolPresentationNotice notice;
  notice.kind = ToolPresentationNoticeKind::Info;
  notice.text = "Raw payload hidden; use show raw to expand";
  presentation.notices.push_back(std::move(notice));
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
    std::string current;
    for (char ch : view.result) {
      if (ch == '\n') {
        presentation.body_lines.push_back(current);
        current.clear();
      } else {
        current.push_back(ch);
      }
    }
    if (!current.empty() || (!view.result.empty() && view.result.back() != '\n')) {
      presentation.body_lines.push_back(current);
    }
    if (presentation.body_lines.empty()) {
      presentation.body_lines.push_back(view.result);
    }
  } else {
    AddCollapsedRawNotice(presentation);
  }
}

} // namespace

bool IsSemanticFamilyTool(const std::string &tool_name) {
  return tool_name == "Lsp" || IsMatch(tool_name, "lsp") ||
         IsMatch(tool_name, "semantic");
}

ToolPresentation BuildSemanticToolPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = DeriveLifecycle(view);
  presentation.layout = ToolPresentationLayoutKind::BodyFirstPreview;
  presentation.density = ToolPresentationDensity::DetailHeavy;
  presentation.title = SummarizeToolCall(view.name, view.args, view.phase);
  presentation.subtitle = view.name.empty() ? "semantic" : view.name;
  presentation.compact_summary = presentation.title;
  presentation.facts.push_back({"Tool", view.name});

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    presentation.error_text = firmius::shared::ErrorCleaner::clean(view.result);
    AddRawToggleContract(presentation, view);
    return presentation;
  }

  AddRawToggleContract(presentation, view);
  return presentation;
}

} // namespace firmius::tui
