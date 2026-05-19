#ifndef FIRMIUS_SHARED_ITOOL_HPP
#define FIRMIUS_SHARED_ITOOL_HPP

#include "Context.hpp"
#include "IHost.hpp"
#include "utils/JSONSchema.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <rapidjson/document.h>
#include <string>
#include <thread>

/**
 * @brief Extensible tool system for agentic actions.
 */
namespace firmius::provider { class LLMSearchProviderRegistry; }

namespace firmius::shared {

class IAgent; // Forward declaration

/**
 * @brief Context passed to tools during execution.
 */
struct ToolContext {
  IHost &host;   ///< The execution host.
  IAgent &agent; ///< The agent instance invoking the tool.
  std::string
      currentToolCallId; ///< The ID of the tool call currently executing.
  const std::atomic<bool> *cancelSignal = nullptr; ///< Cancellation signal.
  firmius::provider::LLMSearchProviderRegistry *searchRegistry = nullptr; ///< Optional search provider registry.

  bool cancelRequested() const {
    return cancelSignal && cancelSignal->load();
  }

  bool waitFor(std::chrono::milliseconds total,
               std::chrono::milliseconds poll = std::chrono::milliseconds(10)) const {
    auto elapsed = std::chrono::milliseconds(0);
    while (elapsed < total) {
      if (cancelRequested()) {
        return false;
      }
      const auto remaining = total - elapsed;
      const auto sleepFor = remaining < poll ? remaining : poll;
      std::this_thread::sleep_for(sleepFor);
      elapsed += sleepFor;
    }
    return !cancelRequested();
  }
};

/**
 * @brief Metadata describing a tool.
 */
struct ToolMetadata {
  std::string name;        ///< Unique internal/public callable tool name.
  std::string description; ///< Human/LLM-readable description.
  ToolScope scope;         ///< Security scope required to run.
};

/**
 * @brief Result of a tool execution turn.
 */
struct ToolResult {
  bool success;      ///< True if tool succeeded.
  std::string error; ///< Error message (valid if success is false).
  std::string data;  ///< JSON string payload (valid if success is true).
  std::string processId;
  std::string subagentId;
  bool is_background = false;

  ToolResult() : success(false), data("{}") {}

  /**
   * @brief Creates a successful result with a JSON string payload.
   */
  static ToolResult ok(const std::string &jsonData,
                       const std::string &processId = "",
                       const std::string &subagentId = "") {
    ToolResult res;
    res.is_background = false;
    res.success = true;
    res.data = jsonData;
    res.processId = processId;
    res.subagentId = subagentId;
    return res;
  }

  /**
   * @brief Creates a successful result from a RapidJSON document (serializes to
   * string).
   */
  static ToolResult ok(const rapidjson::Document &doc,
                       const std::string &processId = "",
                       const std::string &subagentId = "");

  /**
   * @brief Creates a simple successful result with empty data.
   */
  static ToolResult ok() {
    ToolResult res;
    res.success = true;
    res.data = "{}";
    return res;
  }

  /**
   * @brief Creates a failed result with an error message.
   */
  static ToolResult fail(const std::string &msg) {
    ToolResult res;
    res.success = false;
    res.error = msg;
    res.data = "{}";
    return res;
  }

  bool operator==(const ToolResult &) const = default;
};

/**
 * @brief Interface for an executable agentic tool.
 */
class ITool {
public:
  virtual ~ITool() = default;

  /**
   * @brief Gets the tool's metadata.
   */
  virtual ToolMetadata getMetadata() const = 0;

  /**
   * @brief Gets the JSON Schema for tool input validation.
   */
  virtual std::shared_ptr<JSONSchema> getSchema() const = 0;

  /**
   * @brief Executes the tool with raw JSON input.
   * @param input The validated JSON arguments.
   * @param ctx Execution context.
   * @return Execution result.
   */
  virtual ToolResult execute(const rapidjson::Value &input,
                             ToolContext &ctx) = 0;
};

// Macros for TypedTool mapping
#define START_MAPPING(type)                                                    \
  type transform(const rapidjson::Value &json) override {                      \
    type input;                                                                \
    (void)json;
#define MAP_STRING(field, json_key)                                            \
  if (json.HasMember(json_key) && json[json_key].IsString())                   \
    input.field = json[json_key].GetString();                                  \
  else if (json.HasMember(json_key)) {                                         \
    if (json[json_key].IsInt())                                                \
      input.field = std::to_string(json[json_key].GetInt());                   \
    else if (json[json_key].IsBool())                                          \
      input.field = json[json_key].GetBool() ? "true" : "false";               \
  }
#define MAP_INT(field, json_key)                                               \
  if (json.HasMember(json_key)) {                                              \
    if (json[json_key].IsInt())                                                \
      input.field = json[json_key].GetInt();                                   \
    else if (json[json_key].IsString())                                        \
      try {                                                                    \
        input.field = std::stoi(json[json_key].GetString());                   \
      } catch (...) {                                                          \
        input.field = 0;                                                       \
      }                                                                        \
  }
#define MAP_FLOAT(field, json_key)                                             \
  if (json.HasMember(json_key)) {                                              \
    if (json[json_key].IsNumber())                                             \
      input.field = json[json_key].GetFloat();                                 \
    else if (json[json_key].IsString())                                        \
      try {                                                                    \
        input.field = std::stof(json[json_key].GetString());                   \
      } catch (...) {                                                          \
        input.field = 0.0f;                                                    \
      }                                                                        \
  }
#define MAP_BOOL(field, json_key)                                              \
  if (json.HasMember(json_key)) {                                              \
    if (json[json_key].IsBool())                                               \
      input.field = json[json_key].GetBool();                                  \
    else if (json[json_key].IsString()) {                                      \
      std::string s = json[json_key].GetString();                              \
      std::transform(s.begin(), s.end(), s.begin(),                            \
                     [](unsigned char c) { return std::tolower(c); });         \
      input.field = (s == "true" || s == "1" || s == "yes");                   \
    }                                                                          \
  }
#define END_MAPPING                                                            \
  return input;                                                                \
  }

/**
 * @brief Base class for tools with strongly-typed inputs.
 */
template <typename T> class TypedTool : public ITool {
public:
  /**
   * @brief Transforms raw JSON to the input type T.
   */
  virtual T transform(const rapidjson::Value &json) = 0;

  /**
   * @brief Executes the tool with typed input.
   */
  virtual ToolResult execute(const T &input, ToolContext &ctx) = 0;

  /**
   * @brief Implementation of ITool::execute that handles transformation.
   */
  ToolResult execute(const rapidjson::Value &json, ToolContext &ctx) override {
    T input = transform(json);
    return execute(input, ctx);
  }
};

} // namespace firmius::shared

#endif
