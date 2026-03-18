#ifndef FIRMIUS_ACTIVE_PLAN_STATE_HPP
#define FIRMIUS_ACTIVE_PLAN_STATE_HPP

#include "Context.hpp"
#include "Events.hpp"
#include <optional>
#include <string>
#include <vector>

namespace firmius::tui {

struct PlanLaneChunkRow {
  std::string id;
  std::string title;
  firmius::shared::WorkChunkStatus status =
      firmius::shared::WorkChunkStatus::Draft;
  std::string status_label;
};

struct PlanLaneModel {
  bool visible = false;
  bool expanded = false;
  std::string thread_id;
  std::string plan_id;
  std::string plan_title;
  std::string collapsed_summary;
  std::vector<PlanLaneChunkRow> chunks;
};

class ActivePlanState {
public:
  void hydrateForThread(const firmius::shared::ThreadMetadata &thread,
                        const std::optional<firmius::shared::Plan> &plan);
  bool handleEvent(const firmius::shared::AppEvent &event,
                   const std::string &current_thread_id);

  void setExpanded(bool expanded);
  void toggleExpanded();
  bool isExpanded() const;

  bool hasActivePlan() const;
  const std::optional<firmius::shared::Plan> &activePlan() const;
  const PlanLaneModel &model() const;

  static std::string statusLabel(firmius::shared::WorkChunkStatus status);
  static std::string collapsedSummary(const firmius::shared::Plan &plan);

private:
  bool applyPlan(const firmius::shared::Plan &plan);
  bool upsertChunk(const std::string &plan_id,
                   const firmius::shared::WorkChunk &chunk);
  void clear();
  void rebuildModel();

  std::optional<firmius::shared::Plan> active_plan_;
  PlanLaneModel model_;
};

} // namespace firmius::tui

#endif
