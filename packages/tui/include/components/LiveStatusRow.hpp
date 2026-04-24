#ifndef FIRMIUS_TUI_COMPONENTS_LIVE_STATUS_ROW_HPP
#define FIRMIUS_TUI_COMPONENTS_LIVE_STATUS_ROW_HPP

#include "SkinConfig.hpp"
#include "Context.hpp"

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

struct LiveStatusPlanRow {
  std::string title;
  firmius::shared::WorkChunkStatus status =
      firmius::shared::WorkChunkStatus::Ready;
};

struct LiveStatusRowModel {
  SkinConfig skin;

  // Current resolved phrase when no transition is active.
  std::string phrase;
  std::string phrase_mode;
  std::string focused_agent_id;
  std::string activity;

  // Phrase transition animation (Claudex funny live row).
  // When active, render a per-character staggered fade-out to background,
  // brief invisible pause, then staggered fade-in.
  bool phrase_transition_active = false;
  float phrase_transition_t = 1.0f; // 0..1
  std::string phrase_prev;
  std::string phrase_next;

  std::string elapsed;

  std::string todo_excerpt;
  bool has_todo_excerpt = false;
  std::string plan_title;
  std::vector<LiveStatusPlanRow> plan_rows;
  std::size_t hidden_plan_count = 0;
  bool has_plan_excerpt = false;

  bool busy = false;
};

// Render the Claudex persistent live row.
//
// Returns emptyElement() when model.skin.show_persistent_live_row is false or
// model.focused_agent_id is empty.
ftxui::Element RenderLiveStatusRow(const LiveStatusRowModel &model);

} // namespace firmius::tui

#endif
