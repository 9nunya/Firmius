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
      firmius::shared::WorkChunkStatus::Ready;
  std::string status_label;
  std::optional<size_t> task_count;  ///< V2: number of tasks in chunk (if any)
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

// V2: Executor-focused chunk detail model
struct ChunkDetailModel {
  bool visible = false;
  std::string chunk_id;
  std::string chunk_title;
  std::string chunk_goal;
  std::string chunk_context;
  std::string chunk_constraints;
  std::string chunk_completion;
  std::string verification_condition;
  std::string handoff_notes;
  firmius::shared::WorkChunkStatus status =
      firmius::shared::WorkChunkStatus::Ready;
  std::string status_label;
  
  // V2 task structure
  struct TaskRow {
    std::string id;
    std::string title;
    std::string goal;
    firmius::shared::WorkChunkStatus status =
        firmius::shared::WorkChunkStatus::Ready;
    std::string status_label;
    std::string notes;
    std::string verification_condition;
  };
  std::vector<TaskRow> tasks;
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
  
  // V2: Executor-focused chunk detail
  void setFocusedChunk(const std::string &chunk_id);
  const std::optional<ChunkDetailModel> &focusedChunk() const;

  static std::string statusLabel(firmius::shared::WorkChunkStatus status);
  static std::string collapsedSummary(const firmius::shared::Plan &plan);

private:
  bool applyPlan(const firmius::shared::Plan &plan);
  bool upsertChunk(const std::string &plan_id,
                   const firmius::shared::WorkChunk &chunk);
  void clear();
  void rebuildModel();
  void rebuildFocusedChunk();

  std::optional<firmius::shared::Plan> active_plan_;
  PlanLaneModel model_;
  std::optional<ChunkDetailModel> focused_chunk_;
};

} // namespace firmius::tui

#endif
