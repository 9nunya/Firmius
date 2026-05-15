#ifndef FIRMIUS_CORE_WORK_SUPPORT_HPP
#define FIRMIUS_CORE_WORK_SUPPORT_HPP

#include "ITool.hpp"
#include "harness/Harness.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
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

} // namespace firmius::core::work

#endif
