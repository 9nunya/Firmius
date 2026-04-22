#include "tools/FleetStatusTool.hpp"
#include "tools/WorkSupport.hpp"
#include "AgentRegistry.hpp"
#include "persistence/ThreadManager.hpp"
#include <rapidjson/document.h>

namespace firmius::core {

namespace {

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

rapidjson::Value lockToJson(const FleetLock &lock,
                            rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value obj(rapidjson::kObjectType);
  obj.AddMember("lock_id", rapidjson::Value(lock.lockId.c_str(), alloc), alloc);
  obj.AddMember("thread_id", rapidjson::Value(lock.threadId.c_str(), alloc), alloc);
  obj.AddMember("root_agent_id", rapidjson::Value(lock.rootAgentId.c_str(), alloc), alloc);
  obj.AddMember("owner_agent_id", rapidjson::Value(lock.ownerAgentId.c_str(), alloc), alloc);
  obj.AddMember("status", rapidjson::Value(lockStatusOrDefault(lock).c_str(), alloc), alloc);
  obj.AddMember("reason", rapidjson::Value(lock.reason.c_str(), alloc), alloc);
  obj.AddMember("created_at", static_cast<uint64_t>(lock.createdAt), alloc);
  obj.AddMember("updated_at", static_cast<uint64_t>(lock.updatedAt), alloc);

  rapidjson::Value paths(rapidjson::kArrayType);
  for (const auto &p : lock.paths) {
    paths.PushBack(rapidjson::Value(p.c_str(), alloc), alloc);
  }
  obj.AddMember("paths", paths, alloc);

  return obj;
}

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
  const std::string threadId = work::requireCurrentThreadId(ctx);
  ThreadManager tm(ThreadManager::defaultBasePath());
  FleetState state = tm.getFleetState(threadId);

  std::string root = input.root_agent_id.value_or("");
  if (root.empty()) {
    root = resolveFleetRootId(ctx.agent.getContext().identity.id);
  }

  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  rapidjson::Value locks(rapidjson::kArrayType);
  for (const auto &lock : state.locks) {
    if (!root.empty() && lock.rootAgentId != root) {
      continue;
    }
    const std::string status = lockStatusOrDefault(lock);
    if (!input.include_closed &&
        (status == "released" || status == "failed" || status == "cancelled")) {
      continue;
    }
    locks.PushBack(lockToJson(lock, alloc), alloc);
  }
  doc.AddMember("locks", locks, alloc);
  return shared::ToolResult::ok(doc);
}

} // namespace firmius::core
