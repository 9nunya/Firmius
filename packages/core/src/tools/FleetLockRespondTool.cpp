#include "tools/FleetLockRespondTool.hpp"
#include "AgentRegistry.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/StringUtil.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::core {

namespace {

std::string requireCurrentThreadId(shared::ToolContext &ctx) {
  const auto &context = ctx.agent.getContext();
  if (!context.history || context.history->threadId.empty()) {
    throw std::runtime_error("No current thread exists");
  }
  return context.history->threadId;
}

uint64_t nowEpochMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

} // namespace

shared::ToolMetadata FleetLockRespondTool::getMetadata() const {
  return {"fleet_lock_respond",
          "Fleet coordination operations: accept or deny a lock request from another agent.",
          shared::ToolScope::Delegation};
}

std::shared_ptr<shared::JSONSchema> FleetLockRespondTool::getSchema() const {
  return shared::zObject({
      {"request_id", shared::zString()->describe("Lock request ID to respond to")},
      {"accept", shared::zBoolean()->describe("True to accept, false to deny")},
      {"deny_reason", shared::zString()->setOptional()
                      ->describe("Reason for denial (e.g., 'already completed')")},
      {"estimated_ms", shared::zInteger()->setOptional()
                         ->describe("Estimated time to complete when accepting")},
  })->required({"request_id", "accept"});
}

shared::ToolResult FleetLockRespondTool::execute(const FleetLockRespondInput &input,
                                                 shared::ToolContext &ctx) {
  const std::string threadId = requireCurrentThreadId(ctx);
  const auto &identity = ctx.agent.getContext().identity;
  const std::string ownerId = identity.id;
  
  if (input.accept) {
    // Accepting: Create a lock that will block the requester until released
    ThreadManager tm(ThreadManager::defaultBasePath());
    
    FleetLock lock;
    lock.lockId = "req-" + input.request_id;
    lock.threadId = threadId;
    lock.ownerAgentId = ownerId;
    lock.rootAgentId = ownerId;
    lock.reason = "Lock request " + input.request_id + " accepted";
    lock.status = "open";
    lock.createdAt = nowEpochMs();
    lock.updatedAt = lock.createdAt;
    
    // Extract paths from the request message if available
    // For now, requester should have specified paths in their request
    
    tm.mutateFleetState(threadId, [&](FleetState &state) {
      state.locks.push_back(lock);
    });
    
    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    doc.AddMember("request_id", rapidjson::Value(input.request_id.c_str(), alloc), alloc);
    doc.AddMember("accepted", true, alloc);
    doc.AddMember("lock_id", rapidjson::Value(lock.lockId.c_str(), alloc), alloc);
    if (input.estimated_ms.has_value()) {
      doc.AddMember("estimated_ms", *input.estimated_ms, alloc);
    }
    return shared::ToolResult::ok(doc);
    
  } else {
    // Denying: Requester should unblock immediately
    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    doc.AddMember("request_id", rapidjson::Value(input.request_id.c_str(), alloc), alloc);
    doc.AddMember("accepted", false, alloc);
    if (input.deny_reason.has_value()) {
      doc.AddMember("deny_reason", rapidjson::Value(input.deny_reason->c_str(), alloc), alloc);
    }
    return shared::ToolResult::ok(doc);
  }
}

} // namespace firmius::core
