#ifndef FIRMIUS_CORE_ROLLING_CONTEXT_MANAGER_HPP
#define FIRMIUS_CORE_ROLLING_CONTEXT_MANAGER_HPP

#include "Context.hpp"
#include "IProvider.hpp"

#include <atomic>
#include <functional>
#include <optional>
#include <string>

namespace firmius::core {

struct ResolvedRollingThresholds {
  bool enabled = false;
  std::string preset;
  float targetOccupancyRatio = 0.0f;
  float bufferOccupancyRatio = 0.0f;
  float emergencyOccupancyRatio = 0.0f;
  float reflectionOccupancyRatio = 0.0f;
  float retainTailRatio = 0.0f;
  std::uint32_t contextWindow = 0;
  std::uint32_t bufferThresholdTokens = 0;
  std::uint32_t targetThresholdTokens = 0;
  std::uint32_t emergencyThresholdTokens = 0;
  std::uint32_t reflectionThresholdTokens = 0;
  std::uint32_t retainedTailTokens = 0;
  std::uint32_t minimumChunkTokens = 0;
};

class RollingContextManager {
public:
  using PersistTurnFn = std::function<void(const shared::AgentTurn &)>;

  static bool isEnabled(const shared::AgentContext &context);
  static ResolvedRollingThresholds
  resolveThresholds(const shared::AgentContext &context);
  static void maintain(shared::AgentContext &context,
                       firmius::provider::IProvider &actorProvider,
                       PersistTurnFn persistTurn,
                       std::atomic<bool> *abortSignal = nullptr);
  static shared::AgentHistory
  filterHistoryForRequest(const shared::AgentContext &context,
                          const shared::AgentHistory &history);
  static std::string buildMemoryOverlay(const shared::AgentContext &context);
  static std::string buildStatusOverlay(const shared::AgentContext &context);
};

} // namespace firmius::core

#endif
