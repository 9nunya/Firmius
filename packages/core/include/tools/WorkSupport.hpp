#ifndef FIRMIUS_CORE_WORK_SUPPORT_HPP
#define FIRMIUS_CORE_WORK_SUPPORT_HPP

#include "ITool.hpp"
#include "harness/Harness.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>

#include <set>
#include <chrono>
#include <stdexcept>
#include <string_view>
#include <string>

namespace firmius::core::work {

inline std::string requireCurrentThreadId(shared::ToolContext &ctx) {
  const auto &context = ctx.agent.getContext();
  if (!context.history || context.history->threadId.empty()) {
    throw std::runtime_error("No current thread exists");
  }
  return context.history->threadId;
}

inline uint64_t nowEpochMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

inline void emitWorkEvent(const shared::AppEvent &event) {
  Harness::instance().publishEvent(event);
}


// ===================== Plan/chunk helpers (shared) =====================

inline std::string chunkStatusToString(shared::WorkChunkStatus status) {
  switch (status) {
  case shared::WorkChunkStatus::Ready:
    return "Ready";
  case shared::WorkChunkStatus::InProgress:
    return "InProgress";
  case shared::WorkChunkStatus::Implemented:
    return "Implemented";
  case shared::WorkChunkStatus::Verifying:
    return "Verifying";
  case shared::WorkChunkStatus::Done:
    return "Done";
  case shared::WorkChunkStatus::Blocked:
    return "Blocked";
  case shared::WorkChunkStatus::Failed:
    return "Failed";
  case shared::WorkChunkStatus::Cancelled:
    return "Cancelled";
  }
  return "Ready";
}

inline shared::WorkChunk &requireChunk(shared::Plan &plan,
                                       const std::string &chunkId) {
  auto it = std::find_if(plan.chunks.begin(), plan.chunks.end(),
                         [&](const shared::WorkChunk &chunk) {
                           return chunk.id == chunkId;
                         });
  if (it == plan.chunks.end()) {
    throw std::runtime_error("Chunk not found: " + chunkId);
  }
  return *it;
}

inline const shared::WorkChunk &requireChunk(const shared::Plan &plan,
                                             const std::string &chunkId) {
  auto it = std::find_if(plan.chunks.begin(), plan.chunks.end(),
                         [&](const shared::WorkChunk &chunk) {
                           return chunk.id == chunkId;
                         });
  if (it == plan.chunks.end()) {
    throw std::runtime_error("Chunk not found: " + chunkId);
  }
  return *it;
}

inline const shared::WorkChunk *findChunkById(const shared::Plan &plan,
                                              std::string_view chunkId) {
  auto it = std::find_if(plan.chunks.begin(), plan.chunks.end(),
                         [&](const shared::WorkChunk &chunk) {
                           return chunk.id == chunkId;
                         });
  if (it == plan.chunks.end()) {
    return nullptr;
  }
  return &*it;
}

inline const shared::WorkChunk *findChunkByUniqueTitle(const shared::Plan &plan,
                                                       std::string_view title) {
  const shared::WorkChunk *match = nullptr;
  for (const auto &chunk : plan.chunks) {
    if (chunk.title != title) {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = &chunk;
  }
  return match;
}

inline const shared::WorkChunk *findDependencyChunk(const shared::Plan &plan,
                                                    const std::string &dependencyRef) {
  if (const auto *byId = findChunkById(plan, dependencyRef)) {
    return byId;
  }
  return findChunkByUniqueTitle(plan, dependencyRef);
}

inline bool chunkDependenciesDone(const shared::Plan &plan,
                                  const shared::WorkChunk &chunk) {
  for (const auto &dependencyId : chunk.dependsOn) {
    const auto *dependency = findDependencyChunk(plan, dependencyId);
    if (dependency == nullptr ||
        dependency->status != shared::WorkChunkStatus::Done) {
      return false;
    }
  }
  return true;
}

inline bool blockChunkIfDependenciesIncomplete(const shared::Plan &plan,
                                               shared::WorkChunk &chunk) {
  if (chunk.status != shared::WorkChunkStatus::Ready ||
      chunkDependenciesDone(plan, chunk)) {
    return false;
  }
  chunk.status = shared::WorkChunkStatus::Blocked;
  return true;
}

inline void requireChunkReadyForExecution(const shared::Plan &plan,
                                          const shared::WorkChunk &chunk,
                                          const std::string &action) {
  if (chunk.status != shared::WorkChunkStatus::Ready ||
      !chunkDependenciesDone(plan, chunk)) {
    throw std::runtime_error("Chunk '" + chunk.id + "' is not ready for " + action +
                             ": status is " + chunkStatusToString(chunk.status) +
                             "; chunk must be Ready and all dependencies must be Done");
  }
}

inline void validateExecutorAssignmentInvariant(ThreadManager &tm,
                                                const std::string &threadId,
                                                const std::string &planId,
                                                const std::string &chunkId,
                                                const std::string &agentId) {
  if (agentId.empty()) {
    return;
  }

  for (const auto &candidatePlan : tm.listPlans(threadId)) {
    for (const auto &candidateChunk : candidatePlan.chunks) {
      if (candidatePlan.id == planId && candidateChunk.id == chunkId) {
        if (!candidateChunk.assignedAgentId.empty() &&
            candidateChunk.assignedAgentId != agentId) {
          throw std::runtime_error("Work authority denied: chunk '" + chunkId +
                                   "' is already assigned to executor '" +
                                   candidateChunk.assignedAgentId + "'");
        }
        continue;
      }
      if (candidateChunk.assignedAgentId != agentId) {
        continue;
      }
      throw std::runtime_error("Work authority denied: executor '" + agentId +
                               "' already owns chunk '" + candidateChunk.id + "'");
    }
  }
}

inline std::string buildExecutorLockDoctrine() {
  return R"(
## Fleet Coordination Doctrine for Executors

The goal is not "put a generic lock on every file."
The goal is to prevent workers from colliding on unstable shared surfaces during implementation and verification.

### Mental Model

Think in terms of **edit ownership until stable**.
If worker A is still modifying or stabilizing a shared surface, worker B should not race in and "help fix" that same surface during verification.

### Tool Reference
fleet_lock check/acquire/release/request/wait
)";
}

inline std::string buildWorkerLockDoctrine() {
  return R"(
## Fleet Coordination Doctrine for Workers

Coordinate with peer workers to avoid racing on unstable shared surfaces.

### Tool Reference
fleet_lock check/acquire/release/request/wait
)";
}
inline bool reconcileChunkDependencies(shared::Plan &plan) {
  bool changed = false;
  for (auto &chunk : plan.chunks) {
    if (chunk.status == shared::WorkChunkStatus::Blocked) {
      bool done = true;
      for (const auto &dependencyId : chunk.dependsOn) {
        auto it = std::find_if(plan.chunks.begin(), plan.chunks.end(),
                               [&](const shared::WorkChunk &candidate) {
                                 return candidate.id == dependencyId ||
                                        candidate.title == dependencyId;
                               });
        if (it == plan.chunks.end() || it->status != shared::WorkChunkStatus::Done) {
          done = false;
          break;
        }
      }
      if (done) {
        chunk.status = shared::WorkChunkStatus::Ready;
        chunk.updatedAt = nowEpochMs();
        changed = true;
      }
    }
  }
  return changed;
}

} // namespace firmius::core::work

#endif
