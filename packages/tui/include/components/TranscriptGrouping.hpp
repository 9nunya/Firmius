#ifndef FIRMIUS_TUI_COMPONENTS_TRANSCRIPT_GROUPING_HPP
#define FIRMIUS_TUI_COMPONENTS_TRANSCRIPT_GROUPING_HPP

#include "utils/ToolView.hpp"
#include <optional>
#include <string>
#include <vector>

namespace firmius::tui {

enum class QuickToolCategory {
  None,
  Read,
  List,
  Search,
};

struct QuickToolDescriptor {
  QuickToolCategory category = QuickToolCategory::None;
  std::string target;
};

struct QuickToolGroupSummary {
  QuickToolCategory category = QuickToolCategory::None;
  std::vector<std::string> targets;
  bool has_preparing = false;
  bool has_live = false;
  bool has_error = false;
  int preparing_count = 0;
  int live_count = 0;
};

QuickToolCategory QuickToolCategoryForName(const std::string &name);
bool IsQuickToolCategory(QuickToolCategory category);
bool IsQuickInspectionTool(const std::string &name);
QuickToolDescriptor DescribeQuickToolCall(const shared::ToolCallView &view);
std::vector<std::string>
DedupeQuickToolTargets(const std::vector<std::string> &targets);
std::string QuickToolGroupLabel(const QuickToolGroupSummary &summary);

} // namespace firmius::tui

#endif
