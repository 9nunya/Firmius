#ifndef FIRMIUS_PROVIDER_IPROVIDER_HPP
#define FIRMIUS_PROVIDER_IPROVIDER_HPP

#include "Context.hpp"
#include "Enums.hpp"
#include "Events.hpp"
#include "utils/AbortController.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief LLM Provider abstraction layer.
 */
namespace firmius::provider {

using namespace firmius::shared;

enum class ProviderType : std::uint8_t { OAuth, APIKey };

/**
 * @brief Schema definition for an LLM-compatible tool.
 */
struct ToolDefinition {
  std::string name;        ///< Name of the tool.
  std::string description; ///< Description for the LLM.
  std::string inputSchema; ///< JSON Schema for the tool's input.
};

/**
 * @brief Configuration options for an LLM generation request.
 */
struct ProviderOptions {
  std::string modelId; ///< The ID of the model to use.
  std::string
      modelVariantJson;     ///< Optional JSON payload for the selected variant.
  float temperature = 0.7f; ///< Generation temperature.
  std::optional<std::uint32_t> maxTokens; ///< Optional maximum token limit.
  std::vector<std::string> stop;          ///< Optional list of stop sequences.
  std::vector<ToolDefinition>
      tools; ///< List of tools available for this request.
  std::atomic<bool> *abortSignal =
      nullptr; ///< Optional signal to abort the request immediately.
  std::shared_ptr<AbortController> abortController =
      nullptr; ///< Optional controller for immediate out-of-band aborts.
};

/**
 * @brief Interface for a specific LLM backend (e.g., OpenAI, Anthropic,
 * OpenRouter).
 */
  class IProvider {
  public:
    virtual ~IProvider() = default;

  /**
   * @brief Returns the unique identifier for this provider.
   */
  virtual std::string getId() const = 0;

  /**
   * @brief Streams a response from the LLM based on conversation history.
   * @param history The full chronological conversation history.
   * @param opts Generation and tool configuration.
   * @param onEvent Callback for real-time stream deltas.
   */
  virtual void stream(const AgentHistory &history, const ProviderOptions &opts,
                      std::function<void(const StreamEvent &)> onEvent) = 0;

  /**
   * @brief Discovers and lists available models from this provider.
    * @return A list of supported models.
    */
  virtual std::vector<ModelInfo> listModels() = 0;

  /**
   * @brief Returns the ModelInfo for a given model ID.
   */
  virtual ModelInfo getModelInfo(const std::string &modelId) = 0;

  /**
   * @brief Generates a synchronous summary of the conversation history.
   */
  virtual void generateSummary(const std::string &modelId,
                               const AgentHistory &history,
                               const std::string &compactionPrompt,
                               std::function<void(const StreamEvent &)> onEvent,
                               std::atomic<bool> *abortSignal = nullptr) = 0;

  /**
   * @brief Returns the current quotas/limits for this provider.
    * @return A map of quota names to values (e.g., "remainingFraction" as
    * string).
    */
    virtual std::map<std::string, std::string> getQuotas() const { return {}; }

  /**
   * @brief Returns the type of authentication this provider uses.
   */
  virtual ProviderType getProviderType() const = 0;

  /**
   * @brief Returns whether this provider is configured enough to be offered in
   * interactive model pickers.
   */
  virtual bool isConfigured() const { return true; }
  };

} // namespace firmius::provider

#endif
