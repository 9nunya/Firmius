#ifndef FIRMIUS_SHARED_IAGENT_HPP
#define FIRMIUS_SHARED_IAGENT_HPP

#include "Context.hpp"
#include "Events.hpp"
#include "IEnvironment.hpp"
#include "IPermissions.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace firmius::shared {

/**
 * @brief Represents a choice of model and provider.
 */
struct ModelChoice {
  std::string providerId;
  std::string modelId;
  std::optional<std::string> variantName;
};

/**
 * @brief Interface for the Agent Engine.
 * 
 * Refactored to use composition: IAgent delegates environment operations
 * to IEnvironment and permission checks to IPermissions.
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
   * @param images Optional images to include with the task.
   */
  virtual void run(const std::string &task,
                   std::function<void(const StreamEvent &)> onEvent,
                   const std::vector<ImageContent> &images = {}) = 0;

  /**
   * @brief Resumes the agent from existing context without appending a user
   * task turn.
   */
  virtual void resume(std::function<void(const StreamEvent &)> onEvent) = 0;

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
   * @brief Clears the interrupt flag.
   */
  virtual void clearInterrupt() = 0;

  /**
   * @brief Compacts the agent context without resuming execution.
   */
  virtual void compactNow(
      std::function<void(const StreamEvent &)> onEvent) = 0;

  /**
   * @brief Sets the provider and model for this agent.
   * @param providerId The provider ID.
   * @param modelId The model ID.
   */
  virtual void setModel(const std::string &providerId,
                        const std::string &modelId) = 0;

  /**
   * @brief Sets the provider, model, and variant for this agent.
   * @param providerId The provider ID.
   * @param modelId The model ID.
   * @param variantName The model variant name (e.g., "low", "medium", "max").
   */
  virtual void setModel(const std::string &providerId, 
                        const std::string &modelId,
                        const std::string &variantName) = 0;

  /**
   * @brief Returns the preferred model choice for this agent based on purpose
   * or user configuration fallbacks.
   */
  virtual ModelChoice getPreferredModel() const = 0;

  /**
   * @brief Checks if the agent is currently running.
   * @return True if the agent is executing a task.
   */
  virtual bool isRunning() const = 0;

  /**
   * @brief Checks if the agent is booting.
   * @return True if booting.
   */
  virtual bool isBooting() const = 0;

  /**
   * @brief Sets the booting state.
   * @param b The booting state.
   */
  virtual void setBooting(bool b) = 0;

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

  /**
   * @brief Manually triggers a rewrite of the history journal to disk.
   */
  virtual void saveHistory() = 0;

  /**
   * @brief Returns the execution environment for this agent.
   * @return Shared pointer to the environment.
   */
  virtual std::shared_ptr<IEnvironment> getEnvironment() const = 0;

  /**
   * @brief Returns the permissions manager for this agent.
   * @return Shared pointer to the permissions.
   */
  virtual std::shared_ptr<IPermissions> getPermissions() const = 0;

  /**
   * @brief Returns the agent's host (convenience accessor).
   * @return Shared pointer to the host.
   */
  virtual std::shared_ptr<IHost> getHost() = 0;
};

} // namespace firmius::shared

#endif // FIRMIUS_SHARED_IAGENT_HPP
