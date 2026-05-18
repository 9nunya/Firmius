#include "tools/FleetStatusTool.hpp"
#include "AgentRegistry.hpp"
#include "persistence/ThreadManager.hpp"
#include <rapidjson/document.h>
#include <sstream>
#include <vector>

namespace firmius::core {

namespace {

std::string requireCurrentThreadId(shared::ToolContext &ctx) {
  const auto &context = ctx.agent.getContext();
  if (!context.history || context.history->threadId.empty()) {
    throw std::runtime_error("No current thread exists");
  }
  return context.history->threadId;
}

std::string resolveFleetRootId(const std::string &agentId) {
  if (agentId.empty()) {
    return "";
  }
  auto current = AgentRegistry::instance().getAgent(agentId);
  int depth = 0;
  while (current && depth < 100) {
    const std::string parentId = current->getContext().identity.parentId;
    if (parentId.empty()) {
      return current->getContext().identity.id;
    }
    current = AgentRegistry::instance().getAgent(parentId);
    depth++;
  }
  return agentId;
}

std::string lockStatusOrDefault(const FleetLock &lock) {
  if (!lock.status.empty()) {
    return lock.status;
  }
  return "open";
}

// Token-waste pass 3: lockToJson removed. The structured 9-field-per-lock
// representation has been replaced by an inline prose enumeration in
// FleetStatusTool::execute.

} // namespace

shared::ToolMetadata FleetStatusTool::getMetadata() const {
  return {"fleet_status",
          "Fleet coordination operations: inspect current locks for the thread or a specific root agent.",
          shared::ToolScope::Delegation};
}

std::shared_ptr<shared::JSONSchema> FleetStatusTool::getSchema() const {
  return shared::zObject({
      {"root_agent_id", shared::zString()->setOptional()
                            ->describe("Optional fleet root to filter by.")},
      {"include_closed", shared::zBoolean()->setOptional()
                             ->describe("Include released/failed locks.")},
  });
}

shared::ToolResult FleetStatusTool::execute(const FleetStatusInput &input,
                                            shared::ToolContext &ctx) {
  const std::string threadId = requireCurrentThreadId(ctx);
  ThreadManager tm(ThreadManager::defaultBasePath());
  FleetState state = tm.getFleetState(threadId);

  std::string root = input.root_agent_id.value_or("");
  if (root.empty()) {
    root = resolveFleetRootId(ctx.agent.getContext().identity.id);
  }

  // Token-waste pass 3: prose-first fleet status. Old shape was
  // `locks: [lockToJson(...), ...]` with 9 fields per lock. Now we
  // enumerate locks one per line in the prose `result`; the model can
  // still see lock_id (for release/wait), owner, paths, and status.
  std::vector<const FleetLock *> filtered;
  for (const auto &lock : state.locks) {
    if (!root.empty() && lock.rootAgentId != root) continue;
    const std::string status = lockStatusOrDefault(lock);
    if (!input.include_closed &&
        (status == "released" || status == "failed" || status == "cancelled")) {
      continue;
    }
    filtered.push_back(&lock);
  }

  std::ostringstream prose;
  if (filtered.empty()) {
    prose << "No active fleet locks.";
  } else {
    prose << filtered.size() << " active fleet lock"
          << (filtered.size() == 1 ? "" : "s") << ":\n";
    for (const auto *lock : filtered) {
      prose << "  " << lock->lockId << " — "
            << lockStatusOrDefault(*lock) << ", owner=" << lock->ownerAgentId;
      if (!lock->reason.empty()) prose << ", reason=" << lock->reason;
      if (!lock->paths.empty()) {
        prose << ", paths=[";
        for (size_t i = 0; i < lock->paths.size(); ++i) {
          if (i) prose << ", ";
          prose << lock->paths[i];
        }
        prose << "]";
      }
      prose << "\n";
    }
  }

  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  const std::string proseStr = prose.str();
  doc.AddMember(
      "result",
      rapidjson::Value(proseStr.c_str(),
                       static_cast<rapidjson::SizeType>(proseStr.size()),
                       alloc).Move(),
      alloc);
  doc.AddMember("count", static_cast<uint32_t>(filtered.size()), alloc);
  return shared::ToolResult::ok(doc);
}

} // namespace firmius::core
