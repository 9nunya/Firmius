#ifndef FIRMIUS_CORE_AGENT_HPP
#define FIRMIUS_CORE_AGENT_HPP

#include "Events.hpp"
#include "IAgent.hpp"
#include "IHost.hpp"
#include "IProvider.hpp"
#include "IEnvironment.hpp"
#include "IPermissions.hpp"
#include "persistence/Journaler.hpp"
#include "tools/ToolRegistry.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "mcp/McpManager.hpp"
#include "agents/StreamSanityDetector.hpp"
namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief The primary Agent Engine implementation.
 */
class Agent : public IAgent, public std::enable_shared_from_this<Agent> {
public:
  Agent(AgentContext context, std::shared_ptr<shared::IEnvironment> environment,
        std::shared_ptr<shared::IPermissions> permissions,
        ToolRegistry &toolRegistry,
        std::shared_ptr<Journaler> journaler = nullptr);
  ~Agent() override;

  void reset() override;
  void run(const std::string &task,
           std::function<void(const StreamEvent &)> onEvent,
           const std::vector<ImageContent> &images = {}) override;
  void resume(std::function<void(const StreamEvent &)> onEvent);

  void interrupt() override;
  bool isInterrupted() const override { return interrupted.load(); }
  void clearInterrupt() override;
  void setModel(const std::string &providerId,
                const std::string &modelId) override;
  void setModel(const std::string &providerId, const std::string &modelId,
                const std::string &variantName) override;
  bool isRunning() const override { return running.load(); }
  bool isBooting() const override { return booting.load(); }
  void setBooting(bool b) override { booting = b; }
  void initializeMcpServers();

  const AgentContext &getContext() const override { return context; }
  AgentContext &getMutableContext() override { return context; }
  std::shared_ptr<shared::IHost> getHost() override;

  ModelChoice getPreferredModel() const override;
  
  std::shared_ptr<IEnvironment> getEnvironment() const override { return environment_; }
  std::shared_ptr<IPermissions> getPermissions() const override { return permissions_; }

  void compactNow(
      std::function<void(const StreamEvent &)> onEvent) override;
  void saveHistory() override;
  void appendHistoryTurn(const AgentTurn &turn) override;

  // Backward compatibility during transition
  std::string
  spawnProcess(const std::string &command, const std::string &toolCallId = "",
               const std::string &cwd = "",
               const std::map<std::string, std::string> &env = {},
               bool monitorCompletion = false);
  ProcessSnapshot inspectProcess(const std::string &id);
  void writeToProcess(const std::string &id, const std::string &data);
  void registerProcessId(const std::string &id);
  void emitProcessSpawned(const std::string &processId,
                          const std::string &toolCallId,
                          const std::string &command);

  void addBlockingProcessId(const std::string &id);
  void removeBlockingProcessId(const std::string &id);
  std::vector<std::string> getBlockingProcessIds();

  bool hasReadFile(const std::string &path) const;
  void markFileAsRead(const std::string &path);
  bool hasFullyReadFile(const std::string &path) const;
  void markFileAsFullyRead(const std::string &path);
  std::string resolvePath(const std::string &inputPath) const;

  mcp::McpManager &getMcpManager() { return mcpManager_; }
  std::shared_ptr<mcp::McpClient> getMcpClient(const std::string &serverName, shared::ToolContext &toolCtx);

private:
  struct PendingModelSwitch {
    std::string providerId;
    std::string modelId;
    std::optional<std::string> variantName;
  };

  void setModelInternal(const std::string &providerId,
                        const std::string &modelId,
                        const std::optional<std::string> &variantName);
  void applyPendingModelSwitchIfAny();

  void runImpl(const std::optional<std::string> &task,
               std::function<void(const StreamEvent &)> onEvent,
               const std::vector<ImageContent> &images);
  void compactContext(std::function<void(const shared::StreamEvent &)> onEvent);
  void executeTools(const std::vector<ToolCall> &calls,
                    std::function<void(const shared::StreamEvent &)> onEvent,
                    const std::shared_ptr<std::atomic<bool>> &runCancelToken);
  shared::AgentTurn makeInternalNudgeTurn(const std::string &turnIdPrefix,
                                          const std::string &text,
                                          shared::Role role = shared::Role::System) const;
  void appendTurnToHistory(const shared::AgentTurn &turn);
  static std::uint64_t nowMs();

  AgentContext context;
  std::shared_ptr<firmius::provider::IProvider> provider;
  std::shared_ptr<firmius::shared::IEnvironment> environment_;
  std::shared_ptr<firmius::shared::IPermissions> permissions_;
  ToolRegistry &toolRegistry;
  std::shared_ptr<Journaler> journaler;
  std::atomic<bool> interrupted{false};
  std::atomic<bool> running{false};
  std::atomic<bool> booting{false};
  std::mutex cancelTokenMutex_;
  std::shared_ptr<std::atomic<bool>> activeRunCancelToken_;
  std::shared_ptr<shared::AbortController> activeRunAbortController_;
  mutable std::mutex runStateMutex_;
  std::condition_variable runStateCv_;
  std::mutex modelSwitchMutex;
  std::optional<PendingModelSwitch> pendingModelSwitch_;
  std::mutex runMutex_;
  std::function<void(const shared::StreamEvent &)> eventCallback;
  std::mutex callbackMutex;
  std::mutex blockingProcessMutex;

  mcp::McpManager mcpManager_;
  std::unordered_set<std::string> backgroundProcessIds;
};


} // namespace firmius::core

#endif
