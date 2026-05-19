#include "tools/FleetLockTool.hpp"
#include "AgentRegistry.hpp"
#include "harness/Harness.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/StringUtil.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

namespace firmius::core {

namespace {

static constexpr int kFleetLockPollIntervalMs = 50;
static constexpr int kFleetLockRespondPollIntervalMs = 100;

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
          "Fleet coordination operations: acquire, release, request, wait, or check file locks.",
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
  const std::string threadId = requireCurrentThreadId(ctx);
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
        const uint64_t start = nowEpochMs();
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
          
          const uint64_t now = nowEpochMs();
          if (now - start >= static_cast<uint64_t>(*input.timeout_ms)) {
            return shared::ToolResult::fail("Lock acquire timed out");
          }
          
          std::this_thread::sleep_for(std::chrono::milliseconds(kFleetLockPollIntervalMs));
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
    lock.createdAt = nowEpochMs();
    lock.updatedAt = lock.createdAt;

    tm.mutateFleetState(threadId, [&](FleetState &state) {
      state.locks.push_back(lock);
    });

    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    // Token-waste pass 5: prose-first acquire result. Dropped `mode` echo.
    std::ostringstream prose;
    prose << "Acquired lock " << lock.lockId << " over [";
    for (size_t i = 0; i < input.paths.size(); ++i) {
      if (i) prose << ", ";
      prose << input.paths[i];
    }
    prose << "].";
    const std::string proseStr = prose.str();
    doc.AddMember(
        "result",
        rapidjson::Value(proseStr.c_str(),
                         static_cast<rapidjson::SizeType>(proseStr.size()),
                         alloc).Move(),
        alloc);
    doc.AddMember("lock_id", rapidjson::Value(lock.lockId.c_str(), alloc), alloc);
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
      lock->updatedAt = nowEpochMs();
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
    // Token-waste pass 5: prose-first release result.
    std::string releaseProse = "Released lock " + input.lock_id + ".";
    doc.AddMember(
        "result",
        rapidjson::Value(releaseProse.c_str(),
                         static_cast<rapidjson::SizeType>(releaseProse.size()),
                         alloc).Move(),
        alloc);
    doc.AddMember("lock_id", rapidjson::Value(input.lock_id.c_str(), alloc), alloc);
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
    std::string requestMsg =
        "Fleet coordination request\n"
        "Request ID: " +
        requestId + "\n" +
        "Requester: " + ownerId + "\n" +
        "Reason: " + input.reason + "\n" +
        "Requested ownership hold for paths:\n";
    for (const auto &path : input.paths) {
      requestMsg += "- " + path + "\n";
    }
    requestMsg +=
        "\nIf you are still actively editing or stabilizing these surfaces, "
        "claim narrow ownership with `fleet_lock` and release it when your "
        "current edit/verification wave is done. If you are already done or "
        "the request is irrelevant, ignore it or finish cleanly so the "
        "requester can continue.";
    
    Harness::instance().queueInternalMessage(
        *input.target_agent_id, threadId, requestMsg);
    
    // Wait for response
    const uint64_t start = nowEpochMs();
    const int timeout = input.timeout_ms.value_or(120000);
    
    bool observedTargetOwnership = false;
    while (true) {
      bool targetExists = AgentRegistry::instance().getAgent(
                              *input.target_agent_id) != nullptr;
      bool targetOwnsSurface = false;
      bool conflictingOpenLock = false;
      {
        ThreadManager tm(ThreadManager::defaultBasePath());
        FleetState state = tm.getFleetState(threadId);
        for (const auto &lock : state.locks) {
          if (lockStatusOrDefault(lock) != "open") {
            continue;
          }
          bool overlaps = false;
          for (const auto &path : input.paths) {
            for (const auto &lp : lock.paths) {
              if (lp == path) {
                overlaps = true;
                break;
              }
            }
            if (overlaps) {
              break;
            }
          }
          if (!overlaps) {
            continue;
          }
          if (lock.ownerAgentId == *input.target_agent_id) {
            targetOwnsSurface = true;
          } else if (lock.ownerAgentId != ownerId) {
            conflictingOpenLock = true;
          }
        }
      }

      if (targetOwnsSurface) {
        observedTargetOwnership = true;
      }

      if (observedTargetOwnership && !targetOwnsSurface) {
        rapidjson::Document doc;
        doc.SetObject();
        auto &alloc = doc.GetAllocator();
        // Token-waste pass 5: prose-first lock-request resolution.
        std::string releasedProse =
            "Lock request " + requestId +
            " resolved: target released the surface.";
        doc.AddMember(
            "result",
            rapidjson::Value(releasedProse.c_str(),
                             static_cast<rapidjson::SizeType>(releasedProse.size()),
                             alloc).Move(),
            alloc);
        doc.AddMember("request_id",
                      rapidjson::Value(requestId.c_str(), alloc), alloc);
        doc.AddMember("status", rapidjson::Value("released", alloc), alloc);
        return shared::ToolResult::ok(doc);
      }

      if (!targetExists && !observedTargetOwnership) {
        rapidjson::Document doc;
        doc.SetObject();
        auto &alloc = doc.GetAllocator();
        std::string unavailProse =
            "Lock request " + requestId +
            " — target agent is unavailable.";
        if (conflictingOpenLock) {
          unavailProse +=
              " Conflicting open locks remain on the requested paths.";
        }
        doc.AddMember(
            "result",
            rapidjson::Value(unavailProse.c_str(),
                             static_cast<rapidjson::SizeType>(unavailProse.size()),
                             alloc).Move(),
            alloc);
        doc.AddMember("request_id",
                      rapidjson::Value(requestId.c_str(), alloc), alloc);
        doc.AddMember("status",
                      rapidjson::Value("target_unavailable", alloc), alloc);
        if (conflictingOpenLock) {
          doc.AddMember("conflicts_remaining", true, alloc);
        }
        return shared::ToolResult::ok(doc);
      }

      const uint64_t now = nowEpochMs();
      if (now - start >= static_cast<uint64_t>(timeout)) {
        return shared::ToolResult::fail("Lock request timed out");
      }
      
      std::this_thread::sleep_for(std::chrono::milliseconds(kFleetLockRespondPollIntervalMs));
    }
    
  } else if (input.mode == "wait") {
    if (input.lock_id.empty()) {
      return shared::ToolResult::fail("wait mode requires 'lock_id'");
    }
    
    ThreadManager tm(ThreadManager::defaultBasePath());
    const uint64_t start = nowEpochMs();
    
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
        // Token-waste pass 5: prose-first wait result.
        std::string waitProse = "Lock " + input.lock_id + " released.";
        doc.AddMember(
            "result",
            rapidjson::Value(waitProse.c_str(),
                             static_cast<rapidjson::SizeType>(waitProse.size()),
                             alloc).Move(),
            alloc);
        doc.AddMember("lock_id",
                      rapidjson::Value(input.lock_id.c_str(), alloc), alloc);
        doc.AddMember("status", rapidjson::Value("released", alloc), alloc);
        return shared::ToolResult::ok(doc);
      }
      
      if (input.timeout_ms.has_value()) {
        const uint64_t now = nowEpochMs();
        if (now - start >= static_cast<uint64_t>(*input.timeout_ms)) {
          return shared::ToolResult::fail("Lock wait timed out");
        }
      }
      
      std::this_thread::sleep_for(std::chrono::milliseconds(kFleetLockPollIntervalMs));
    }
    
  } else if (input.mode == "check") {
    ThreadManager tm(ThreadManager::defaultBasePath());
    FleetState state = tm.getFleetState(threadId);

    // Token-waste pass 5: prose-first check result. Each conflicting/open
    // lock becomes one prose line (id, owner, paths, reason); the
    // structured fields shrink to has_conflicts + conflicting_paths.
    struct CheckRow {
      std::string lockId;
      std::string ownerAgentId;
      std::string reason;
      std::vector<std::string> paths;
      bool conflicts = false;
    };
    std::vector<CheckRow> rows;
    std::vector<std::string> conflictingPaths;

    for (const auto &lock : state.locks) {
      const std::string status = lockStatusOrDefault(lock);
      if (status != "open") continue;

      bool hasConflict = false;
      if (lock.paths.empty()) {
        hasConflict = !input.paths.empty();
      } else {
        for (const auto &requestedPath : input.paths) {
          for (const auto &lockPath : lock.paths) {
            if (requestedPath == lockPath) { hasConflict = true; break; }
          }
          if (hasConflict) break;
        }
      }

      if (hasConflict || input.paths.empty()) {
        CheckRow row;
        row.lockId = lock.lockId;
        row.ownerAgentId = lock.ownerAgentId;
        row.reason = lock.reason;
        row.paths = lock.paths;
        row.conflicts = hasConflict;
        rows.push_back(std::move(row));

        if (hasConflict) {
          for (const auto &lockPath : lock.paths) {
            if (std::find(conflictingPaths.begin(), conflictingPaths.end(),
                          lockPath) == conflictingPaths.end()) {
              conflictingPaths.push_back(lockPath);
            }
          }
        }
      }
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    std::ostringstream prose;
    if (rows.empty()) {
      prose << "No matching locks.";
    } else {
      prose << rows.size() << " open lock"
            << (rows.size() == 1 ? "" : "s") << " match";
      if (!conflictingPaths.empty()) {
        prose << "; " << conflictingPaths.size()
              << " conflict path" << (conflictingPaths.size() == 1 ? "" : "s");
      }
      prose << ":\n";
      for (const auto &row : rows) {
        prose << "  " << row.lockId << " — owner=" << row.ownerAgentId;
        if (!row.reason.empty()) prose << ", reason=" << row.reason;
        if (!row.paths.empty()) {
          prose << ", paths=[";
          for (size_t i = 0; i < row.paths.size(); ++i) {
            if (i) prose << ", ";
            prose << row.paths[i];
          }
          prose << "]";
        }
        if (row.conflicts) prose << ", CONFLICTS";
        prose << "\n";
      }
    }
    const std::string proseStr = prose.str();
    doc.AddMember(
        "result",
        rapidjson::Value(proseStr.c_str(),
                         static_cast<rapidjson::SizeType>(proseStr.size()),
                         alloc).Move(),
        alloc);
    rapidjson::Value cpArr(rapidjson::kArrayType);
    for (const auto &p : conflictingPaths) {
      cpArr.PushBack(rapidjson::Value(p.c_str(), alloc), alloc);
    }
    doc.AddMember("conflicting_paths", cpArr, alloc);
    doc.AddMember("has_conflicts", !conflictingPaths.empty(), alloc);

    return shared::ToolResult::ok(doc);

  } else {
    return shared::ToolResult::fail("Unknown mode: " + input.mode);
  }
}

} // namespace firmius::core
