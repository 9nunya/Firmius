#ifndef FIRMIUS_TUI_TOOLS_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_TOOL_PRESENTATION_HPP

#include "tools/ProcessState.hpp"
#include "tools/SubagentState.hpp"
#include "utils/ToolView.hpp"
#include <optional>
#include <string>
#include <vector>

namespace firmius::tui {

enum class ToolPresentationLifecycle {
  Preparing,
  Running,
  Success,
  Error,
};

enum class ToolPresentationNoticeKind {
  Info,
  Warning,
  Error,
};

enum class ToolPresentationLayoutKind {
  BodyFirstStream,
  BodyFirstPreview,
  ResultsList,
  CompactFactCard,
};

enum class ToolPresentationDensity {
  OneLineSummary,
  CompactSummaryCard,
  BodyFirstSummary,
  DetailHeavy,
};

struct ToolPresentationFact {
  std::string key;
  std::string value;
};

struct ToolPresentationSection {
  std::string title;
  std::vector<std::string> lines;
};

struct ToolPresentationNotice {
  ToolPresentationNoticeKind kind = ToolPresentationNoticeKind::Info;
  std::string text;
};

struct ToolPresentation {
  ToolPresentationLifecycle lifecycle = ToolPresentationLifecycle::Preparing;
  ToolPresentationLayoutKind layout = ToolPresentationLayoutKind::CompactFactCard;
  ToolPresentationDensity density = ToolPresentationDensity::CompactSummaryCard;
  std::string title;
  std::string subtitle;
  std::string compact_summary;
  std::vector<std::string> body_lines;
  std::vector<ToolPresentationFact> facts;
  std::vector<ToolPresentationSection> sections;
  std::vector<std::string> footer_badges;
  std::optional<std::string> status_footer;
  std::vector<ToolPresentationNotice> notices;
  bool expandable = false;
  bool expanded = false;
  std::optional<std::string> error_text;
};

ToolPresentation BuildToolPresentation(
    const firmius::shared::ToolCallView &view,
    const NormalizedProcessState *process_state = nullptr,
    const NormalizedSubagentState *subagent_state = nullptr);

} // namespace firmius::tui

#endif
