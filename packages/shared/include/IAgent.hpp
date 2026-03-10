#ifndef FIRMIUS_SHARED_IAGENT_HPP
#define FIRMIUS_SHARED_IAGENT_HPP

#include "Context.hpp"
#include "Events.hpp"
#include "IHost.hpp"
#include <functional>
#include <map>
#include <string>

namespace firmius {
namespace core {
class AgentPermissionChecks;
}
} // namespace firmius

namespace firmius::shared {

/**
 * @brief Interface for the Agent Engine.
 */

class IAgent {
public:
  virtual ~IAgent() = default;

  /**
   * @brief Resets the agent's history and state.
   */
  virtual void reset() = 0;

  /**
   * @brief Runs the agent on a specific task.
   * @param task The task description.
   * @param onEvent Callback for real-time stream events.
   */
  virtual void run(const std::string &task,
                   std::function<void(const StreamEvent &)> onEvent) = 0;

  /**
   * @brief Gets the current agent context (read-only).
   * @return The agent context.
   */
  virtual const AgentContext &getContext() const = 0;

  /**
   * @brief Gets the current agent context (mutable).
   * @return The agent context.
   */
  virtual AgentContext &getMutableContext() = 0;

  virtual firmius::core::AgentPermissionChecks &getPermissionChecks() const = 0;

  /**
   * @brief Resolves a path relative to the agent's CWD.
   * @param path The path to resolve.
   * @return An absolute, normalized path within the sandbox.
   */
  virtual std::string resolvePath(const std::string &path) const = 0;

  /**
   * @brief Interrupts the current agent execution.
   */
  virtual void interrupt() = 0;

  /**
   * @brief Checks if the agent has been interrupted.
   * @return True if interrupt() has been called.
   */
  virtual bool isInterrupted() const = 0;

  /**
   * @brief Sets the provider and model for this agent.
   * @param providerId The provider ID.
   * @param modelId The model ID.
   */
  virtual void setModel(const std::string &providerId,
                        const std::string &modelId) = 0;

  /**
   * @brief Checks if the agent is currently running.
   * @return True if the agent is executing a task.
   */
  virtual bool isRunning() const = 0;
  virtual bool isBooting() const = 0;
  virtual void setBooting(bool b) = 0;

  /**
   * @brief Spawns a background process.
   * @return A unique process ID.
   */
  virtual std::string
  spawnProcess(const std::string &command, const std::string &toolCallId = "",
               const std::string &cwd = "",
               const std::map<std::string, std::string> &env = {}) = 0;

  /**
   * @brief Inspects a background process.
   * @param id The process ID.
   * @return A snapshot of the process state.
   */
  virtual ProcessSnapshot inspectProcess(const std::string &id) = 0;

  /**
   * @brief Writes data to a background process's stdin.
   * @param id The process ID.
   * @param data The data to write.
   */
  virtual void writeToProcess(const std::string &id,
                              const std::string &data) = 0;
  virtual void registerProcessId(const std::string &id) = 0;

  virtual void emitProcessSpawned(const std::string &processId,
                                  const std::string &toolCallId,
                                  const std::string &command) = 0;

  virtual void addBlockingProcessId(const std::string &id) = 0;
  virtual void removeBlockingProcessId(const std::string &id) = 0;
  virtual std::vector<std::string> getBlockingProcessIds() = 0;

  /**
   * @brief Checks if a file has been read in the current session.

   * @param path The absolute path to the file.
   * @return True if the file has been read.
   */
  virtual bool hasReadFile(const std::string &path) const = 0;

  /**
   * @brief Marks a file as having been read in the current session.
   * @param path The absolute path to the file.
   */
  virtual void markFileAsRead(const std::string &path) = 0;

  /**
   * @brief Returns the agent's host.
   */
  virtual std::shared_ptr<IHost> getHost() = 0;
};

} // namespace firmius::shared

#endif
