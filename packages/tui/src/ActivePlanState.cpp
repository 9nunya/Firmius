#include "ActivePlanState.hpp"

namespace firmius::tui {

void ActivePlanState::hydrateForThread(
    const firmius::shared::ThreadMetadata &thread,
    const std::optional<firmius::shared::Plan> & /*plan*/) {
  clear();
  model_.thread_id = thread.threadId;
}

bool ActivePlanState::handleEvent(const firmius::shared::AppEvent & /*event*/,
                                  const std::string & /*current_thread_id*/) {
  return false;
}

void ActivePlanState::setExpanded(bool expanded) { model_.expanded = expanded; }

void ActivePlanState::toggleExpanded() { model_.expanded = !model_.expanded; }

bool ActivePlanState::isExpanded() const { return model_.expanded; }

bool ActivePlanState::hasActivePlan() const { return false; }

const std::optional<firmius::shared::Plan> &ActivePlanState::activePlan() const {
  return active_plan_;
}

const PlanLaneModel &ActivePlanState::model() const { return model_; }

std::string ActivePlanState::statusLabel(firmius::shared::WorkChunkStatus status) {
  switch (status) {
  case firmius::shared::WorkChunkStatus::Ready:
    return "Ready";
  case firmius::shared::WorkChunkStatus::InProgress:
    return "In Progress";
  case firmius::shared::WorkChunkStatus::Implemented:
    return "Implemented";
  case firmius::shared::WorkChunkStatus::Verifying:
    return "Verifying";
  case firmius::shared::WorkChunkStatus::Done:
    return "Done";
  case firmius::shared::WorkChunkStatus::Blocked:
    return "Blocked";
  case firmius::shared::WorkChunkStatus::Failed:
    return "Failed";
  case firmius::shared::WorkChunkStatus::Cancelled:
    return "Cancelled";
  }
  return "Unknown";
}

std::string ActivePlanState::collapsedSummary(
    const firmius::shared::Plan &plan) {
  return plan.title.empty() ? std::string{"Plan removed"} : plan.title;
}

bool ActivePlanState::applyPlan(const firmius::shared::Plan & /*plan*/) {
  clear();
  return false;
}

bool ActivePlanState::upsertChunk(const std::string & /*plan_id*/,
                                  const firmius::shared::WorkChunk & /*chunk*/) {
  return false;
}

void ActivePlanState::clear() {
  active_plan_.reset();
  focused_chunk_.reset();
  model_.visible = false;
  model_.plan_id.clear();
  model_.plan_title.clear();
  model_.collapsed_summary.clear();
  model_.chunks.clear();
  model_.executor_task_view = false;
  model_.executor_chunk_id.clear();
  model_.executor_chunk_title.clear();
  model_.executor_tasks.clear();
  model_.highlight_chunk_id.clear();
  model_.toggle_hint = "Todo";
  model_.focused_chunk.reset();
}

void ActivePlanState::rebuildModel() {
  model_.visible = false;
  model_.chunks.clear();
  model_.focused_chunk.reset();
}

void ActivePlanState::rebuildFocusedChunk() { model_.focused_chunk = focused_chunk_; }

void ActivePlanState::setFocusedChunk(const std::string & /*chunk_id*/) {
  focused_chunk_.reset();
  model_.focused_chunk.reset();
}

const std::optional<ChunkDetailModel> &ActivePlanState::focusedChunk() const {
  return focused_chunk_;
}

} // namespace firmius::tui
