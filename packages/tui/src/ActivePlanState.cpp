#include "ActivePlanState.hpp"
#include <algorithm>
#include <array>
#include <sstream>
#include <type_traits>

namespace firmius::tui {

using firmius::shared::AppEvent;
using firmius::shared::ChunkAdded;
using firmius::shared::ChunkAssigned;
using firmius::shared::ChunkStatusChanged;
using firmius::shared::ChunkUpdated;
using firmius::shared::Plan;
using firmius::shared::PlanActivated;
using firmius::shared::PlanCreated;
using firmius::shared::PlanUpdated;
using firmius::shared::ThreadMetadata;
using firmius::shared::WorkChunk;
using firmius::shared::WorkChunkStatus;

namespace {

struct SummaryBucket {
  const char *label;
  int count = 0;
};

} // namespace

void ActivePlanState::hydrateForThread(const ThreadMetadata &thread,
                                       const std::optional<Plan> &plan) {
  model_.thread_id = thread.threadId;
  if (!plan.has_value() || thread.activePlanId.empty() ||
      plan->id != thread.activePlanId) {
    clear();
    model_.thread_id = thread.threadId;
    return;
  }

  active_plan_ = plan;
  rebuildModel();
}

bool ActivePlanState::handleEvent(const AppEvent &event,
                                  const std::string &current_thread_id) {
  return std::visit(
      [&](auto &&e) -> bool {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, PlanCreated>) {
          if (e.threadId != current_thread_id) {
            return false;
          }
          if (active_plan_.has_value() && active_plan_->id == e.plan.id) {
            return applyPlan(e.plan);
          }
          return false;
        } else if constexpr (std::is_same_v<T, PlanUpdated>) {
          if (e.threadId != current_thread_id) {
            return false;
          }
          if (active_plan_.has_value() && active_plan_->id == e.plan.id) {
            return applyPlan(e.plan);
          }
          return false;
        } else if constexpr (std::is_same_v<T, PlanActivated>) {
          if (e.threadId != current_thread_id) {
            return false;
          }
          return applyPlan(e.plan);
        } else if constexpr (std::is_same_v<T, ChunkAdded>) {
          if (e.threadId != current_thread_id) {
            return false;
          }
          return upsertChunk(e.planId, e.chunk);
        } else if constexpr (std::is_same_v<T, ChunkUpdated>) {
          if (e.threadId != current_thread_id) {
            return false;
          }
          return upsertChunk(e.planId, e.chunk);
        } else if constexpr (std::is_same_v<T, ChunkAssigned>) {
          if (e.threadId != current_thread_id) {
            return false;
          }
          return upsertChunk(e.planId, e.chunk);
        } else if constexpr (std::is_same_v<T, ChunkStatusChanged>) {
          if (e.threadId != current_thread_id) {
            return false;
          }
          return upsertChunk(e.planId, e.chunk);
        }

        return false;
      },
      event);
}

void ActivePlanState::setExpanded(bool expanded) {
  model_.expanded = expanded;
}

void ActivePlanState::toggleExpanded() { model_.expanded = !model_.expanded; }

bool ActivePlanState::isExpanded() const { return model_.expanded; }

bool ActivePlanState::hasActivePlan() const {
  return active_plan_.has_value() && model_.visible;
}

const std::optional<Plan> &ActivePlanState::activePlan() const {
  return active_plan_;
}

const PlanLaneModel &ActivePlanState::model() const { return model_; }

std::string ActivePlanState::statusLabel(WorkChunkStatus status) {
  switch (status) {
  case WorkChunkStatus::Draft:
    return "Draft";
  case WorkChunkStatus::Ready:
    return "Ready";
  case WorkChunkStatus::InProgress:
    return "In Progress";
  case WorkChunkStatus::Implemented:
    return "Implemented";
  case WorkChunkStatus::Verifying:
    return "Verifying";
  case WorkChunkStatus::Done:
    return "Done";
  case WorkChunkStatus::Blocked:
    return "Blocked";
  case WorkChunkStatus::Failed:
    return "Failed";
  case WorkChunkStatus::Cancelled:
    return "Cancelled";
  }
  return "Unknown";
}

std::string ActivePlanState::collapsedSummary(const Plan &plan) {
  SummaryBucket implementing{"implementing"};
  SummaryBucket implemented{"implemented"};
  SummaryBucket verifying{"verifying"};
  SummaryBucket waiting{"waiting"};
  SummaryBucket done{"done"};
  SummaryBucket blocked{"blocked"};
  SummaryBucket failed{"failed"};
  SummaryBucket cancelled{"cancelled"};

  for (const auto &chunk : plan.chunks) {
    switch (chunk.status) {
    case WorkChunkStatus::Draft:
    case WorkChunkStatus::Ready:
      ++waiting.count;
      break;
    case WorkChunkStatus::InProgress:
      ++implementing.count;
      break;
    case WorkChunkStatus::Implemented:
      ++implemented.count;
      break;
    case WorkChunkStatus::Verifying:
      ++verifying.count;
      break;
    case WorkChunkStatus::Done:
      ++done.count;
      break;
    case WorkChunkStatus::Blocked:
      ++blocked.count;
      break;
    case WorkChunkStatus::Failed:
      ++failed.count;
      break;
    case WorkChunkStatus::Cancelled:
      ++cancelled.count;
      break;
    }
  }

  std::ostringstream summary;
  summary << "Plan: " << plan.title;

  const std::array<SummaryBucket, 7> buckets = {
      implementing, implemented, verifying, waiting, done, blocked, failed};

  int rendered_buckets = 0;
  for (const auto &bucket : buckets) {
    if (bucket.count <= 0) {
      continue;
    }
    summary << " | " << bucket.count << " " << bucket.label;
    ++rendered_buckets;
  }

  if (rendered_buckets == 0) {
    summary << " | " << plan.chunks.size() << " chunks";
  }
  if (cancelled.count > 0) {
    summary << " | " << cancelled.count << " cancelled";
  }

  return summary.str();
}

bool ActivePlanState::applyPlan(const Plan &plan) {
  active_plan_ = plan;
  rebuildModel();
  return true;
}

bool ActivePlanState::upsertChunk(const std::string &plan_id,
                                  const WorkChunk &chunk) {
  if (!active_plan_.has_value() || active_plan_->id != plan_id) {
    return false;
  }

  auto it = std::find_if(active_plan_->chunks.begin(), active_plan_->chunks.end(),
                         [&](const WorkChunk &existing) {
                           return existing.id == chunk.id;
                         });
  if (it == active_plan_->chunks.end()) {
    active_plan_->chunks.push_back(chunk);
  } else {
    *it = chunk;
  }

  rebuildModel();
  return true;
}

void ActivePlanState::clear() {
  active_plan_.reset();
  model_.visible = false;
  model_.plan_id.clear();
  model_.plan_title.clear();
  model_.collapsed_summary.clear();
  model_.chunks.clear();
}

void ActivePlanState::rebuildModel() {
  if (!active_plan_.has_value()) {
    clear();
    return;
  }

  model_.visible = true;
  model_.thread_id = active_plan_->threadId;
  model_.plan_id = active_plan_->id;
  model_.plan_title = active_plan_->title;
  model_.collapsed_summary = collapsedSummary(*active_plan_);
  model_.chunks.clear();
  model_.chunks.reserve(active_plan_->chunks.size());

  for (const auto &chunk : active_plan_->chunks) {
    model_.chunks.push_back(
        PlanLaneChunkRow{chunk.id, chunk.title, chunk.status,
                         statusLabel(chunk.status)});
  }
}

} // namespace firmius::tui
