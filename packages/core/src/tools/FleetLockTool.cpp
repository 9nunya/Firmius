#include "tools/FleetLockTool.hpp"
#include "AgentRegistry.hpp"
#include "harness/Harness.hpp"
#include "persistence/ThreadManager.hpp"
#include "tools/WorkToolCommon.hpp"
#include "utils/StringUtil.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <chrono>
#include <thread>

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

FleetLock *findLock(FleetState &state, const std::string &lockId) {
  for (auto &lock : state.locks) {
    if (lock.lockId == lockId) {
      return &lock;
    }
  }
  return nullptr;
}

FleetLock *findLockByPaths(FleetState &state, const std::vector<std::string> &paths) {
  for (auto &lock : state.locks) {
    if (lock.paths.empty()) continue;
    for (const auto &lockPath : lock.paths) {
      for (const auto &reqPath : paths) {
        if (lockPath == reqPath) {
          return &lock;
        }
      }
    }
  }
  return nullptr;
}

} // namespace

shared::ToolMetadata FleetLockTool::getMetadata() const {
  return {"fleet_lock",
          "Acquire, release, request, or check file locks for worker coordination.",
          shared::ToolScope::Delegation};
}

std::shared_ptr<shared::JSONSchema> FleetLockTool::getSchema() const {
  auto modeEnum = shared::zEnum({"acquire", "release", "request", "wait", "check"})
    ->describe("Lock operation mode");
  
  return shared::zObject({
      {"mode", modeEnum->setOptional()},
      {"lock_id", shared::zString()->setOptional()
                    ->describe("Lock ID (required for release/wait, returned by acquire/request)")},
      {"reason", shared::zString()->setOptional()
                   ->describe("Reason for lock (required for acquire/request)")},
      {"paths", shared::zArray(shared::zString())->setOptional()
                    ->describe("File paths to lock")},
      {"target_agent_id", shared::zString()->setOptional()
                            ->describe("Target agent for lock request")},
      {"timeout_ms", shared::zInteger()->setOptional()
                       ->describe("Max time to wait (ms)")},
  });
}

shared::ToolResult FleetLockTool::execute(const FleetLockInput &input,
                                          shared::ToolContext &ctx) {
  const std::string threadId = worktools::requireCurrentThreadId(ctx);
  const auto &identity = ctx.agent.getContext().identity;
  const std::string ownerId = identity.id;
  
  if (input.mode == "acquire") {
    if (input.reason.empty()) {
      return shared::ToolResult::fail("acquire mode requires 'reason'");
    }
    if (input.paths.empty()) {
      return shared::ToolResult::fail("acquire mode requires 'paths'");
    }
    
    ThreadManager tm(ThreadManager::defaultBasePath());
    
    // Check for existing lock on these paths
    FleetLock *existingLock = nullptr;
    tm.mutateFleetState(threadId, [&](FleetState &state) {
      existingLock = findLockByPaths(state, input.paths);
    });
    
    if (existingLock && existingLock->ownerAgentId != ownerId) {
      // Wait for lock if timeout specified
      if (input.timeout_ms.has_value() && *input.timeout_ms > 0) {
        const uint64_t start = worktools::nowEpochMs();
        while (true) {
          bool lockReleased = false;
          tm.mutateFleetState(threadId, [&](FleetState &state) {
            auto *lock = findLock(state, existingLock->lockId);
            if (!lock) {
              lockReleased = true;
              return;
            }
            const std::string status = lockStatusOrDefault(*lock);
            if (status != "open") {
              lockReleased = true;
              return;
            }
          });
          
          if (lockReleased) {
            break;
          }
          
          const uint64_t now = worktools::nowEpochMs();
          if (now - start >= static_cast<uint64_t>(*input.timeout_ms)) {
            return shared::ToolResult::fail("Lock acquire timed out");
          }
          
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
      } else if (existingLock) {
        return shared::ToolResult::fail("Files locked by agent " + existingLock->ownerAgentId);
      }
    }
    
    // Create new lock
    FleetLock lock;
    lock.lockId = shared::StringUtil::generateUuid();
    lock.threadId = threadId;
    lock.ownerAgentId = ownerId;
    lock.rootAgentId = resolveFleetRootId(ownerId);
    lock.reason = input.reason;
    lock.paths = input.paths;
    lock.status = "open";
    lock.createdAt = worktools::nowEpochMs();
    lock.updatedAt = lock.createdAt;

    tm.mutateFleetState(threadId, [&](FleetState &state) {
      state.locks.push_back(lock);
    });

    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    doc.AddMember("lock_id", rapidjson::Value(lock.lockId.c_str(), alloc), alloc);
    doc.AddMember("mode", rapidjson::Value("acquire", alloc), alloc);
    doc.AddMember("paths", [&]() {
      rapidjson::Value arr(rapidjson::kArrayType);
      for (const auto &p : input.paths) {
        arr.PushBack(rapidjson::Value(p.c_str(), alloc), alloc);
      }
      return arr;
    }(), alloc);
    return shared::ToolResult::ok(doc);
    
  } else if (input.mode == "release") {
    if (input.lock_id.empty()) {
      return shared::ToolResult::fail("release mode requires 'lock_id'");
    }
    
    ThreadManager tm(ThreadManager::defaultBasePath());
    bool found = false;
    bool released = false;
    
    tm.mutateFleetState(threadId, [&](FleetState &state) {
      auto *lock = findLock(state, input.lock_id);
      if (!lock) {
        return;
      }
      found = true;
      if (lock->ownerAgentId != ownerId) {
        return;
      }
      lock->status = "released";
      lock->updatedAt = worktools::nowEpochMs();
      released = true;
    });
    
    if (!found) {
      return shared::ToolResult::fail("Lock not found");
    }
    if (!released) {
      return shared::ToolResult::fail("Lock not owned by you");
    }
    
    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    doc.AddMember("lock_id", rapidjson::Value(input.lock_id.c_str(), alloc), alloc);
    doc.AddMember("mode", rapidjson::Value("release", alloc), alloc);
    doc.AddMember("status", rapidjson::Value("released", alloc), alloc);
    return shared::ToolResult::ok(doc);
    
  } else if (input.mode == "request") {
    if (!input.target_agent_id.has_value() || input.target_agent_id->empty()) {
      return shared::ToolResult::fail("request mode requires 'target_agent_id'");
    }
    if (input.paths.empty()) {
      return shared::ToolResult::fail("request mode requires 'paths'");
    }
    
    std::string requestId = shared::StringUtil::generateUuid();
    
    // Queue request to target agent
    std::string requestMsg = "LOCK_REQUEST|" + requestId + "|" + 
                             ownerId + "|" + input.reason + "|";
    for (size_t i = 0; i < input.paths.size(); ++i) {
      if (i > 0) requestMsg += ",";
      requestMsg += input.paths[i];
    }
    
    Harness::instance().queueInternalMessage(
        *input.target_agent_id, threadId, requestMsg);
    
    // Wait for response
    const uint64_t start = worktools::nowEpochMs();
    const int timeout = input.timeout_ms.value_or(120000);
    
    while (true) {
      // Check for response in a shared state location
      // For now, simplified: just wait and check if paths become available
      bool pathsFree = false;
      {
        ThreadManager tm(ThreadManager::defaultBasePath());
        FleetState state = tm.getFleetState(threadId);
        pathsFree = true;
        for (const auto &path : input.paths) {
          for (const auto &lock : state.locks) {
            for (const auto &lp : lock.paths) {
              if (lp == path && lockStatusOrDefault(lock) == "open" && 
                  lock.ownerAgentId != ownerId && lock.ownerAgentId != input.target_agent_id) {
                pathsFree = false;
                break;
              }
            }
          }
        }
      }
      
      if (pathsFree) {
        rapidjson::Document doc;
        doc.SetObject();
        auto &alloc = doc.GetAllocator();
        doc.AddMember("request_id", rapidjson::Value(requestId.c_str(), alloc), alloc);
        doc.AddMember("mode", rapidjson::Value("request", alloc), alloc);
        doc.AddMember("status", rapidjson::Value("fulfilled", alloc), alloc);
        return shared::ToolResult::ok(doc);
      }
      
      const uint64_t now = worktools::nowEpochMs();
      if (now - start >= static_cast<uint64_t>(timeout)) {
        return shared::ToolResult::fail("Lock request timed out");
      }
      
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
  } else if (input.mode == "wait") {
    if (input.lock_id.empty()) {
      return shared::ToolResult::fail("wait mode requires 'lock_id'");
    }
    
    ThreadManager tm(ThreadManager::defaultBasePath());
    const uint64_t start = worktools::nowEpochMs();
    
    while (true) {
      bool lockReleased = false;
      FleetState state = tm.getFleetState(threadId);
      auto *lock = findLock(state, input.lock_id);
      
      if (!lock) {
        lockReleased = true;
      } else {
        const std::string status = lockStatusOrDefault(*lock);
        if (status != "open") {
          lockReleased = true;
        }
      }
      
      if (lockReleased) {
        rapidjson::Document doc;
        doc.SetObject();
        auto &alloc = doc.GetAllocator();
        doc.AddMember("lock_id", rapidjson::Value(input.lock_id.c_str(), alloc), alloc);
        doc.AddMember("mode", rapidjson::Value("wait", alloc), alloc);
        doc.AddMember("status", rapidjson::Value("released", alloc), alloc);
        return shared::ToolResult::ok(doc);
      }
      
      if (input.timeout_ms.has_value()) {
        const uint64_t now = worktools::nowEpochMs();
        if (now - start >= static_cast<uint64_t>(*input.timeout_ms)) {
          return shared::ToolResult::fail("Lock wait timed out");
        }
      }
      
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
  } else if (input.mode == "check") {
    ThreadManager tm(ThreadManager::defaultBasePath());
    FleetState state = tm.getFleetState(threadId);
    
    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    
    rapidjson::Value locks(rapidjson::kArrayType);
    rapidjson::Value conflictingPaths(rapidjson::kArrayType);
    
    for (const auto &lock : state.locks) {
      const std::string status = lockStatusOrDefault(lock);
      if (status != "open") continue;
      
      bool hasConflict = false;
      if (lock.paths.empty()) {
        hasConflict = !input.paths.empty();
      } else {
        for (const auto &requestedPath : input.paths) {
          for (const auto &lockPath : lock.paths) {
            if (requestedPath == lockPath) {
              hasConflict = true;
              break;
            }
          }
          if (hasConflict) break;
        }
      }
      
      if (hasConflict || input.paths.empty()) {
        rapidjson::Value lockObj(rapidjson::kObjectType);
        lockObj.AddMember("lock_id", rapidjson::Value(lock.lockId.c_str(), alloc), alloc);
        lockObj.AddMember("owner_agent_id", rapidjson::Value(lock.ownerAgentId.c_str(), alloc), alloc);
        lockObj.AddMember("reason", rapidjson::Value(lock.reason.c_str(), alloc), alloc);
        lockObj.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
        
        rapidjson::Value pathsArr(rapidjson::kArrayType);
        for (const auto &p : lock.paths) {
          pathsArr.PushBack(rapidjson::Value(p.c_str(), alloc), alloc);
        }
        lockObj.AddMember("paths", pathsArr, alloc);
        lockObj.AddMember("conflicts", hasConflict, alloc);
        locks.PushBack(lockObj, alloc);
        
        if (hasConflict) {
          for (const auto &lockPath : lock.paths) {
            bool alreadyAdded = false;
            for (const auto &added : conflictingPaths.GetArray()) {
              if (added.GetString() == lockPath) {
                alreadyAdded = true;
                break;
              }
            }
            if (!alreadyAdded) {
              conflictingPaths.PushBack(rapidjson::Value(lockPath.c_str(), alloc), alloc);
            }
          }
        }
      }
    }
    
    doc.AddMember("locks", locks, alloc);
    doc.AddMember("conflicting_paths", conflictingPaths, alloc);
    doc.AddMember("has_conflicts", !conflictingPaths.Empty(), alloc);
    
    return shared::ToolResult::ok(doc);
    
  } else {
    return shared::ToolResult::fail("Unknown mode: " + input.mode);
  }
}

} // namespace firmius::core
