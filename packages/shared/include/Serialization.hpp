#ifndef FIRMIUS_SHARED_SERIALIZATION_HPP
#define FIRMIUS_SHARED_SERIALIZATION_HPP

#include "Context.hpp"
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
