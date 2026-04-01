#ifndef FIRMIUS_CORE_WORK_TOOL_COMMON_HPP
#define FIRMIUS_CORE_WORK_TOOL_COMMON_HPP

#include "ITool.hpp"
#include "Serialization.hpp"
#include "persistence/ThreadManager.hpp"
#include <rapidjson/document.h>
#include <string>
#include <vector>

namespace firmius::core::worktools {

enum class WorkAgentRole { Lead, Executor, Auditor, Worker, Scout, Unknown };

ThreadManager makeThreadManager();
std::string requireCurrentThreadId(shared::ToolContext &ctx);
uint64_t nowEpochMs();
WorkAgentRole roleForContext(const shared::ToolContext &ctx);
std::string roleName(WorkAgentRole role);
std::string normalizeWorkRole(const std::string &role,
                              const std::string &fieldName,
                              bool allowEmpty = false);
std::string normalizePersonaRole(const std::string &persona,
                                 const std::string &fieldName);
bool hasScope(const shared::ToolContext &ctx, shared::ToolScope scope);

void requirePlanReadAccess(const shared::ToolContext &ctx);
void requirePlanWriteAccess(const shared::ToolContext &ctx);
void requireChunkReadAccess(const shared::ToolContext &ctx);
void requireChunkAddAccess(const shared::ToolContext &ctx);
void requireChunkUpdateAccess(const rapidjson::Value &input,
                              const shared::ToolContext &ctx,
                              const std::string &threadId,
                              const shared::Plan &plan,
                              const shared::WorkChunk &chunk);
void validateExecutorAssignmentInvariant(ThreadManager &tm,
                                         const std::string &threadId,
                                         const std::string &planId,
                                         const std::string &chunkId,
                                         const std::string &agentId);

std::string planStatusToString(shared::PlanStatus status);
shared::PlanStatus parsePlanStatus(const std::string &status);
std::string chunkStatusToString(shared::WorkChunkStatus status);
shared::WorkChunkStatus parseChunkStatus(const std::string &status);

std::vector<std::string> parseStringArray(const rapidjson::Value &input,
                                          const char *key);
std::vector<shared::WorkTask> parseTaskArray(const rapidjson::Value &input,
                                             const char *key);

shared::Plan loadPlan(ThreadManager &tm, const std::string &threadId,
                      const std::string &planId);
shared::WorkChunk &requireChunk(shared::Plan &plan, const std::string &chunkId);
const shared::WorkChunk &requireChunk(const shared::Plan &plan,
                                      const std::string &chunkId);
bool chunkDependenciesDone(const shared::Plan &plan,
                           const shared::WorkChunk &chunk);
bool blockChunkIfDependenciesIncomplete(const shared::Plan &plan,
                                        shared::WorkChunk &chunk);
bool unblockChunkIfDependenciesMet(const shared::Plan &plan,
                                   shared::WorkChunk &chunk);
bool chunkReadyForExecution(const shared::Plan &plan,
                            const shared::WorkChunk &chunk);
bool unblockDependentChunks(shared::Plan &plan,
                            const std::string &chunkId);
/// Reconcile all chunk dependencies in the plan - automatically unblock any chunks
/// whose dependencies are all Done. Returns true if any chunk was unblocked.
bool reconcileChunkDependencies(shared::Plan &plan);
void requireChunkReadyForExecution(const shared::Plan &plan,
                                   const shared::WorkChunk &chunk,
                                   const std::string &action = "dispatch");

rapidjson::Value makePlanSummary(const shared::Plan &plan, bool isActive,
                                 rapidjson::Document::AllocatorType &alloc);
rapidjson::Value makeChunkSummary(const shared::WorkChunk &chunk,
                                  rapidjson::Document::AllocatorType &alloc);

void emitWorkEvent(const shared::AppEvent &event);

// Fleet lock utilities for executor/worker coordination
struct FileConflictInfo {
  std::string filePath;
  std::string lockId;
  std::string ownerAgentId;
  std::string reason;
};

/// Check if any files in the given list have active locks held by other agents.
/// Returns a list of conflicts.
std::vector<FileConflictInfo> checkFileConflicts(
    const std::string &threadId,
    const std::vector<std::string> &filePaths,
    const std::string &requestingAgentId);

/// Acquire a lock for the given files. Returns lock ID on success, empty string on failure.
std::string acquireFileLock(
    const std::string &threadId,
    const std::vector<std::string> &filePaths,
    const std::string &reason,
    const std::string &ownerAgentId,
    int timeoutMs = -1);

/// Release a lock by ID. Returns true on success.
bool releaseFileLock(const std::string &threadId, const std::string &lockId);

/// Build lock doctrine text for executor prompts.
std::string buildExecutorLockDoctrine();

/// Build lock doctrine text for worker prompts.
std::string buildWorkerLockDoctrine();

} // namespace firmius::core::worktools

#endif
