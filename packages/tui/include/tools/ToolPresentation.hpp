#ifndef FIRMIUS_TUI_TOOLS_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_TOOL_PRESENTATION_HPP

#include "tools/ProcessState.hpp"
#include "tools/SubagentState.hpp"
#include "utils/ToolView.hpp"
#include <ftxui/dom/elements.hpp>
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
  InlineStatusRow,
  BodyFirstStream,
  BodyFirstPreview,
  DiffPreview,
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
  ToolPresentationNoticeKind kind = ToolPresentationNoticeKind::Info;
  std::vector<std::string> lines;
};

struct ToolPresentationDiffLine {
  char type = ' ';
  int old_line = 0;
  int new_line = 0;
  std::string content;
  bool highlight_background = false;
};

struct ToolPresentationDiffSection {
  std::string title;
  std::string meta;
  std::optional<std::string> error_text;
  std::optional<std::string> empty_state_text;
  std::vector<ToolPresentationDiffLine> lines;
};

struct ToolPresentationNotice {
  ToolPresentationNoticeKind kind = ToolPresentationNoticeKind::Info;
  std::string text;
};

struct ToolPresentationToggleLabels {
  std::string collapsed = "show more";
  std::string expanded = "hide";
};

struct ToolPresentation {
  ToolPresentationLifecycle lifecycle = ToolPresentationLifecycle::Preparing;
  ToolPresentationLayoutKind layout = ToolPresentationLayoutKind::CompactFactCard;
  ToolPresentationDensity density = ToolPresentationDensity::CompactSummaryCard;
  std::string title;
  std::string subtitle;
  std::string compact_summary;
  std::vector<std::string> body_lines;
  std::vector<ftxui::Element> custom_body_elements;
  std::string diff_source_name;
  std::vector<ToolPresentationDiffSection> diff_sections;
  std::vector<ToolPresentationFact> facts;
  std::vector<ToolPresentationSection> sections;
  std::vector<std::string> footer_badges;
  std::optional<std::string> status_footer;
  std::vector<ToolPresentationNotice> notices;
  bool expandable = false;
  bool expanded = false;
  std::optional<std::string> error_text;
  bool ansi_aware = false;
  std::optional<std::string> custom_icon;
  ToolPresentationToggleLabels toggle_labels;
};

ToolPresentation BuildToolPresentation(
    const firmius::shared::ToolCallView &view,
    const NormalizedProcessState *process_state = nullptr,
    const NormalizedSubagentState *subagent_state = nullptr);

} // namespace firmius::tui

#endif
