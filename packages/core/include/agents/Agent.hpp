#ifndef FIRMIUS_CORE_AGENT_HPP
#define FIRMIUS_CORE_AGENT_HPP

#include "Events.hpp"
#include "IAgent.hpp"
#include "IHost.hpp"
#include "IProvider.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "persistence/Journaler.hpp"
#include "tools/ToolRegistry.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief The primary Agent Engine implementation.
 */
class Agent : public IAgent {
public:
  Agent(AgentContext context, std::unique_ptr<shared::IHost> host,
        ToolRegistry &toolRegistry,
        std::shared_ptr<Journaler> journaler = nullptr);
  ~Agent() override;

  void reset() override;
  void run(const std::string &task,
           std::function<void(const StreamEvent &)> onEvent) override;

  void interrupt() override;
  bool isInterrupted() const override { return interrupted.load(); }
  void clearInterrupt() override;
  void setModel(const std::string &providerId,
                const std::string &modelId) override;
  void setModel(const std::string &providerId, const std::string &modelId,
                const std::string &variantName);
  bool isRunning() const override { return running.load(); }
  bool isBooting() const override { return booting.load(); }
  void setBooting(bool b) override { booting = b; }

  std::string
  spawnProcess(const std::string &command, const std::string &toolCallId = "",
               const std::string &cwd = "",
               const std::map<std::string, std::string> &env = {}) override;
  ProcessSnapshot inspectProcess(const std::string &id) override;
  void writeToProcess(const std::string &id, const std::string &data) override;
  void registerProcessId(const std::string &id) override;
  void emitProcessSpawned(const std::string &processId,
                          const std::string &toolCallId,
                          const std::string &command) override;

  void addBlockingProcessId(const std::string &id) override;
  void removeBlockingProcessId(const std::string &id) override;
  std::vector<std::string> getBlockingProcessIds() override;

  bool hasReadFile(const std::string &path) const override;
  void markFileAsRead(const std::string &path) override;
  bool hasFullyReadFile(const std::string &path) const override;
  void markFileAsFullyRead(const std::string &path) override;

  const AgentContext &getContext() const override { return context; }
  AgentContext &getMutableContext() override { return context; }
  std::string resolvePath(const std::string &inputPath) const override;
  std::shared_ptr<IHost> getHost() override { return host; }
  firmius::core::AgentPermissionChecks &getPermissionChecks() const override {
    return *permissionChecks;
  }
  void compactNow(
      std::function<void(const StreamEvent &)> onEvent) override;

private:
  void compactContext(std::function<void(const shared::StreamEvent &)> onEvent);
  void executeTools(const std::vector<ToolCallChunk> &chunks,
                    std::function<void(const shared::StreamEvent &)> onEvent);
  static std::uint64_t nowMs();

  AgentContext context;
  std::shared_ptr<firmius::provider::IProvider> provider;
  std::shared_ptr<shared::IHost> host;
  ToolRegistry &toolRegistry;
  std::shared_ptr<Journaler> journaler;
  std::atomic<bool> interrupted{false};
  std::atomic<bool> running{false};
  std::atomic<bool> booting{false};
  std::mutex runMutex;
  std::function<void(const shared::StreamEvent &)> eventCallback;
  std::mutex callbackMutex;
  std::mutex blockingProcessMutex; // protects blockingProcessIds

  std::unique_ptr<firmius::core::AgentPermissionChecks> permissionChecks;

  std::unordered_set<std::string> backgroundProcessIds;
};

} // namespace firmius::core

#endif
