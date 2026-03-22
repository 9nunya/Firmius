#ifndef FIRMIUS_SHARED_SERIALIZATION_HPP
#define FIRMIUS_SHARED_SERIALIZATION_HPP

#include "Context.hpp"
#include "Enums.hpp"
#include "Message.hpp"
#include "Metrics.hpp"
#include "Events.hpp"

#include <rapidjson/document.h>
#include <string>

/**
 * @brief RapidJSON-based marshalling and unmarshalling logic.
 */
namespace firmius::shared {

/**
 * @brief Serializes an AgentContext to a RapidJSON document.
 */
rapidjson::Document toJson(const AgentContext& context);

/**
 * @brief Deserializes an AgentContext from a RapidJSON value.
 */
AgentContext fromJson(const rapidjson::Value& value);

/**
 * @brief Serializes a Message to a RapidJSON document.
 */
rapidjson::Document toJson(const Message& msg);

/**
 * @brief Deserializes a Message from a RapidJSON value.
 */
Message messageFromJsonValue(const rapidjson::Value& value);

/**
 * @brief Serializes an AgentTurn to a RapidJSON document.
 */
rapidjson::Document toJson(const AgentTurn& turn);

/**
 * @brief Deserializes an AgentTurn from a RapidJSON value.
 */
AgentTurn agentTurnFromJsonValue(const rapidjson::Value& value);

/**
 * @brief Serializes a StreamEvent to a RapidJSON document.
 */
rapidjson::Document toJson(const StreamEvent& event);

/**
 * @brief Deserializes a StreamEvent from a RapidJSON value.
 */
StreamEvent streamEventFromJsonValue(const rapidjson::Value& value);

/**
 * @brief Serializes a MessagePart to a RapidJSON document.
 */
rapidjson::Document toJson(const MessagePart& part);

/**
 * @brief Deserializes a MessagePart from a RapidJSON value.
 */
MessagePart messagePartFromJsonValue(const rapidjson::Value& value);

/**
 * @brief Serializes AgentMetrics to a RapidJSON document.
 */
rapidjson::Document toJson(const AgentMetrics& metrics);

/**
 * @brief Deserializes AgentMetrics from a RapidJSON value.
 */
AgentMetrics agentMetricsFromJsonValue(const rapidjson::Value& value);

// ModelInfo standalone serialization
rapidjson::Document toJson(const ModelInfo& model);
ModelInfo modelInfoFromJsonValue(const rapidjson::Value& v);

// AgentConfig standalone serialization
rapidjson::Document toJson(const AgentConfig& config);
AgentConfig agentConfigFromJsonValue(const rapidjson::Value& v);

// HostCreationOptions standalone serialization
rapidjson::Document toJson(const HostCreationOptions& options);
HostCreationOptions hostCreationOptionsFromJsonValue(const rapidjson::Value& v);

/**
 * @brief Serializes ThreadMetadata to a RapidJSON document.
 */
rapidjson::Document toJson(const ThreadMetadata& metadata);

/**
 * @brief Deserializes ThreadMetadata from a RapidJSON value.
 */
ThreadMetadata threadMetadataFromJson(const rapidjson::Value& value);

/**
 * @brief Serializes a WorkChunk to a RapidJSON document.
 */
rapidjson::Document toJson(const WorkChunk& chunk);

/**
 * @brief Deserializes a WorkChunk from a RapidJSON value.
 */
WorkChunk workChunkFromJson(const rapidjson::Value& value);

/**
 * @brief Serializes a WorkTask to a RapidJSON document.
 */
rapidjson::Document toJson(const WorkTask& task);

/**
 * @brief Deserializes a WorkTask from a RapidJSON value.
 */
WorkTask workTaskFromJson(const rapidjson::Value& value);

/**
 * @brief Serializes a Plan to a RapidJSON document.
 */
rapidjson::Document toJson(const Plan& plan);

/**
 * @brief Deserializes a Plan from a RapidJSON value.
 */
Plan planFromJson(const rapidjson::Value& value);

/**
 * @brief Serializes a TodoItem to a RapidJSON document.
 */
rapidjson::Document toJson(const TodoItem& item);

/**
 * @brief Deserializes a TodoItem from a RapidJSON value.
 */
TodoItem todoItemFromJson(const rapidjson::Value& value);

/**
 * @brief Serializes an AgentTodoList to a RapidJSON document.
 */
rapidjson::Document toJson(const AgentTodoList& list);

/**
 * @brief Deserializes an AgentTodoList from a RapidJSON value.
 */
AgentTodoList agentTodoListFromJson(const rapidjson::Value& value);

/**
 * @brief Serializes thread artifact metadata to a RapidJSON document.
 */
rapidjson::Document toJson(const ThreadArtifactMetadata& metadata);

/**
 * @brief Deserializes thread artifact metadata from a RapidJSON value.
 */
ThreadArtifactMetadata threadArtifactMetadataFromJson(const rapidjson::Value& value);

/**
 * @brief Serializes an EngineEvent to a RapidJSON document.
 */
rapidjson::Document toJson(const EngineEvent& event);

/**
 * @brief Deserializes an EngineEvent from a RapidJSON value.
 */
EngineEvent engineEventFromJson(const rapidjson::Value& value);

/**
 * @brief Convenience function to serialize context to a JSON string.
 */
std::string serializeToString(const AgentContext& context);

/**
 * @brief Convenience function to deserialize context from a JSON string.
 */
AgentContext deserializeFromString(const std::string& json);

}

#endif
