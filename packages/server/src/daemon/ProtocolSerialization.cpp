#include "daemon/ProtocolSerialization.hpp"

#include "Serialization.hpp"
#include "utils/PermissionProfiles.hpp"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>

namespace firmius::daemon {
namespace {

rapidjson::Value jsonString(const std::string &value,
                            rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out;
  out.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), allocator);
  return out;
}

std::string eventKindToString(DaemonEventKind kind) {
  switch (kind) {
  case DaemonEventKind::RuntimeAppEvent:
    return "runtime_app_event";
  case DaemonEventKind::ClientSessionRegistered:
    return "client_session_registered";
  case DaemonEventKind::ClientSessionDisconnected:
    return "client_session_disconnected";
  case DaemonEventKind::ClientSessionUpdated:
    return "client_session_updated";
  case DaemonEventKind::HookStateChanged:
    return "hook_state_changed";
  case DaemonEventKind::PactStateChanged:
    return "pact_state_changed";
  case DaemonEventKind::InitProgress:
    return "init_progress";
  }
  return "runtime_app_event";
}

DaemonEventKind eventKindFromString(const std::string &kind) {
  if (kind == "client_session_registered") {
    return DaemonEventKind::ClientSessionRegistered;
  }
  if (kind == "client_session_disconnected") {
    return DaemonEventKind::ClientSessionDisconnected;
  }
  if (kind == "client_session_updated") {
    return DaemonEventKind::ClientSessionUpdated;
  }
  if (kind == "hook_state_changed") {
    return DaemonEventKind::HookStateChanged;
  }
  if (kind == "pact_state_changed") {
    return DaemonEventKind::PactStateChanged;
  }
  if (kind == "init_progress") {
    return DaemonEventKind::InitProgress;
  }
  return DaemonEventKind::RuntimeAppEvent;
}

std::string agentStatusToWire(firmius::shared::AgentStatus status) {
  using S = firmius::shared::AgentStatus;
  switch (status) {
  case S::Idle: return "Idle";
  case S::Streaming: return "Streaming";
  case S::ExecutingTool: return "ExecutingTool";
  case S::AwaitingInput: return "AwaitingInput";
  case S::Compacting: return "Compacting";
  case S::ProviderWaiting: return "ProviderWaiting";
  case S::Error: return "Error";
  case S::Cancelled: return "Cancelled";
  }
  return "Idle";
}

std::optional<firmius::shared::AgentStatus> agentStatusFromWire(const std::string &str) {
  using S = firmius::shared::AgentStatus;
  if (str == "Idle") return S::Idle;
  if (str == "Streaming") return S::Streaming;
  if (str == "ExecutingTool") return S::ExecutingTool;
  if (str == "AwaitingInput") return S::AwaitingInput;
  if (str == "Compacting") return S::Compacting;
  if (str == "ProviderWaiting") return S::ProviderWaiting;
  if (str == "Error") return S::Error;
  if (str == "Cancelled") return S::Cancelled;
  return std::nullopt;
}

rapidjson::Value messagePartValue(const firmius::shared::ImageContent &image,
                                  rapidjson::Document::AllocatorType &allocator) {
  firmius::shared::MessagePart part = image;
  auto doc = firmius::shared::toJson(part);
  rapidjson::Value out(rapidjson::kObjectType);
  out.CopyFrom(doc, allocator);
  return out;
}

firmius::shared::ImageContent imageContentFromValue(const rapidjson::Value &value) {
  auto part = firmius::shared::messagePartFromJsonValue(value);
  if (auto image = std::get_if<firmius::shared::ImageContent>(&part)) {
    return *image;
  }
  throw std::runtime_error("JSON value is not ImageContent");
}

rapidjson::Value toJsonValue(
    const firmius::shared::AgentConfig::RollingModelConfig &model,
    rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("enabled", model.enabled, allocator);
  out.AddMember("provider_id", jsonString(model.providerId, allocator), allocator);
  out.AddMember("model_id", jsonString(model.modelId, allocator), allocator);
  out.AddMember("variant_name", jsonString(model.variantName, allocator), allocator);
  return out;
}

firmius::shared::AgentConfig::RollingModelConfig
rollingModelConfigFromJson(const rapidjson::Value &value) {
  firmius::shared::AgentConfig::RollingModelConfig model;
  if (!value.IsObject()) {
    return model;
  }
  if (value.HasMember("enabled") && value["enabled"].IsBool()) {
    model.enabled = value["enabled"].GetBool();
  }
  if (value.HasMember("provider_id") && value["provider_id"].IsString()) {
    model.providerId = value["provider_id"].GetString();
  }
  if (value.HasMember("model_id") && value["model_id"].IsString()) {
    model.modelId = value["model_id"].GetString();
  }
  if (value.HasMember("variant_name") && value["variant_name"].IsString()) {
    model.variantName = value["variant_name"].GetString();
  }
  return model;
}

rapidjson::Value toJsonValue(
    const firmius::shared::UserConfig::RollingMemoryConfig &config,
    rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("enabled", config.enabled, allocator);
  out.AddMember("mode", jsonString(config.mode, allocator), allocator);
  out.AddMember("preset", jsonString(config.preset, allocator), allocator);
  out.AddMember("target_occupancy_ratio", config.targetOccupancyRatio, allocator);
  out.AddMember("buffer_occupancy_ratio", config.bufferOccupancyRatio, allocator);
  out.AddMember("emergency_occupancy_ratio", config.emergencyOccupancyRatio,
                allocator);
  out.AddMember("reflection_occupancy_ratio", config.reflectionOccupancyRatio,
                allocator);
  out.AddMember("retain_tail_ratio", config.retainTailRatio, allocator);
  out.AddMember("minimum_retained_tail_tokens",
                config.minimumRetainedTailTokens, allocator);
  out.AddMember("minimum_chunk_tokens", config.minimumChunkTokens, allocator);
  out.AddMember("emit_event_turns", config.emitEventTurns, allocator);
  out.AddMember("observer", toJsonValue(config.observer, allocator), allocator);
  out.AddMember("reflector", toJsonValue(config.reflector, allocator), allocator);
  out.AddMember("working_memory_updater",
                toJsonValue(config.workingMemoryUpdater, allocator), allocator);
  return out;
}

firmius::shared::UserConfig::RollingMemoryConfig
rollingMemoryConfigFromJson(const rapidjson::Value &value) {
  firmius::shared::UserConfig::RollingMemoryConfig config;
  if (!value.IsObject()) {
    return config;
  }
  if (value.HasMember("enabled") && value["enabled"].IsBool()) {
    config.enabled = value["enabled"].GetBool();
  }
  if (value.HasMember("mode") && value["mode"].IsString()) {
    config.mode = value["mode"].GetString();
  }
  if (value.HasMember("preset") && value["preset"].IsString()) {
    config.preset = value["preset"].GetString();
  }
  if (value.HasMember("target_occupancy_ratio") &&
      value["target_occupancy_ratio"].IsNumber()) {
    config.targetOccupancyRatio = value["target_occupancy_ratio"].GetFloat();
  }
  if (value.HasMember("buffer_occupancy_ratio") &&
      value["buffer_occupancy_ratio"].IsNumber()) {
    config.bufferOccupancyRatio = value["buffer_occupancy_ratio"].GetFloat();
  }
  if (value.HasMember("emergency_occupancy_ratio") &&
      value["emergency_occupancy_ratio"].IsNumber()) {
    config.emergencyOccupancyRatio =
        value["emergency_occupancy_ratio"].GetFloat();
  }
  if (value.HasMember("reflection_occupancy_ratio") &&
      value["reflection_occupancy_ratio"].IsNumber()) {
    config.reflectionOccupancyRatio =
        value["reflection_occupancy_ratio"].GetFloat();
  }
  if (value.HasMember("retain_tail_ratio") &&
      value["retain_tail_ratio"].IsNumber()) {
    config.retainTailRatio = value["retain_tail_ratio"].GetFloat();
  }
  if (value.HasMember("minimum_retained_tail_tokens") &&
      value["minimum_retained_tail_tokens"].IsUint()) {
    config.minimumRetainedTailTokens =
        value["minimum_retained_tail_tokens"].GetUint();
  }
  if (value.HasMember("minimum_chunk_tokens") &&
      value["minimum_chunk_tokens"].IsUint()) {
    config.minimumChunkTokens = value["minimum_chunk_tokens"].GetUint();
  }
  if (value.HasMember("emit_event_turns") &&
      value["emit_event_turns"].IsBool()) {
    config.emitEventTurns = value["emit_event_turns"].GetBool();
  }
  if (value.HasMember("observer")) {
    config.observer = rollingModelConfigFromJson(value["observer"]);
  }
  if (value.HasMember("reflector")) {
    config.reflector = rollingModelConfigFromJson(value["reflector"]);
  }
  if (value.HasMember("working_memory_updater")) {
    config.workingMemoryUpdater =
        rollingModelConfigFromJson(value["working_memory_updater"]);
  }
  return config;
}

rapidjson::Value toJsonValue(const firmius::shared::ModelRouteCategory &category,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value models(rapidjson::kArrayType);
  for (const auto &option : category.models) {
    rapidjson::Value item(rapidjson::kObjectType);
    item.AddMember("provider_id", jsonString(option.providerId, allocator),
                   allocator);
    item.AddMember("model_id", jsonString(option.modelId, allocator), allocator);
    item.AddMember("variant_name", jsonString(option.variantName, allocator),
                   allocator);
    models.PushBack(item, allocator);
  }
  out.AddMember("models", models, allocator);
  return out;
}

firmius::shared::ModelRouteCategory
modelRouteCategoryFromJson(const rapidjson::Value &value) {
  firmius::shared::ModelRouteCategory category;
  if (!value.IsObject() || !value.HasMember("models") ||
      !value["models"].IsArray()) {
    return category;
  }
  for (const auto &item : value["models"].GetArray()) {
    if (!item.IsObject()) {
      continue;
    }
    firmius::shared::ModelOption option;
    if (item.HasMember("provider_id") && item["provider_id"].IsString()) {
      option.providerId = item["provider_id"].GetString();
    }
    if (item.HasMember("model_id") && item["model_id"].IsString()) {
      option.modelId = item["model_id"].GetString();
    }
    if (item.HasMember("variant_name") && item["variant_name"].IsString()) {
      option.variantName = item["variant_name"].GetString();
    }
    category.models.push_back(std::move(option));
  }
  return category;
}

rapidjson::Value toJsonValue(const firmius::shared::McpServerConfig &config,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("transport", jsonString(config.transport, allocator), allocator);
  out.AddMember("enabled", config.enabled, allocator);
  out.AddMember("command", jsonString(config.command, allocator), allocator);
  rapidjson::Value args(rapidjson::kArrayType);
  for (const auto &arg : config.args) {
    args.PushBack(jsonString(arg, allocator), allocator);
  }
  out.AddMember("args", args, allocator);
  rapidjson::Value env(rapidjson::kObjectType);
  for (const auto &[key, val] : config.env) {
    env.AddMember(jsonString(key, allocator), jsonString(val, allocator),
                  allocator);
  }
  out.AddMember("env", env, allocator);
  out.AddMember("cwd", jsonString(config.cwd, allocator), allocator);
  out.AddMember("url", jsonString(config.url, allocator), allocator);
  out.AddMember("auth_header", jsonString(config.authHeader, allocator),
                allocator);
  out.AddMember("auth_bearer_token",
                jsonString(config.authBearerToken, allocator), allocator);
  out.AddMember("allow_insecure_tls", config.allowInsecureTls, allocator);
  out.AddMember("ca_cert_path", jsonString(config.caCertPath, allocator),
                allocator);
  return out;
}

firmius::shared::McpServerConfig
mcpServerConfigFromJson(const rapidjson::Value &value) {
  firmius::shared::McpServerConfig config;
  if (!value.IsObject()) {
    return config;
  }
  if (value.HasMember("transport") && value["transport"].IsString()) {
    config.transport = value["transport"].GetString();
  }
  if (value.HasMember("enabled") && value["enabled"].IsBool()) {
    config.enabled = value["enabled"].GetBool();
  }
  if (value.HasMember("command") && value["command"].IsString()) {
    config.command = value["command"].GetString();
  }
  if (value.HasMember("args") && value["args"].IsArray()) {
    config.args.clear();
    for (const auto &arg : value["args"].GetArray()) {
      if (arg.IsString()) {
        config.args.push_back(arg.GetString());
      }
    }
  }
  if (value.HasMember("env") && value["env"].IsObject()) {
    config.env.clear();
    for (auto it = value["env"].MemberBegin(); it != value["env"].MemberEnd();
         ++it) {
      if (it->value.IsString()) {
        config.env[it->name.GetString()] = it->value.GetString();
      }
    }
  }
  if (value.HasMember("cwd") && value["cwd"].IsString()) {
    config.cwd = value["cwd"].GetString();
  }
  if (value.HasMember("url") && value["url"].IsString()) {
    config.url = value["url"].GetString();
  }
  if (value.HasMember("auth_header") && value["auth_header"].IsString()) {
    config.authHeader = value["auth_header"].GetString();
  }
  if (value.HasMember("auth_bearer_token") &&
      value["auth_bearer_token"].IsString()) {
    config.authBearerToken = value["auth_bearer_token"].GetString();
  }
  if (value.HasMember("allow_insecure_tls") &&
      value["allow_insecure_tls"].IsBool()) {
    config.allowInsecureTls = value["allow_insecure_tls"].GetBool();
  }
  if (value.HasMember("ca_cert_path") && value["ca_cert_path"].IsString()) {
    config.caCertPath = value["ca_cert_path"].GetString();
  }
  return config;
}

rapidjson::Value toJsonValue(
    const firmius::shared::ProviderDefaultsConfig &config,
    rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("temperature", config.temperature, allocator);
  if (config.maxTokens.has_value()) {
    out.AddMember("max_tokens", config.maxTokens.value(), allocator);
  } else {
    out.AddMember("max_tokens", rapidjson::Value(rapidjson::kNullType),
                  allocator);
  }
  out.AddMember("stream_usage", config.streamUsage, allocator);
  return out;
}

firmius::shared::ProviderDefaultsConfig
providerDefaultsConfigFromJson(const rapidjson::Value &value) {
  firmius::shared::ProviderDefaultsConfig config;
  if (!value.IsObject()) {
    return config;
  }
  if (value.HasMember("temperature") && value["temperature"].IsNumber()) {
    config.temperature = value["temperature"].GetFloat();
  }
  if (value.HasMember("max_tokens")) {
    if (value["max_tokens"].IsUint()) {
      config.maxTokens = value["max_tokens"].GetUint();
    } else if (value["max_tokens"].IsNull()) {
      config.maxTokens = std::nullopt;
    }
  }
  if (value.HasMember("stream_usage") && value["stream_usage"].IsBool()) {
    config.streamUsage = value["stream_usage"].GetBool();
  }
  return config;
}

rapidjson::Value toJsonValue(const firmius::shared::RetryPolicyConfig &config,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("max_retries", config.maxRetries, allocator);
  out.AddMember("base_delay_ms", config.baseDelayMs, allocator);
  out.AddMember("max_delay_ms", config.maxDelayMs, allocator);
  out.AddMember("jitter_min", config.jitterMin, allocator);
  out.AddMember("jitter_max", config.jitterMax, allocator);
  out.AddMember("use_shared_backoff_sequence", config.useSharedBackoffSequence,
                allocator);
  out.AddMember("respect_retry_after", config.respectRetryAfter, allocator);
  out.AddMember("timeout_seconds", config.timeoutSeconds, allocator);
  out.AddMember("connect_timeout_seconds", config.connectTimeoutSeconds,
                allocator);
  rapidjson::Value retryStatuses(rapidjson::kArrayType);
  for (int code : config.retryHttpStatuses) {
    retryStatuses.PushBack(code, allocator);
  }
  out.AddMember("retry_http_statuses", retryStatuses, allocator);
  rapidjson::Value nonRetryStatuses(rapidjson::kArrayType);
  for (int code : config.nonRetryHttpStatuses) {
    nonRetryStatuses.PushBack(code, allocator);
  }
  out.AddMember("non_retry_http_statuses", nonRetryStatuses, allocator);
  rapidjson::Value retryCurlErrors(rapidjson::kArrayType);
  for (const auto &code : config.retryCurlErrors) {
    retryCurlErrors.PushBack(jsonString(code, allocator), allocator);
  }
  out.AddMember("retry_curl_errors", retryCurlErrors, allocator);
  return out;
}

firmius::shared::RetryPolicyConfig
retryPolicyConfigFromJson(const rapidjson::Value &value) {
  firmius::shared::RetryPolicyConfig config;
  if (!value.IsObject()) {
    return config;
  }
  if (value.HasMember("max_retries") && value["max_retries"].IsInt()) {
    config.maxRetries = value["max_retries"].GetInt();
  }
  if (value.HasMember("base_delay_ms") && value["base_delay_ms"].IsInt()) {
    config.baseDelayMs = value["base_delay_ms"].GetInt();
  }
  if (value.HasMember("max_delay_ms") && value["max_delay_ms"].IsInt()) {
    config.maxDelayMs = value["max_delay_ms"].GetInt();
  }
  if (value.HasMember("jitter_min") && value["jitter_min"].IsNumber()) {
    config.jitterMin = value["jitter_min"].GetDouble();
  }
  if (value.HasMember("jitter_max") && value["jitter_max"].IsNumber()) {
    config.jitterMax = value["jitter_max"].GetDouble();
  }
  if (value.HasMember("use_shared_backoff_sequence") &&
      value["use_shared_backoff_sequence"].IsBool()) {
    config.useSharedBackoffSequence =
        value["use_shared_backoff_sequence"].GetBool();
  }
  if (value.HasMember("respect_retry_after") &&
      value["respect_retry_after"].IsBool()) {
    config.respectRetryAfter = value["respect_retry_after"].GetBool();
  }
  if (value.HasMember("timeout_seconds") &&
      value["timeout_seconds"].IsInt()) {
    config.timeoutSeconds = value["timeout_seconds"].GetInt();
  }
  if (value.HasMember("connect_timeout_seconds") &&
      value["connect_timeout_seconds"].IsInt()) {
    config.connectTimeoutSeconds = value["connect_timeout_seconds"].GetInt();
  }
  if (value.HasMember("retry_http_statuses") &&
      value["retry_http_statuses"].IsArray()) {
    config.retryHttpStatuses.clear();
    for (const auto &item : value["retry_http_statuses"].GetArray()) {
      if (item.IsInt()) {
        config.retryHttpStatuses.push_back(item.GetInt());
      }
    }
  }
  if (value.HasMember("non_retry_http_statuses") &&
      value["non_retry_http_statuses"].IsArray()) {
    config.nonRetryHttpStatuses.clear();
    for (const auto &item : value["non_retry_http_statuses"].GetArray()) {
      if (item.IsInt()) {
        config.nonRetryHttpStatuses.push_back(item.GetInt());
      }
    }
  }
  if (value.HasMember("retry_curl_errors") &&
      value["retry_curl_errors"].IsArray()) {
    config.retryCurlErrors.clear();
    for (const auto &item : value["retry_curl_errors"].GetArray()) {
      if (item.IsString()) {
        config.retryCurlErrors.push_back(item.GetString());
      }
    }
  }
  return config;
}

rapidjson::Value toJsonValue(
    const firmius::shared::ProviderVariantConfig &config,
    rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("label", jsonString(config.label, allocator), allocator);
  out.AddMember("request_json", jsonString(config.requestJson, allocator),
                allocator);
  out.AddMember("description", jsonString(config.description, allocator),
                allocator);
  return out;
}

firmius::shared::ProviderVariantConfig
providerVariantConfigFromJson(const rapidjson::Value &value) {
  firmius::shared::ProviderVariantConfig config;
  if (!value.IsObject()) {
    return config;
  }
  if (value.HasMember("label") && value["label"].IsString()) {
    config.label = value["label"].GetString();
  }
  if (value.HasMember("request_json") && value["request_json"].IsString()) {
    config.requestJson = value["request_json"].GetString();
  }
  if (value.HasMember("description") && value["description"].IsString()) {
    config.description = value["description"].GetString();
  }
  return config;
}

rapidjson::Value toJsonValue(
    const firmius::shared::ProviderModelConfig &config,
    rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("default_variant", jsonString(config.defaultVariant, allocator),
                allocator);
  rapidjson::Value variants(rapidjson::kObjectType);
  for (const auto &[name, variant] : config.variants) {
    variants.AddMember(jsonString(name, allocator),
                       toJsonValue(variant, allocator), allocator);
  }
  out.AddMember("variants", variants, allocator);
  out.AddMember("override_context_window", config.overrideContextWindow,
                allocator);
  out.AddMember("context_window", config.contextWindow, allocator);
  out.AddMember("override_max_output_tokens", config.overrideMaxOutputTokens,
                allocator);
  out.AddMember("max_output_tokens", config.maxOutputTokens, allocator);
  out.AddMember("override_modalities", config.overrideModalities, allocator);
  rapidjson::Value modalities(rapidjson::kArrayType);
  for (const auto &modality : config.modalities) {
    modalities.PushBack(jsonString(modality, allocator), allocator);
  }
  out.AddMember("modalities", modalities, allocator);
  out.AddMember("override_supports_reasoning",
                config.overrideSupportsReasoning, allocator);
  out.AddMember("supports_reasoning", config.supportsReasoning, allocator);
  return out;
}

firmius::shared::ProviderModelConfig
providerModelConfigFromJson(const rapidjson::Value &value) {
  firmius::shared::ProviderModelConfig config;
  if (!value.IsObject()) {
    return config;
  }
  if (value.HasMember("default_variant") &&
      value["default_variant"].IsString()) {
    config.defaultVariant = value["default_variant"].GetString();
  }
  if (value.HasMember("variants") && value["variants"].IsObject()) {
    config.variants.clear();
    for (auto it = value["variants"].MemberBegin();
         it != value["variants"].MemberEnd(); ++it) {
      config.variants[it->name.GetString()] =
          providerVariantConfigFromJson(it->value);
    }
  }
  if (value.HasMember("override_context_window") &&
      value["override_context_window"].IsBool()) {
    config.overrideContextWindow = value["override_context_window"].GetBool();
  }
  if (value.HasMember("context_window") && value["context_window"].IsUint()) {
    config.contextWindow = value["context_window"].GetUint();
  }
  if (value.HasMember("override_max_output_tokens") &&
      value["override_max_output_tokens"].IsBool()) {
    config.overrideMaxOutputTokens =
        value["override_max_output_tokens"].GetBool();
  }
  if (value.HasMember("max_output_tokens") &&
      value["max_output_tokens"].IsUint()) {
    config.maxOutputTokens = value["max_output_tokens"].GetUint();
  }
  if (value.HasMember("override_modalities") &&
      value["override_modalities"].IsBool()) {
    config.overrideModalities = value["override_modalities"].GetBool();
  }
  if (value.HasMember("modalities") && value["modalities"].IsArray()) {
    config.modalities.clear();
    for (const auto &item : value["modalities"].GetArray()) {
      if (item.IsString()) {
        config.modalities.push_back(item.GetString());
      }
    }
  }
  if (value.HasMember("override_supports_reasoning") &&
      value["override_supports_reasoning"].IsBool()) {
    config.overrideSupportsReasoning =
        value["override_supports_reasoning"].GetBool();
  }
  if (value.HasMember("supports_reasoning") &&
      value["supports_reasoning"].IsBool()) {
    config.supportsReasoning = value["supports_reasoning"].GetBool();
  }
  return config;
}

rapidjson::Value toJsonValue(
    const firmius::shared::ProviderProfileConfig &config,
    rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("auth_mode", jsonString(config.authMode, allocator), allocator);
  out.AddMember("kind", jsonString(config.kind, allocator), allocator);
  out.AddMember("display_name", jsonString(config.displayName, allocator),
                allocator);
  out.AddMember("enabled", config.enabled, allocator);
  out.AddMember("base_url", jsonString(config.baseUrl, allocator), allocator);
  out.AddMember("models_endpoint", jsonString(config.modelsEndpoint, allocator),
                allocator);
  out.AddMember("chat_endpoint", jsonString(config.chatEndpoint, allocator),
                allocator);
  out.AddMember("messages_endpoint",
                jsonString(config.messagesEndpoint, allocator), allocator);
  out.AddMember("api_key_ref", jsonString(config.apiKeyRef, allocator),
                allocator);
  out.AddMember("default_api_key", jsonString(config.defaultApiKey, allocator),
                allocator);
  out.AddMember("allow_missing_api_key", config.allowMissingApiKey, allocator);
  rapidjson::Value headers(rapidjson::kObjectType);
  for (const auto &[key, val] : config.headers) {
    headers.AddMember(jsonString(key, allocator), jsonString(val, allocator),
                      allocator);
  }
  out.AddMember("headers", headers, allocator);
  out.AddMember("defaults", toJsonValue(config.defaults, allocator), allocator);
  out.AddMember("reasoning_field_name",
                jsonString(config.reasoningFieldName, allocator), allocator);
  out.AddMember("anthropic_version",
                jsonString(config.anthropicVersion, allocator), allocator);
  out.AddMember("beta_header", jsonString(config.betaHeader, allocator),
                allocator);
  out.AddMember("retry", toJsonValue(config.retry, allocator), allocator);
  rapidjson::Value models(rapidjson::kObjectType);
  for (const auto &[modelId, model] : config.modelVariants) {
    models.AddMember(jsonString(modelId, allocator),
                     toJsonValue(model, allocator), allocator);
  }
  out.AddMember("model_variants", models, allocator);
  return out;
}

firmius::shared::ProviderProfileConfig
providerProfileConfigFromJson(const rapidjson::Value &value) {
  firmius::shared::ProviderProfileConfig config;
  if (!value.IsObject()) {
    return config;
  }
  if (value.HasMember("auth_mode") && value["auth_mode"].IsString()) {
    config.authMode = value["auth_mode"].GetString();
  }
  if (value.HasMember("kind") && value["kind"].IsString()) {
    config.kind = value["kind"].GetString();
  }
  if (value.HasMember("display_name") && value["display_name"].IsString()) {
    config.displayName = value["display_name"].GetString();
  }
  if (value.HasMember("enabled") && value["enabled"].IsBool()) {
    config.enabled = value["enabled"].GetBool();
  }
  if (value.HasMember("base_url") && value["base_url"].IsString()) {
    config.baseUrl = value["base_url"].GetString();
  }
  if (value.HasMember("models_endpoint") &&
      value["models_endpoint"].IsString()) {
    config.modelsEndpoint = value["models_endpoint"].GetString();
  }
  if (value.HasMember("chat_endpoint") && value["chat_endpoint"].IsString()) {
    config.chatEndpoint = value["chat_endpoint"].GetString();
  }
  if (value.HasMember("messages_endpoint") &&
      value["messages_endpoint"].IsString()) {
    config.messagesEndpoint = value["messages_endpoint"].GetString();
  }
  if (value.HasMember("api_key_ref") && value["api_key_ref"].IsString()) {
    config.apiKeyRef = value["api_key_ref"].GetString();
  }
  if (value.HasMember("default_api_key") &&
      value["default_api_key"].IsString()) {
    config.defaultApiKey = value["default_api_key"].GetString();
  }
  if (value.HasMember("allow_missing_api_key") &&
      value["allow_missing_api_key"].IsBool()) {
    config.allowMissingApiKey = value["allow_missing_api_key"].GetBool();
  }
  if (value.HasMember("headers") && value["headers"].IsObject()) {
    config.headers.clear();
    for (auto it = value["headers"].MemberBegin();
         it != value["headers"].MemberEnd(); ++it) {
      if (it->value.IsString()) {
        config.headers[it->name.GetString()] = it->value.GetString();
      }
    }
  }
  if (value.HasMember("defaults")) {
    config.defaults = providerDefaultsConfigFromJson(value["defaults"]);
  }
  if (value.HasMember("reasoning_field_name") &&
      value["reasoning_field_name"].IsString()) {
    config.reasoningFieldName = value["reasoning_field_name"].GetString();
  }
  if (value.HasMember("anthropic_version") &&
      value["anthropic_version"].IsString()) {
    config.anthropicVersion = value["anthropic_version"].GetString();
  }
  if (value.HasMember("beta_header") && value["beta_header"].IsString()) {
    config.betaHeader = value["beta_header"].GetString();
  }
  if (value.HasMember("retry")) {
    config.retry = retryPolicyConfigFromJson(value["retry"]);
  }
  if (value.HasMember("model_variants") &&
      value["model_variants"].IsObject()) {
    config.modelVariants.clear();
    for (auto it = value["model_variants"].MemberBegin();
         it != value["model_variants"].MemberEnd(); ++it) {
      config.modelVariants[it->name.GetString()] =
          providerModelConfigFromJson(it->value);
    }
  }
  if (config.authMode.empty()) {
    config.authMode = config.allowMissingApiKey ? "none" : "api_key";
  }
  return config;
}

rapidjson::Value toJsonValue(const firmius::shared::UserConfig &config,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("default_provider_id",
                jsonString(config.defaultProviderId, allocator), allocator);
  out.AddMember("default_model_id", jsonString(config.defaultModelId, allocator),
                allocator);
  out.AddMember("default_model_variant",
                jsonString(config.defaultModelVariant, allocator), allocator);
  out.AddMember("default_lead_persona",
                jsonString(config.defaultLeadPersona, allocator), allocator);
  out.AddMember("default_temperature", config.defaultTemperature, allocator);
  out.AddMember("dangerously_skip_permissions",
                config.dangerouslySkipPermissions, allocator);
  if (config.defaultMaxTokens.has_value()) {
    out.AddMember("default_max_tokens", config.defaultMaxTokens.value(),
                  allocator);
  } else {
    out.AddMember("default_max_tokens", rapidjson::Value(rapidjson::kNullType),
                  allocator);
  }
  out.AddMember("show_internal_nudges", config.showInternalNudges, allocator);
  out.AddMember("hide_errors", config.hideErrors, allocator);
  out.AddMember("rolling_memory",
                toJsonValue(config.rollingMemory, allocator), allocator);
  rapidjson::Value routerCategories(rapidjson::kObjectType);
  for (const auto &[name, route] : config.modelRouterCategories) {
    routerCategories.AddMember(jsonString(name, allocator),
                               toJsonValue(route, allocator), allocator);
  }
  out.AddMember("model_router_categories", routerCategories, allocator);
  rapidjson::Value purposeRoutes(rapidjson::kObjectType);
  for (const auto &[purpose, category] : config.purposeRoutes) {
    purposeRoutes.AddMember(jsonString(purpose, allocator),
                            jsonString(category, allocator), allocator);
  }
  out.AddMember("purpose_routes", purposeRoutes, allocator);
  out.AddMember("default_route_category",
                jsonString(config.defaultRouteCategory, allocator), allocator);
  out.AddMember("enable_subagent_route_fallback",
                config.enableSubagentRouteFallback, allocator);
  rapidjson::Value fallbackOrder(rapidjson::kArrayType);
  for (const auto &category : config.subagentRouteFallbackOrder) {
    fallbackOrder.PushBack(jsonString(category, allocator), allocator);
  }
  out.AddMember("subagent_route_fallback_order", fallbackOrder, allocator);
  rapidjson::Value mcpServers(rapidjson::kObjectType);
  for (const auto &[name, server] : config.mcpServers) {
    mcpServers.AddMember(jsonString(name, allocator),
                         toJsonValue(server, allocator), allocator);
  }
  out.AddMember("mcp_servers", mcpServers, allocator);
  rapidjson::Value providers(rapidjson::kObjectType);
  for (const auto &[id, provider] : config.providers) {
    providers.AddMember(jsonString(id, allocator),
                        toJsonValue(provider, allocator), allocator);
  }
  out.AddMember("providers", providers, allocator);
  return out;
}

firmius::shared::UserConfig userConfigFromJson(const rapidjson::Value &value) {
  firmius::shared::UserConfig config;
  if (!value.IsObject()) {
    return config;
  }
  if (value.HasMember("default_provider_id") &&
      value["default_provider_id"].IsString()) {
    config.defaultProviderId = value["default_provider_id"].GetString();
  }
  if (value.HasMember("default_model_id") &&
      value["default_model_id"].IsString()) {
    config.defaultModelId = value["default_model_id"].GetString();
  }
  if (value.HasMember("default_model_variant") &&
      value["default_model_variant"].IsString()) {
    config.defaultModelVariant = value["default_model_variant"].GetString();
  }
  if (value.HasMember("default_lead_persona") &&
      value["default_lead_persona"].IsString()) {
    config.defaultLeadPersona = value["default_lead_persona"].GetString();
  }
  if (value.HasMember("default_temperature") &&
      value["default_temperature"].IsNumber()) {
    config.defaultTemperature = value["default_temperature"].GetFloat();
  }
  if (value.HasMember("dangerously_skip_permissions") &&
      value["dangerously_skip_permissions"].IsBool()) {
    config.dangerouslySkipPermissions =
        value["dangerously_skip_permissions"].GetBool();
  }
  if (value.HasMember("default_max_tokens")) {
    if (value["default_max_tokens"].IsUint()) {
      config.defaultMaxTokens = value["default_max_tokens"].GetUint();
    } else if (value["default_max_tokens"].IsNull()) {
      config.defaultMaxTokens = std::nullopt;
    }
  }
  if (value.HasMember("show_internal_nudges") &&
      value["show_internal_nudges"].IsBool()) {
    config.showInternalNudges = value["show_internal_nudges"].GetBool();
  }
  if (value.HasMember("hide_errors") && value["hide_errors"].IsBool()) {
    config.hideErrors = value["hide_errors"].GetBool();
  }
  if (value.HasMember("rolling_memory")) {
    config.rollingMemory = rollingMemoryConfigFromJson(value["rolling_memory"]);
  }
  if (value.HasMember("model_router_categories") &&
      value["model_router_categories"].IsObject()) {
    config.modelRouterCategories.clear();
    for (auto it = value["model_router_categories"].MemberBegin();
         it != value["model_router_categories"].MemberEnd(); ++it) {
      config.modelRouterCategories[it->name.GetString()] =
          modelRouteCategoryFromJson(it->value);
    }
  }
  if (value.HasMember("purpose_routes") &&
      value["purpose_routes"].IsObject()) {
    config.purposeRoutes.clear();
    for (auto it = value["purpose_routes"].MemberBegin();
         it != value["purpose_routes"].MemberEnd(); ++it) {
      if (it->value.IsString()) {
        config.purposeRoutes[it->name.GetString()] = it->value.GetString();
      }
    }
  }
  if (value.HasMember("default_route_category") &&
      value["default_route_category"].IsString()) {
    config.defaultRouteCategory = value["default_route_category"].GetString();
  }
  if (value.HasMember("enable_subagent_route_fallback") &&
      value["enable_subagent_route_fallback"].IsBool()) {
    config.enableSubagentRouteFallback =
        value["enable_subagent_route_fallback"].GetBool();
  }
  if (value.HasMember("subagent_route_fallback_order") &&
      value["subagent_route_fallback_order"].IsArray()) {
    config.subagentRouteFallbackOrder.clear();
    for (const auto &item : value["subagent_route_fallback_order"].GetArray()) {
      if (item.IsString()) {
        config.subagentRouteFallbackOrder.push_back(item.GetString());
      }
    }
  }
  if (value.HasMember("mcp_servers") && value["mcp_servers"].IsObject()) {
    config.mcpServers.clear();
    for (auto it = value["mcp_servers"].MemberBegin();
         it != value["mcp_servers"].MemberEnd(); ++it) {
      config.mcpServers[it->name.GetString()] =
          mcpServerConfigFromJson(it->value);
    }
  }
  if (value.HasMember("providers") && value["providers"].IsObject()) {
    config.providers.clear();
    for (auto it = value["providers"].MemberBegin();
         it != value["providers"].MemberEnd(); ++it) {
      config.providers[it->name.GetString()] =
          providerProfileConfigFromJson(it->value);
    }
  }
  return config;
}

} // namespace

rapidjson::Value toJsonValue(const WorkspacePresence &presence,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("cwd", jsonString(presence.cwd, allocator), allocator);
  out.AddMember("workspace_root", jsonString(presence.workspaceRoot, allocator),
                allocator);
  out.AddMember("repo_root", jsonString(presence.repoRoot, allocator), allocator);
  return out;
}

WorkspacePresence workspacePresenceFromJson(const rapidjson::Value &value) {
  WorkspacePresence presence;
  if (!value.IsObject()) {
    return presence;
  }
  if (value.HasMember("cwd") && value["cwd"].IsString()) {
    presence.cwd = value["cwd"].GetString();
  }
  if (value.HasMember("workspace_root") && value["workspace_root"].IsString()) {
    presence.workspaceRoot = value["workspace_root"].GetString();
  }
  if (value.HasMember("repo_root") && value["repo_root"].IsString()) {
    presence.repoRoot = value["repo_root"].GetString();
  }
  return presence;
}

rapidjson::Value toJsonValue(const ClientIdentity &identity,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("client_id", jsonString(identity.clientId, allocator), allocator);
  out.AddMember("ui_kind", jsonString(identity.uiKind, allocator), allocator);
  out.AddMember("pid", identity.pid, allocator);
  rapidjson::Value flags(rapidjson::kArrayType);
  for (const auto &flag : identity.capabilityFlags) {
    flags.PushBack(jsonString(flag, allocator), allocator);
  }
  out.AddMember("capability_flags", flags, allocator);
  return out;
}

ClientIdentity clientIdentityFromJson(const rapidjson::Value &value) {
  ClientIdentity identity;
  if (!value.IsObject()) {
    return identity;
  }
  if (value.HasMember("client_id") && value["client_id"].IsString()) {
    identity.clientId = value["client_id"].GetString();
  }
  if (value.HasMember("ui_kind") && value["ui_kind"].IsString()) {
    identity.uiKind = value["ui_kind"].GetString();
  }
  if (value.HasMember("pid") && value["pid"].IsInt()) {
    identity.pid = value["pid"].GetInt();
  }
  if (value.HasMember("capability_flags") && value["capability_flags"].IsArray()) {
    for (const auto &flag : value["capability_flags"].GetArray()) {
      if (flag.IsString()) {
        identity.capabilityFlags.emplace_back(flag.GetString());
      }
    }
  }
  return identity;
}

rapidjson::Value toJsonValue(const ClientSessionSnapshot &session,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("identity", toJsonValue(session.identity, allocator), allocator);
  out.AddMember("presence", toJsonValue(session.presence, allocator), allocator);
  out.AddMember("focused_thread_id", jsonString(session.focusedThreadId, allocator),
                allocator);
  out.AddMember("focused_agent_id", jsonString(session.focusedAgentId, allocator),
                allocator);
  out.AddMember("connected_at_ms", session.connectedAtMs, allocator);
  out.AddMember("last_seen_at_ms", session.lastSeenAtMs, allocator);
  out.AddMember("subscribed", session.subscribed, allocator);
  return out;
}

ClientSessionSnapshot clientSessionSnapshotFromJson(const rapidjson::Value &value) {
  ClientSessionSnapshot session;
  if (!value.IsObject()) {
    return session;
  }
  if (value.HasMember("identity")) {
    session.identity = clientIdentityFromJson(value["identity"]);
  }
  if (value.HasMember("presence")) {
    session.presence = workspacePresenceFromJson(value["presence"]);
  }
  if (value.HasMember("focused_thread_id") && value["focused_thread_id"].IsString()) {
    session.focusedThreadId = value["focused_thread_id"].GetString();
  }
  if (value.HasMember("focused_agent_id") && value["focused_agent_id"].IsString()) {
    session.focusedAgentId = value["focused_agent_id"].GetString();
  }
  if (value.HasMember("connected_at_ms") && value["connected_at_ms"].IsUint64()) {
    session.connectedAtMs = value["connected_at_ms"].GetUint64();
  }
  if (value.HasMember("last_seen_at_ms") && value["last_seen_at_ms"].IsUint64()) {
    session.lastSeenAtMs = value["last_seen_at_ms"].GetUint64();
  }
  if (value.HasMember("subscribed") && value["subscribed"].IsBool()) {
    session.subscribed = value["subscribed"].GetBool();
  }
  return session;
}

rapidjson::Value toJsonValue(const DaemonPingResponse &response,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("protocol_version", jsonString(response.protocolVersion, allocator),
                allocator);
  out.AddMember("server_name", jsonString(response.serverName, allocator), allocator);
  out.AddMember("pid", response.pid, allocator);
  out.AddMember("ok", response.ok, allocator);
  return out;
}

DaemonPingResponse daemonPingResponseFromJson(const rapidjson::Value &value) {
  DaemonPingResponse response;
  if (!value.IsObject()) {
    return response;
  }
  if (value.HasMember("protocol_version") && value["protocol_version"].IsString()) {
    response.protocolVersion = value["protocol_version"].GetString();
  }
  if (value.HasMember("server_name") && value["server_name"].IsString()) {
    response.serverName = value["server_name"].GetString();
  }
  if (value.HasMember("pid") && value["pid"].IsInt()) {
    response.pid = value["pid"].GetInt();
  }
  if (value.HasMember("ok") && value["ok"].IsBool()) {
    response.ok = value["ok"].GetBool();
  }
  return response;
}

rapidjson::Value toJsonValue(const ClientHelloRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("protocol_version", jsonString(request.protocolVersion, allocator),
                allocator);
  out.AddMember("identity", toJsonValue(request.identity, allocator), allocator);
  out.AddMember("presence", toJsonValue(request.presence, allocator), allocator);
  out.AddMember("focused_thread_id", jsonString(request.focusedThreadId, allocator),
                allocator);
  out.AddMember("focused_agent_id", jsonString(request.focusedAgentId, allocator),
                allocator);
  return out;
}

ClientHelloRequest clientHelloRequestFromJson(const rapidjson::Value &value) {
  ClientHelloRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("protocol_version") && value["protocol_version"].IsString()) {
    request.protocolVersion = value["protocol_version"].GetString();
  }
  if (value.HasMember("identity")) {
    request.identity = clientIdentityFromJson(value["identity"]);
  }
  if (value.HasMember("presence")) {
    request.presence = workspacePresenceFromJson(value["presence"]);
  }
  if (value.HasMember("focused_thread_id") && value["focused_thread_id"].IsString()) {
    request.focusedThreadId = value["focused_thread_id"].GetString();
  }
  if (value.HasMember("focused_agent_id") && value["focused_agent_id"].IsString()) {
    request.focusedAgentId = value["focused_agent_id"].GetString();
  }
  return request;
}

rapidjson::Value toJsonValue(const ClientHelloResponse &response,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("protocol_version", jsonString(response.protocolVersion, allocator),
                allocator);
  out.AddMember("session", toJsonValue(response.session, allocator), allocator);
  return out;
}
rapidjson::Value toJsonValue(const DaemonAuditEmitRuntimeEventRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("event_type", jsonString(request.eventType, allocator), allocator);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  out.AddMember("parent_agent_id", jsonString(request.parentAgentId, allocator),
                allocator);
  out.AddMember("text", jsonString(request.text, allocator), allocator);
  out.AddMember("tool_call_id", jsonString(request.toolCallId, allocator), allocator);
  out.AddMember("tool_name", jsonString(request.toolName, allocator), allocator);
  out.AddMember("tool_args_json", jsonString(request.toolArgsJson, allocator),
                allocator);
  return out;
}

DaemonAuditEmitRuntimeEventRequest
daemonAuditEmitRuntimeEventRequestFromJson(const rapidjson::Value &value) {
  DaemonAuditEmitRuntimeEventRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("event_type") && value["event_type"].IsString()) {
    request.eventType = value["event_type"].GetString();
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("parent_agent_id") && value["parent_agent_id"].IsString()) {
    request.parentAgentId = value["parent_agent_id"].GetString();
  }
  if (value.HasMember("text") && value["text"].IsString()) {
    request.text = value["text"].GetString();
  }
  if (value.HasMember("tool_call_id") && value["tool_call_id"].IsString()) {
    request.toolCallId = value["tool_call_id"].GetString();
  }
  if (value.HasMember("tool_name") && value["tool_name"].IsString()) {
    request.toolName = value["tool_name"].GetString();
  }
  if (value.HasMember("tool_args_json") && value["tool_args_json"].IsString()) {
    request.toolArgsJson = value["tool_args_json"].GetString();
  }
  return request;
}

rapidjson::Value toJsonValue(const DaemonAuditEmitRuntimeEventResponse &response,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("emitted", response.emitted, allocator);
  out.AddMember("runtime_event_type", jsonString(response.runtimeEventType, allocator),
                allocator);
  out.AddMember("thread_id", jsonString(response.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(response.agentId, allocator), allocator);
  return out;
}

DaemonAuditEmitRuntimeEventResponse
daemonAuditEmitRuntimeEventResponseFromJson(const rapidjson::Value &value) {
  DaemonAuditEmitRuntimeEventResponse response;
  if (!value.IsObject()) {
    return response;
  }
  if (value.HasMember("emitted") && value["emitted"].IsBool()) {
    response.emitted = value["emitted"].GetBool();
  }
  if (value.HasMember("runtime_event_type") && value["runtime_event_type"].IsString()) {
    response.runtimeEventType = value["runtime_event_type"].GetString();
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    response.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    response.agentId = value["agent_id"].GetString();
  }
  return response;
}

ClientHelloResponse clientHelloResponseFromJson(const rapidjson::Value &value) {
  ClientHelloResponse response;
  if (!value.IsObject()) {
    return response;
  }
  if (value.HasMember("protocol_version") && value["protocol_version"].IsString()) {
    response.protocolVersion = value["protocol_version"].GetString();
  }
  if (value.HasMember("session")) {
    response.session = clientSessionSnapshotFromJson(value["session"]);
  }
  return response;
}

rapidjson::Value toJsonValue(const ThreadsOpenResponse &response,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("opened", response.opened, allocator);
  auto threadDoc = firmius::shared::toJson(response.thread);
  rapidjson::Value threadValue(rapidjson::kObjectType);
  threadValue.CopyFrom(threadDoc, allocator);
  out.AddMember("thread", threadValue, allocator);
  out.AddMember("focused_agent_id", jsonString(response.focusedAgentId, allocator),
                allocator);
  return out;
}

ThreadsOpenResponse threadsOpenResponseFromJson(const rapidjson::Value &value) {
  ThreadsOpenResponse response;
  if (!value.IsObject()) {
    return response;
  }
  if (value.HasMember("opened") && value["opened"].IsBool()) {
    response.opened = value["opened"].GetBool();
  }
  if (value.HasMember("thread")) {
    response.thread = firmius::shared::threadMetadataFromJson(value["thread"]);
  }
  if (value.HasMember("focused_agent_id") && value["focused_agent_id"].IsString()) {
    response.focusedAgentId = value["focused_agent_id"].GetString();
  }
  return response;
}

rapidjson::Value toJsonValue(const ThreadsCreateRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("cwd", jsonString(request.cwd, allocator), allocator);
  out.AddMember("lead_persona", jsonString(request.leadPersona, allocator), allocator);
  out.AddMember("initial_mode", jsonString(request.initialMode, allocator), allocator);
  out.AddMember(
      "permission_mode",
      jsonString(firmius::shared::permissionModeStorageString(request.permissionMode),
                 allocator),
      allocator);
  return out;
}

ThreadsCreateRequest threadsCreateRequestFromJson(const rapidjson::Value &value) {
  ThreadsCreateRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("cwd") && value["cwd"].IsString()) {
    request.cwd = value["cwd"].GetString();
  }
  if (value.HasMember("lead_persona") && value["lead_persona"].IsString()) {
    request.leadPersona = value["lead_persona"].GetString();
  }
  if (value.HasMember("initial_mode") && value["initial_mode"].IsString()) {
    request.initialMode = value["initial_mode"].GetString();
  }
  if (value.HasMember("permission_mode") && value["permission_mode"].IsString()) {
    request.permissionMode = firmius::shared::permissionModeFromStorageString(
        value["permission_mode"].GetString());
  }
  return request;
}

rapidjson::Value toJsonValue(const ThreadsCreateResponse &response,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  auto threadDoc = firmius::shared::toJson(response.thread);
  rapidjson::Value threadValue(rapidjson::kObjectType);
  threadValue.CopyFrom(threadDoc, allocator);
  out.AddMember("thread", threadValue, allocator);
  out.AddMember("focused_agent_id", jsonString(response.focusedAgentId, allocator),
                allocator);
  return out;
}

ThreadsCreateResponse threadsCreateResponseFromJson(const rapidjson::Value &value) {
  ThreadsCreateResponse response;
  if (!value.IsObject()) {
    return response;
  }
  if (value.HasMember("thread")) {
    response.thread = firmius::shared::threadMetadataFromJson(value["thread"]);
  }
  if (value.HasMember("focused_agent_id") && value["focused_agent_id"].IsString()) {
    response.focusedAgentId = value["focused_agent_id"].GetString();
  }
  return response;
}

rapidjson::Value toJsonValue(const ThreadsSendRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  out.AddMember("text", jsonString(request.text, allocator), allocator);
  rapidjson::Value images(rapidjson::kArrayType);
  for (const auto &image : request.images) {
    images.PushBack(messagePartValue(image, allocator), allocator);
  }
  out.AddMember("images", images, allocator);
  return out;
}

ThreadsSendRequest threadsSendRequestFromJson(const rapidjson::Value &value) {
  ThreadsSendRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("text") && value["text"].IsString()) {
    request.text = value["text"].GetString();
  }
  if (value.HasMember("images") && value["images"].IsArray()) {
    for (const auto &image : value["images"].GetArray()) {
      request.images.push_back(imageContentFromValue(image));
    }
  }
  return request;
}

rapidjson::Value toJsonValue(const ThreadsSendResponse &response,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("accepted", response.accepted, allocator);
  out.AddMember("thread_id", jsonString(response.threadId, allocator), allocator);
  out.AddMember("focused_agent_id", jsonString(response.focusedAgentId, allocator),
                allocator);
  return out;
}

ThreadsSendResponse threadsSendResponseFromJson(const rapidjson::Value &value) {
  ThreadsSendResponse response;
  if (!value.IsObject()) {
    return response;
  }
  if (value.HasMember("accepted") && value["accepted"].IsBool()) {
    response.accepted = value["accepted"].GetBool();
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    response.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("focused_agent_id") && value["focused_agent_id"].IsString()) {
    response.focusedAgentId = value["focused_agent_id"].GetString();
  }
  return response;
}

rapidjson::Value toJsonValue(const UiSnapshotRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  out.AddMember("include_transcript", request.includeTranscript, allocator);
  out.AddMember("include_tool_calls", request.includeToolCalls, allocator);
  out.AddMember("include_processes", request.includeProcesses, allocator);
  out.AddMember("include_config", request.includeConfig, allocator);
  out.AddMember("include_catalogs", request.includeCatalogs, allocator);
  return out;
}

UiSnapshotRequest uiSnapshotRequestFromJson(const rapidjson::Value &value) {
  UiSnapshotRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("include_transcript") && value["include_transcript"].IsBool()) {
    request.includeTranscript = value["include_transcript"].GetBool();
  }
  if (value.HasMember("include_tool_calls") && value["include_tool_calls"].IsBool()) {
    request.includeToolCalls = value["include_tool_calls"].GetBool();
  }
  if (value.HasMember("include_processes") && value["include_processes"].IsBool()) {
    request.includeProcesses = value["include_processes"].GetBool();
  }
  if (value.HasMember("include_config") && value["include_config"].IsBool()) {
    request.includeConfig = value["include_config"].GetBool();
  }
  if (value.HasMember("include_catalogs") && value["include_catalogs"].IsBool()) {
    request.includeCatalogs = value["include_catalogs"].GetBool();
  }
  return request;
}

rapidjson::Value toJsonValue(const UiSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("session", toJsonValue(snapshot.session, allocator), allocator);
  out.AddMember("threads", toJsonValue(snapshot.threads, allocator), allocator);
  if (snapshot.focusedThread) {
    out.AddMember("focused_thread", toJsonValue(*snapshot.focusedThread, allocator), allocator);
  }
  out.AddMember("agents", toJsonValue(snapshot.agents, allocator), allocator);
  if (snapshot.focusedAgent) {
    out.AddMember("focused_agent", toJsonValue(*snapshot.focusedAgent, allocator), allocator);
  }
  if (snapshot.focusedAgentTodo) {
    out.AddMember("focused_agent_todo",
                  toJsonValue(*snapshot.focusedAgentTodo, allocator), allocator);
  }
  if (snapshot.transcript) {
    out.AddMember("transcript", toJsonValue(*snapshot.transcript, allocator), allocator);
  }
  out.AddMember("tool_calls", toJsonValue(snapshot.toolCalls, allocator), allocator);
  out.AddMember("subagents", toJsonValue(snapshot.subagents, allocator), allocator);
  out.AddMember("process_summary", toJsonValue(snapshot.processSummary, allocator), allocator);
  out.AddMember("processes", toJsonValue(snapshot.processes, allocator), allocator);
  out.AddMember("permissions", toJsonValue(snapshot.permissions, allocator), allocator);
  out.AddMember("models", toJsonValue(snapshot.models, allocator), allocator);
  out.AddMember("providers", toJsonValue(snapshot.providers, allocator), allocator);
  out.AddMember("config", toJsonValue(snapshot.config, allocator), allocator);
  out.AddMember("router", toJsonValue(snapshot.router, allocator), allocator);
  out.AddMember("purposes", toJsonValue(snapshot.purposes, allocator), allocator);
  out.AddMember("rolling_memory", toJsonValue(snapshot.rollingMemory, allocator), allocator);
  out.AddMember("mcp", toJsonValue(snapshot.mcp, allocator), allocator);
  out.AddMember("hooks", toJsonValue(snapshot.hooks, allocator), allocator);
  out.AddMember("pacts", toJsonValue(snapshot.pacts, allocator), allocator);
  out.AddMember("artifacts", toJsonValue(snapshot.artifacts, allocator), allocator);
  out.AddMember("history", toJsonValue(snapshot.history, allocator), allocator);
  out.AddMember("edits", toJsonValue(snapshot.edits, allocator), allocator);
  out.AddMember("latest_event_sequence", snapshot.latestEventSequence, allocator);
  return out;
}

UiSnapshot uiSnapshotFromJson(const rapidjson::Value &value) {
  UiSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("session")) snapshot.session = clientSessionSnapshotFromJson(value["session"]);
  if (value.HasMember("threads")) snapshot.threads = threadMetadataListFromJson(value["threads"]);
  if (value.HasMember("focused_thread")) snapshot.focusedThread = threadSnapshotFromJson(value["focused_thread"]);
  if (value.HasMember("agents")) snapshot.agents = agentTreeSnapshotFromJson(value["agents"]);
  if (value.HasMember("focused_agent")) snapshot.focusedAgent = agentRuntimeSnapshotFromJson(value["focused_agent"]);
  if (value.HasMember("focused_agent_todo")) {
    snapshot.focusedAgentTodo =
        agentTodoSnapshotFromJson(value["focused_agent_todo"]);
  }
  if (value.HasMember("transcript")) snapshot.transcript = transcriptSnapshotFromJson(value["transcript"]);
  if (value.HasMember("tool_calls")) snapshot.toolCalls = toolCallSnapshotListFromJson(value["tool_calls"]);
  if (value.HasMember("subagents")) snapshot.subagents = subagentActivitySnapshotFromJson(value["subagents"]);
  if (value.HasMember("process_summary")) snapshot.processSummary = processRuntimeSummaryFromJson(value["process_summary"]);
  if (value.HasMember("processes")) snapshot.processes = processSnapshotListFromJson(value["processes"]);
  if (value.HasMember("permissions")) snapshot.permissions = permissionQueueSnapshotFromJson(value["permissions"]);
  if (value.HasMember("models")) snapshot.models = modelCatalogSnapshotFromJson(value["models"]);
  if (value.HasMember("providers")) snapshot.providers = providerCatalogSnapshotFromJson(value["providers"]);
  if (value.HasMember("config")) snapshot.config = userConfigSnapshotFromJson(value["config"]);
  if (value.HasMember("router")) snapshot.router = routerConfigSnapshotFromJson(value["router"]);
  if (value.HasMember("purposes")) snapshot.purposes = purposesConfigSnapshotFromJson(value["purposes"]);
  if (value.HasMember("rolling_memory")) snapshot.rollingMemory = rollingMemoryConfigSnapshotFromJson(value["rolling_memory"]);
  if (value.HasMember("mcp")) snapshot.mcp = mcpConfigSnapshotFromJson(value["mcp"]);
  if (value.HasMember("hooks")) snapshot.hooks = hookStateSnapshotFromJson(value["hooks"]);
  if (value.HasMember("pacts") && value["pacts"].IsArray()) {
    for (const auto &entry : value["pacts"].GetArray()) snapshot.pacts.push_back(pactSnapshotFromJson(entry));
  }
  if (value.HasMember("artifacts")) snapshot.artifacts = artifactCatalogSnapshotFromJson(value["artifacts"]);
  if (value.HasMember("history")) snapshot.history = historySnapshotFromJson(value["history"]);
  if (value.HasMember("edits")) snapshot.edits = editHistorySnapshotFromJson(value["edits"]);
  if (value.HasMember("latest_event_sequence") && value["latest_event_sequence"].IsUint64()) {
    snapshot.latestEventSequence = value["latest_event_sequence"].GetUint64();
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const EventSubscriptionRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value kinds(rapidjson::kArrayType);
  for (const auto &kind : request.eventKinds) {
    kinds.PushBack(jsonString(kind, allocator), allocator);
  }
  out.AddMember("event_kinds", kinds, allocator);
  out.AddMember("since_sequence", request.sinceSequence, allocator);
  return out;
}

EventSubscriptionRequest eventSubscriptionRequestFromJson(const rapidjson::Value &value) {
  EventSubscriptionRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("event_kinds") && value["event_kinds"].IsArray()) {
    for (const auto &kind : value["event_kinds"].GetArray()) {
      if (kind.IsString()) {
        request.eventKinds.emplace_back(kind.GetString());
      }
    }
  }
  if (value.HasMember("since_sequence") && value["since_sequence"].IsUint64()) {
    request.sinceSequence = value["since_sequence"].GetUint64();
  }
  return request;
}

rapidjson::Value toJsonValue(const EventSubscriptionResponse &response,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("subscribed", response.subscribed, allocator);
  return out;
}

EventSubscriptionResponse eventSubscriptionResponseFromJson(const rapidjson::Value &value) {
  EventSubscriptionResponse response;
  if (!value.IsObject()) {
    return response;
  }
  if (value.HasMember("subscribed") && value["subscribed"].IsBool()) {
    response.subscribed = value["subscribed"].GetBool();
  }
  return response;
}

rapidjson::Value toJsonValue(const DaemonEventEnvelope &event,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("kind", jsonString(eventKindToString(event.kind), allocator), allocator);
  out.AddMember("subscription_target", jsonString(event.subscriptionTarget, allocator),
                allocator);
  out.AddMember("server_timestamp_ms", event.serverTimestampMs, allocator);
  out.AddMember("sequence", event.sequence, allocator);
  out.AddMember("runtime_event_type", jsonString(event.runtimeEventType, allocator),
                allocator);
  out.AddMember("runtime_event_thread_id",
                jsonString(event.runtimeEventThreadId, allocator), allocator);
  out.AddMember("runtime_event_agent_id",
                jsonString(event.runtimeEventAgentId, allocator), allocator);
  out.AddMember("runtime_event_json", jsonString(event.runtimeEventJson, allocator),
                allocator);
  if (event.agentStatus.has_value()) {
    out.AddMember("agent_status",
                  jsonString(agentStatusToWire(*event.agentStatus), allocator),
                  allocator);
  }
  if (event.session.has_value()) {
    out.AddMember("session", toJsonValue(*event.session, allocator), allocator);
  }
  if (event.hookState.has_value()) {
    out.AddMember("hook_state", toJsonValue(*event.hookState, allocator),
                  allocator);
  }
  if (event.pactState.has_value()) {
    out.AddMember("pact_state", toJsonValue(*event.pactState, allocator),
                  allocator);
  }
  if (!event.initMessage.empty()) {
    out.AddMember("init_message", jsonString(event.initMessage, allocator),
                  allocator);
  }
  return out;
}

DaemonEventEnvelope daemonEventEnvelopeFromJson(const rapidjson::Value &value) {
  DaemonEventEnvelope event;
  if (!value.IsObject()) {
    return event;
  }
  if (value.HasMember("kind") && value["kind"].IsString()) {
    event.kind = eventKindFromString(value["kind"].GetString());
  }
  if (value.HasMember("subscription_target") && value["subscription_target"].IsString()) {
    event.subscriptionTarget = value["subscription_target"].GetString();
  }
  if (value.HasMember("server_timestamp_ms") && value["server_timestamp_ms"].IsUint64()) {
    event.serverTimestampMs = value["server_timestamp_ms"].GetUint64();
  }
  if (value.HasMember("sequence") && value["sequence"].IsUint64()) {
    event.sequence = value["sequence"].GetUint64();
  }
  if (value.HasMember("runtime_event_type") && value["runtime_event_type"].IsString()) {
    event.runtimeEventType = value["runtime_event_type"].GetString();
  }
  if (value.HasMember("runtime_event_thread_id") &&
      value["runtime_event_thread_id"].IsString()) {
    event.runtimeEventThreadId = value["runtime_event_thread_id"].GetString();
  }
  if (value.HasMember("runtime_event_agent_id") &&
      value["runtime_event_agent_id"].IsString()) {
    event.runtimeEventAgentId = value["runtime_event_agent_id"].GetString();
  }
  if (value.HasMember("runtime_event_json") &&
      value["runtime_event_json"].IsString()) {
    event.runtimeEventJson = value["runtime_event_json"].GetString();
  }
  if (value.HasMember("agent_status") && value["agent_status"].IsString()) {
    event.agentStatus = agentStatusFromWire(value["agent_status"].GetString());
  }
  if (value.HasMember("session")) {
    event.session = clientSessionSnapshotFromJson(value["session"]);
  }
  if (value.HasMember("hook_state") && value["hook_state"].IsObject()) {
    event.hookState = hookStateSnapshotFromJson(value["hook_state"]);
  }
  if (value.HasMember("pact_state") && value["pact_state"].IsObject()) {
    event.pactState = pactSnapshotFromJson(value["pact_state"]);
  }
  if (value.HasMember("init_message") && value["init_message"].IsString()) {
    event.initMessage = value["init_message"].GetString();
  }
  return event;
}

rapidjson::Value toJsonValue(const std::vector<ClientSessionSnapshot> &sessions,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &session : sessions) {
    out.PushBack(toJsonValue(session, allocator), allocator);
  }
  return out;
}

std::vector<ClientSessionSnapshot> clientSessionSnapshotListFromJson(
    const rapidjson::Value &value) {
  std::vector<ClientSessionSnapshot> sessions;
  if (!value.IsArray()) {
    return sessions;
  }
  for (const auto &entry : value.GetArray()) {
    sessions.push_back(clientSessionSnapshotFromJson(entry));
  }
  return sessions;
}

rapidjson::Value toJsonValue(const std::vector<firmius::shared::ThreadMetadata> &threads,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &thread : threads) {
    auto threadDoc = firmius::shared::toJson(thread);
    rapidjson::Value entry(rapidjson::kObjectType);
    entry.CopyFrom(threadDoc, allocator);
    out.PushBack(entry, allocator);
  }
  return out;
}

std::vector<firmius::shared::ThreadMetadata> threadMetadataListFromJson(
    const rapidjson::Value &value) {
  std::vector<firmius::shared::ThreadMetadata> threads;
  if (!value.IsArray()) {
    return threads;
  }
  for (const auto &entry : value.GetArray()) {
    threads.push_back(firmius::shared::threadMetadataFromJson(entry));
  }
  return threads;
}

rapidjson::Value toJsonValue(const ThreadSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  auto threadDoc = firmius::shared::toJson(snapshot.thread);
  rapidjson::Value threadValue(rapidjson::kObjectType);
  threadValue.CopyFrom(threadDoc, allocator);
  out.AddMember("thread", threadValue, allocator);
  out.AddMember("focused_agent_id", jsonString(snapshot.focusedAgentId, allocator),
                allocator);
  rapidjson::Value agentIds(rapidjson::kArrayType);
  for (const auto &agentId : snapshot.agentIds) {
    agentIds.PushBack(jsonString(agentId, allocator), allocator);
  }
  out.AddMember("agent_ids", agentIds, allocator);
  out.AddMember("artifact_count",
                static_cast<std::uint64_t>(snapshot.artifactCount), allocator);
  out.AddMember("pending_permission_count",
                static_cast<std::uint64_t>(snapshot.pendingPermissionCount),
                allocator);
  out.AddMember("focused", snapshot.focused, allocator);
  return out;
}

ThreadSnapshot threadSnapshotFromJson(const rapidjson::Value &value) {
  ThreadSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread")) {
    snapshot.thread = firmius::shared::threadMetadataFromJson(value["thread"]);
  }
  if (value.HasMember("focused_agent_id") &&
      value["focused_agent_id"].IsString()) {
    snapshot.focusedAgentId = value["focused_agent_id"].GetString();
  }
  if (value.HasMember("agent_ids") && value["agent_ids"].IsArray()) {
    for (const auto &agentId : value["agent_ids"].GetArray()) {
      if (agentId.IsString()) {
        snapshot.agentIds.push_back(agentId.GetString());
      }
    }
  }
  if (value.HasMember("artifact_count") && value["artifact_count"].IsUint64()) {
    snapshot.artifactCount =
        static_cast<std::size_t>(value["artifact_count"].GetUint64());
  }
  if (value.HasMember("pending_permission_count") &&
      value["pending_permission_count"].IsUint64()) {
    snapshot.pendingPermissionCount = static_cast<std::size_t>(
        value["pending_permission_count"].GetUint64());
  }
  if (value.HasMember("focused") && value["focused"].IsBool()) {
    snapshot.focused = value["focused"].GetBool();
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const AgentTargetRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  return out;
}

AgentTargetRequest agentTargetRequestFromJson(const rapidjson::Value &value) {
  AgentTargetRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  return request;
}

rapidjson::Value toJsonValue(const AgentRuntimeSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(snapshot.agentId, allocator), allocator);
  out.AddMember("parent_agent_id", jsonString(snapshot.parentAgentId, allocator),
                allocator);
  out.AddMember("persona", jsonString(snapshot.persona, allocator), allocator);
  out.AddMember("friendly_name", jsonString(snapshot.friendlyName, allocator),
                allocator);
  out.AddMember("title", jsonString(snapshot.title, allocator), allocator);
  out.AddMember("cwd", jsonString(snapshot.cwd, allocator), allocator);
  out.AddMember("host_id", jsonString(snapshot.hostId, allocator), allocator);
  out.AddMember("active_mode", jsonString(snapshot.activeMode, allocator),
                allocator);
  out.AddMember("status", static_cast<int>(snapshot.status), allocator);
  out.AddMember("provider_id", jsonString(snapshot.providerId, allocator),
                allocator);
  out.AddMember("model_id", jsonString(snapshot.modelId, allocator), allocator);
  out.AddMember("variant_name", jsonString(snapshot.variantName, allocator),
                allocator);
  out.AddMember("max_tokens", snapshot.maxTokens, allocator);
  out.AddMember("context_window_tokens", snapshot.contextWindowTokens, allocator);
  out.AddMember("context_used_tokens", snapshot.contextUsedTokens, allocator);
  out.AddMember("context_sent_tokens", snapshot.contextSentTokens, allocator);
  rapidjson::Value pending(rapidjson::kArrayType);
  for (const auto &toolCallId : snapshot.pendingToolCalls) {
    pending.PushBack(jsonString(toolCallId, allocator), allocator);
  }
  out.AddMember("pending_tool_calls", pending, allocator);
  rapidjson::Value owned(rapidjson::kArrayType);
  for (const auto &processId : snapshot.ownedProcesses) {
    owned.PushBack(jsonString(processId, allocator), allocator);
  }
  out.AddMember("owned_processes", owned, allocator);
  rapidjson::Value blocking(rapidjson::kArrayType);
  for (const auto &processId : snapshot.blockingProcessIds) {
    blocking.PushBack(jsonString(processId, allocator), allocator);
  }
  out.AddMember("blocking_process_ids", blocking, allocator);
  if (snapshot.fatalError.has_value()) {
    out.AddMember("fatal_error",
                  jsonString(snapshot.fatalError.value(), allocator), allocator);
  } else {
    out.AddMember("fatal_error", rapidjson::Value(rapidjson::kNullType),
                  allocator);
  }
  out.AddMember("running", snapshot.running, allocator);
  out.AddMember("booting", snapshot.booting, allocator);
  out.AddMember("focused", snapshot.focused, allocator);
  out.AddMember("live", snapshot.live, allocator);
  return out;
}

AgentRuntimeSnapshot agentRuntimeSnapshotFromJson(const rapidjson::Value &value) {
  AgentRuntimeSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    snapshot.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("parent_agent_id") &&
      value["parent_agent_id"].IsString()) {
    snapshot.parentAgentId = value["parent_agent_id"].GetString();
  }
  if (value.HasMember("persona") && value["persona"].IsString()) {
    snapshot.persona = value["persona"].GetString();
  }
  if (value.HasMember("friendly_name") && value["friendly_name"].IsString()) {
    snapshot.friendlyName = value["friendly_name"].GetString();
  }
  if (value.HasMember("title") && value["title"].IsString()) {
    snapshot.title = value["title"].GetString();
  }
  if (value.HasMember("cwd") && value["cwd"].IsString()) {
    snapshot.cwd = value["cwd"].GetString();
  }
  if (value.HasMember("host_id") && value["host_id"].IsString()) {
    snapshot.hostId = value["host_id"].GetString();
  }
  if (value.HasMember("active_mode") && value["active_mode"].IsString()) {
    snapshot.activeMode = value["active_mode"].GetString();
  }
  if (value.HasMember("status") && value["status"].IsInt()) {
    snapshot.status =
        static_cast<firmius::shared::AgentStatus>(value["status"].GetInt());
  }
  if (value.HasMember("provider_id") && value["provider_id"].IsString()) {
    snapshot.providerId = value["provider_id"].GetString();
  }
  if (value.HasMember("model_id") && value["model_id"].IsString()) {
    snapshot.modelId = value["model_id"].GetString();
  }
  if (value.HasMember("variant_name") && value["variant_name"].IsString()) {
    snapshot.variantName = value["variant_name"].GetString();
  }
  if (value.HasMember("max_tokens") && value["max_tokens"].IsUint()) {
    snapshot.maxTokens = value["max_tokens"].GetUint();
  }
  if (value.HasMember("context_window_tokens") &&
      value["context_window_tokens"].IsUint()) {
    snapshot.contextWindowTokens = value["context_window_tokens"].GetUint();
  }
  if (value.HasMember("context_used_tokens") &&
      value["context_used_tokens"].IsUint()) {
    snapshot.contextUsedTokens = value["context_used_tokens"].GetUint();
  }
  if (value.HasMember("context_sent_tokens") &&
      value["context_sent_tokens"].IsUint()) {
    snapshot.contextSentTokens = value["context_sent_tokens"].GetUint();
  }
  if (value.HasMember("pending_tool_calls") &&
      value["pending_tool_calls"].IsArray()) {
    for (const auto &toolCallId : value["pending_tool_calls"].GetArray()) {
      if (toolCallId.IsString()) {
        snapshot.pendingToolCalls.push_back(toolCallId.GetString());
      }
    }
  }
  if (value.HasMember("owned_processes") && value["owned_processes"].IsArray()) {
    for (const auto &processId : value["owned_processes"].GetArray()) {
      if (processId.IsString()) {
        snapshot.ownedProcesses.push_back(processId.GetString());
      }
    }
  }
  if (value.HasMember("blocking_process_ids") &&
      value["blocking_process_ids"].IsArray()) {
    for (const auto &processId : value["blocking_process_ids"].GetArray()) {
      if (processId.IsString()) {
        snapshot.blockingProcessIds.push_back(processId.GetString());
      }
    }
  }
  if (value.HasMember("fatal_error") && value["fatal_error"].IsString()) {
    snapshot.fatalError = value["fatal_error"].GetString();
  }
  if (value.HasMember("running") && value["running"].IsBool()) {
    snapshot.running = value["running"].GetBool();
  }
  if (value.HasMember("booting") && value["booting"].IsBool()) {
    snapshot.booting = value["booting"].GetBool();
  }
  if (value.HasMember("focused") && value["focused"].IsBool()) {
    snapshot.focused = value["focused"].GetBool();
  }
  if (value.HasMember("live") && value["live"].IsBool()) {
    snapshot.live = value["live"].GetBool();
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const AgentTodoSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(snapshot.agentId, allocator), allocator);
  out.AddMember("next_id", snapshot.nextId, allocator);
  rapidjson::Value items(rapidjson::kArrayType);
  for (const auto &item : snapshot.items) {
    auto itemJson = firmius::shared::toJson(item);
    rapidjson::Value row(rapidjson::kObjectType);
    row.CopyFrom(itemJson, allocator);
    items.PushBack(row, allocator);
  }
  out.AddMember("items", items, allocator);
  return out;
}

AgentTodoSnapshot agentTodoSnapshotFromJson(const rapidjson::Value &value) {
  AgentTodoSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    snapshot.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("next_id") && value["next_id"].IsInt()) {
    snapshot.nextId = value["next_id"].GetInt();
  }
  if (value.HasMember("items") && value["items"].IsArray()) {
    for (const auto &item : value["items"].GetArray()) {
      snapshot.items.push_back(firmius::shared::todoItemFromJson(item));
    }
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const AgentTreeSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("focused_agent_id", jsonString(snapshot.focusedAgentId, allocator),
                allocator);
  out.AddMember("agents", toJsonValue(snapshot.agents, allocator), allocator);
  return out;
}

AgentTreeSnapshot agentTreeSnapshotFromJson(const rapidjson::Value &value) {
  AgentTreeSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("focused_agent_id") &&
      value["focused_agent_id"].IsString()) {
    snapshot.focusedAgentId = value["focused_agent_id"].GetString();
  }
  if (value.HasMember("agents")) {
    snapshot.agents = agentRuntimeSnapshotListFromJson(value["agents"]);
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const ProcessesListRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  return out;
}

ProcessesListRequest processesListRequestFromJson(const rapidjson::Value &value) {
  ProcessesListRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  return request;
}

rapidjson::Value toJsonValue(const ProcessesGetRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out =
      toJsonValue(ProcessesListRequest{request.threadId, request.agentId},
                  allocator);
  out.AddMember("process_id", jsonString(request.processId, allocator), allocator);
  return out;
}

ProcessesGetRequest processesGetRequestFromJson(const rapidjson::Value &value) {
  ProcessesGetRequest request;
  if (!value.IsObject()) {
    return request;
  }
  auto base = processesListRequestFromJson(value);
  request.threadId = base.threadId;
  request.agentId = base.agentId;
  if (value.HasMember("process_id") && value["process_id"].IsString()) {
    request.processId = value["process_id"].GetString();
  }
  return request;
}

rapidjson::Value toJsonValue(const ProcessSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(snapshot.agentId, allocator), allocator);
  out.AddMember("process_id", jsonString(snapshot.processId, allocator), allocator);
  out.AddMember("tool_call_id", jsonString(snapshot.toolCallId, allocator),
                allocator);
  out.AddMember("running", snapshot.running, allocator);
  out.AddMember("exit_code", snapshot.exitCode, allocator);
  out.AddMember("stdout_tail", jsonString(snapshot.stdoutTail, allocator),
                allocator);
  out.AddMember("stderr_tail", jsonString(snapshot.stderrTail, allocator),
                allocator);
  out.AddMember("duration_ms", snapshot.durationMs, allocator);
  out.AddMember("system_id", jsonString(snapshot.systemId, allocator), allocator);
  out.AddMember("blocking", snapshot.blocking, allocator);
  return out;
}

ProcessSnapshot processSnapshotFromJson(const rapidjson::Value &value) {
  ProcessSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    snapshot.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("process_id") && value["process_id"].IsString()) {
    snapshot.processId = value["process_id"].GetString();
  }
  if (value.HasMember("tool_call_id") && value["tool_call_id"].IsString()) {
    snapshot.toolCallId = value["tool_call_id"].GetString();
  }
  if (value.HasMember("running") && value["running"].IsBool()) {
    snapshot.running = value["running"].GetBool();
  }
  if (value.HasMember("exit_code") && value["exit_code"].IsInt()) {
    snapshot.exitCode = value["exit_code"].GetInt();
  }
  if (value.HasMember("stdout_tail") && value["stdout_tail"].IsString()) {
    snapshot.stdoutTail = value["stdout_tail"].GetString();
  }
  if (value.HasMember("stderr_tail") && value["stderr_tail"].IsString()) {
    snapshot.stderrTail = value["stderr_tail"].GetString();
  }
  if (value.HasMember("duration_ms") && value["duration_ms"].IsNumber()) {
    snapshot.durationMs = value["duration_ms"].GetDouble();
  }
  if (value.HasMember("system_id") && value["system_id"].IsString()) {
    snapshot.systemId = value["system_id"].GetString();
  }
  if (value.HasMember("blocking") && value["blocking"].IsBool()) {
    snapshot.blocking = value["blocking"].GetBool();
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const ProcessRuntimeSummary &summary,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(summary.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(summary.agentId, allocator), allocator);
  rapidjson::Value active(rapidjson::kArrayType);
  for (const auto &processId : summary.activeProcessIds) {
    active.PushBack(jsonString(processId, allocator), allocator);
  }
  out.AddMember("active_process_ids", active, allocator);
  rapidjson::Value blocking(rapidjson::kArrayType);
  for (const auto &processId : summary.blockingProcessIds) {
    blocking.PushBack(jsonString(processId, allocator), allocator);
  }
  out.AddMember("blocking_process_ids", blocking, allocator);
  out.AddMember("running_count", static_cast<std::uint64_t>(summary.runningCount),
                allocator);
  out.AddMember("blocking_count",
                static_cast<std::uint64_t>(summary.blockingCount), allocator);
  return out;
}

ProcessRuntimeSummary processRuntimeSummaryFromJson(const rapidjson::Value &value) {
  ProcessRuntimeSummary summary;
  if (!value.IsObject()) {
    return summary;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    summary.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    summary.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("active_process_ids") &&
      value["active_process_ids"].IsArray()) {
    for (const auto &processId : value["active_process_ids"].GetArray()) {
      if (processId.IsString()) {
        summary.activeProcessIds.push_back(processId.GetString());
      }
    }
  }
  if (value.HasMember("blocking_process_ids") &&
      value["blocking_process_ids"].IsArray()) {
    for (const auto &processId : value["blocking_process_ids"].GetArray()) {
      if (processId.IsString()) {
        summary.blockingProcessIds.push_back(processId.GetString());
      }
    }
  }
  if (value.HasMember("running_count") && value["running_count"].IsUint64()) {
    summary.runningCount =
        static_cast<std::size_t>(value["running_count"].GetUint64());
  }
  if (value.HasMember("blocking_count") && value["blocking_count"].IsUint64()) {
    summary.blockingCount =
        static_cast<std::size_t>(value["blocking_count"].GetUint64());
  }
  return summary;
}

rapidjson::Value toJsonValue(const PermissionModeRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  return out;
}

PermissionModeRequest permissionModeRequestFromJson(const rapidjson::Value &value) {
  PermissionModeRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  return request;
}

rapidjson::Value toJsonValue(const PermissionModeUpdateRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out =
      toJsonValue(PermissionModeRequest{request.threadId}, allocator);
  out.AddMember(
      "permission_mode",
      jsonString(firmius::shared::permissionModeStorageString(request.permissionMode),
                 allocator),
      allocator);
  return out;
}

PermissionModeUpdateRequest
permissionModeUpdateRequestFromJson(const rapidjson::Value &value) {
  PermissionModeUpdateRequest request;
  if (!value.IsObject()) {
    return request;
  }
  request.threadId = permissionModeRequestFromJson(value).threadId;
  if (value.HasMember("permission_mode") &&
      value["permission_mode"].IsString()) {
    request.permissionMode = firmius::shared::permissionModeFromStorageString(
        value["permission_mode"].GetString());
  }
  return request;
}

rapidjson::Value toJsonValue(const PermissionResolveRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("request_id", jsonString(request.requestId, allocator), allocator);
  out.AddMember("response", static_cast<int>(request.response), allocator);
  return out;
}

PermissionResolveRequest
permissionResolveRequestFromJson(const rapidjson::Value &value) {
  PermissionResolveRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("request_id") && value["request_id"].IsString()) {
    request.requestId = value["request_id"].GetString();
  }
  if (value.HasMember("response") && value["response"].IsInt()) {
    request.response = static_cast<firmius::shared::PermissionResponse>(
        value["response"].GetInt());
  }
  return request;
}

rapidjson::Value toJsonValue(const PermissionQueueSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember(
      "permission_mode",
      jsonString(firmius::shared::permissionModeStorageString(snapshot.permissionMode),
                 allocator),
      allocator);
  rapidjson::Value pending(rapidjson::kArrayType);
  for (const auto &request : snapshot.pending) {
    rapidjson::Value item(rapidjson::kObjectType);
    item.AddMember("request_id", jsonString(request.requestId, allocator),
                   allocator);
    item.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
    item.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
    item.AddMember("message", jsonString(request.message, allocator), allocator);
    item.AddMember("tool_name", jsonString(request.toolName, allocator), allocator);
    pending.PushBack(item, allocator);
  }
  out.AddMember("pending", pending, allocator);
  return out;
}

PermissionQueueSnapshot
permissionQueueSnapshotFromJson(const rapidjson::Value &value) {
  PermissionQueueSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("permission_mode") &&
      value["permission_mode"].IsString()) {
    snapshot.permissionMode = firmius::shared::permissionModeFromStorageString(
        value["permission_mode"].GetString());
  }
  if (value.HasMember("pending") && value["pending"].IsArray()) {
    for (const auto &item : value["pending"].GetArray()) {
      if (!item.IsObject()) continue;
      firmius::shared::PermissionEscalationRequest req;
      if (item.HasMember("request_id") && item["request_id"].IsString()) {
        req.requestId = item["request_id"].GetString();
      }
      if (item.HasMember("thread_id") && item["thread_id"].IsString()) {
        req.threadId = item["thread_id"].GetString();
      }
      if (item.HasMember("agent_id") && item["agent_id"].IsString()) {
        req.agentId = item["agent_id"].GetString();
      }
      if (item.HasMember("message") && item["message"].IsString()) {
        req.message = item["message"].GetString();
      }
      if (item.HasMember("tool_name") && item["tool_name"].IsString()) {
        req.toolName = item["tool_name"].GetString();
      }
      snapshot.pending.push_back(std::move(req));
    }
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const ModelSwitchRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  out.AddMember("provider_id", jsonString(request.providerId, allocator),
                allocator);
  out.AddMember("model_id", jsonString(request.modelId, allocator), allocator);
  out.AddMember("variant_name", jsonString(request.variantName, allocator),
                allocator);
  return out;
}

ModelSwitchRequest modelSwitchRequestFromJson(const rapidjson::Value &value) {
  ModelSwitchRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("provider_id") && value["provider_id"].IsString()) {
    request.providerId = value["provider_id"].GetString();
  }
  if (value.HasMember("model_id") && value["model_id"].IsString()) {
    request.modelId = value["model_id"].GetString();
  }
  if (value.HasMember("variant_name") && value["variant_name"].IsString()) {
    request.variantName = value["variant_name"].GetString();
  }
  return request;
}

rapidjson::Value toJsonValue(const ModelCatalogSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value models(rapidjson::kArrayType);
  for (const auto &model : snapshot.models) {
    auto modelDoc = firmius::shared::toJson(model);
    rapidjson::Value modelValue(rapidjson::kObjectType);
    modelValue.CopyFrom(modelDoc, allocator);
    models.PushBack(modelValue, allocator);
  }
  out.AddMember("models", models, allocator);
  rapidjson::Value fetching(rapidjson::kArrayType);
  for (const auto &providerId : snapshot.fetchingProviders) {
    fetching.PushBack(jsonString(providerId, allocator), allocator);
  }
  out.AddMember("fetching_providers", fetching, allocator);
  out.AddMember("loaded", snapshot.loaded, allocator);
  out.AddMember("loading", snapshot.loading, allocator);
  return out;
}

ModelCatalogSnapshot modelCatalogSnapshotFromJson(const rapidjson::Value &value) {
  ModelCatalogSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("models") && value["models"].IsArray()) {
    for (const auto &model : value["models"].GetArray()) {
      snapshot.models.push_back(firmius::shared::modelInfoFromJsonValue(model));
    }
  }
  if (value.HasMember("fetching_providers") &&
      value["fetching_providers"].IsArray()) {
    for (const auto &providerId : value["fetching_providers"].GetArray()) {
      if (providerId.IsString()) {
        snapshot.fetchingProviders.push_back(providerId.GetString());
      }
    }
  }
  if (value.HasMember("loaded") && value["loaded"].IsBool()) {
    snapshot.loaded = value["loaded"].GetBool();
  }
  if (value.HasMember("loading") && value["loading"].IsBool()) {
    snapshot.loading = value["loading"].GetBool();
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const ProviderCatalogSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value providers(rapidjson::kArrayType);
  for (const auto &provider : snapshot.providers) {
    rapidjson::Value item(rapidjson::kObjectType);
    item.AddMember("id", jsonString(provider.id, allocator), allocator);
    item.AddMember("kind", jsonString(provider.kind, allocator), allocator);
    item.AddMember("auth_mode", jsonString(provider.authMode, allocator),
                   allocator);
    item.AddMember("display_name", jsonString(provider.displayName, allocator),
                   allocator);
    item.AddMember("enabled", provider.enabled, allocator);
    item.AddMember("configured", provider.configured, allocator);
    item.AddMember("custom", provider.custom, allocator);
    providers.PushBack(item, allocator);
  }
  out.AddMember("providers", providers, allocator);
  return out;
}

ProviderCatalogSnapshot
providerCatalogSnapshotFromJson(const rapidjson::Value &value) {
  ProviderCatalogSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("providers") && value["providers"].IsArray()) {
    for (const auto &item : value["providers"].GetArray()) {
      if (!item.IsObject()) {
        continue;
      }
      ProviderProfileSnapshot provider;
      if (item.HasMember("id") && item["id"].IsString()) {
        provider.id = item["id"].GetString();
      }
      if (item.HasMember("kind") && item["kind"].IsString()) {
        provider.kind = item["kind"].GetString();
      }
      if (item.HasMember("auth_mode") && item["auth_mode"].IsString()) {
        provider.authMode = item["auth_mode"].GetString();
      }
      if (item.HasMember("display_name") && item["display_name"].IsString()) {
        provider.displayName = item["display_name"].GetString();
      }
      if (item.HasMember("enabled") && item["enabled"].IsBool()) {
        provider.enabled = item["enabled"].GetBool();
      }
      if (item.HasMember("configured") && item["configured"].IsBool()) {
        provider.configured = item["configured"].GetBool();
      }
      if (item.HasMember("custom") && item["custom"].IsBool()) {
        provider.custom = item["custom"].GetBool();
      }
      if (item.HasMember("profile") && item["profile"].IsObject()) {
        provider.profile = providerProfileConfigFromJson(item["profile"]);
      }
      snapshot.providers.push_back(std::move(provider));
    }
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const ProviderProfilesUpdateRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value providers(rapidjson::kObjectType);
  for (const auto &[id, profile] : request.providers) {
    providers.AddMember(jsonString(id, allocator),
                        toJsonValue(profile, allocator), allocator);
  }
  out.AddMember("providers", providers, allocator);
  return out;
}

ProviderProfilesUpdateRequest
providerProfilesUpdateRequestFromJson(const rapidjson::Value &value) {
  ProviderProfilesUpdateRequest request;
  if (!value.IsObject() || !value.HasMember("providers") ||
      !value["providers"].IsObject()) {
    return request;
  }
  for (auto it = value["providers"].MemberBegin();
       it != value["providers"].MemberEnd(); ++it) {
    request.providers[it->name.GetString()] =
        providerProfileConfigFromJson(it->value);
  }
  return request;
}

rapidjson::Value toJsonValue(const UserConfigSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("config", toJsonValue(snapshot.config, allocator), allocator);
  return out;
}

UserConfigSnapshot userConfigSnapshotFromJson(const rapidjson::Value &value) {
  UserConfigSnapshot snapshot;
  if (!value.IsObject() || !value.HasMember("config")) {
    return snapshot;
  }
  snapshot.config = userConfigFromJson(value["config"]);
  return snapshot;
}

rapidjson::Value toJsonValue(const ConfigUpdateRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("config", toJsonValue(request.config, allocator), allocator);
  return out;
}

ConfigUpdateRequest configUpdateRequestFromJson(const rapidjson::Value &value) {
  ConfigUpdateRequest request;
  if (!value.IsObject() || !value.HasMember("config")) {
    return request;
  }
  request.config = userConfigFromJson(value["config"]);
  return request;
}

rapidjson::Value toJsonValue(const HistoryGetRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  out.AddMember("limit", request.limit, allocator);
  return out;
}

HistoryGetRequest historyGetRequestFromJson(const rapidjson::Value &value) {
  HistoryGetRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("limit") && value["limit"].IsInt()) {
    request.limit = value["limit"].GetInt();
  }
  return request;
}

rapidjson::Value toJsonValue(const HistoryUndoRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  out.AddMember("count", request.count, allocator);
  return out;
}

HistoryUndoRequest historyUndoRequestFromJson(const rapidjson::Value &value) {
  HistoryUndoRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("count") && value["count"].IsInt()) {
    request.count = value["count"].GetInt();
  }
  return request;
}

rapidjson::Value toJsonValue(const HistoryRedoRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  out.AddMember("undo_action_id", jsonString(request.undoActionId, allocator),
                allocator);
  return out;
}

HistoryRedoRequest historyRedoRequestFromJson(const rapidjson::Value &value) {
  HistoryRedoRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("undo_action_id") &&
      value["undo_action_id"].IsString()) {
    request.undoActionId = value["undo_action_id"].GetString();
  }
  return request;
}

rapidjson::Value toJsonTranscriptUndoActions(
    const std::vector<firmius::shared::TranscriptUndoAction> &actions,
    rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &action : actions) {
    auto doc = firmius::shared::toJson(action);
    rapidjson::Value value(rapidjson::kObjectType);
    value.CopyFrom(doc, allocator);
    out.PushBack(value, allocator);
  }
  return out;
}

std::vector<firmius::shared::TranscriptUndoAction>
transcriptUndoActionsFromJson(const rapidjson::Value &value) {
  std::vector<firmius::shared::TranscriptUndoAction> actions;
  if (!value.IsArray()) {
    return actions;
  }
  for (const auto &entry : value.GetArray()) {
    actions.push_back(firmius::shared::transcriptUndoActionFromJson(entry));
  }
  return actions;
}

rapidjson::Value toJsonTranscriptRedoEligibilities(
    const std::vector<firmius::shared::TranscriptRedoEligibility> &eligibilities,
    rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &eligibility : eligibilities) {
    auto doc = firmius::shared::toJson(eligibility);
    rapidjson::Value value(rapidjson::kObjectType);
    value.CopyFrom(doc, allocator);
    out.PushBack(value, allocator);
  }
  return out;
}

std::vector<firmius::shared::TranscriptRedoEligibility>
transcriptRedoEligibilitiesFromJson(const rapidjson::Value &value) {
  std::vector<firmius::shared::TranscriptRedoEligibility> eligibilities;
  if (!value.IsArray()) {
    return eligibilities;
  }
  for (const auto &entry : value.GetArray()) {
    eligibilities.push_back(
        firmius::shared::transcriptRedoEligibilityFromJson(entry));
  }
  return eligibilities;
}

rapidjson::Value toJsonValue(const HistorySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(snapshot.agentId, allocator), allocator);
  out.AddMember("recent_undo_actions",
                toJsonTranscriptUndoActions(snapshot.recentUndoActions, allocator),
                allocator);
  out.AddMember(
      "redo_eligibilities",
      toJsonTranscriptRedoEligibilities(snapshot.redoEligibilities, allocator),
      allocator);
  out.AddMember("latest_redo_eligible_undo_action_id",
                jsonString(snapshot.latestRedoEligibleUndoActionId, allocator),
                allocator);
  return out;
}

HistorySnapshot historySnapshotFromJson(const rapidjson::Value &value) {
  HistorySnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    snapshot.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("recent_undo_actions")) {
    snapshot.recentUndoActions =
        transcriptUndoActionsFromJson(value["recent_undo_actions"]);
  }
  if (value.HasMember("redo_eligibilities")) {
    snapshot.redoEligibilities =
        transcriptRedoEligibilitiesFromJson(value["redo_eligibilities"]);
  }
  if (value.HasMember("latest_redo_eligible_undo_action_id") &&
      value["latest_redo_eligible_undo_action_id"].IsString()) {
    snapshot.latestRedoEligibleUndoActionId =
        value["latest_redo_eligible_undo_action_id"].GetString();
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const HistoryMutationResult &result,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("applied", result.applied, allocator);
  out.AddMember("thread_id", jsonString(result.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(result.agentId, allocator), allocator);
  if (result.undoAction.has_value()) {
    auto doc = firmius::shared::toJson(result.undoAction.value());
    rapidjson::Value value(rapidjson::kObjectType);
    value.CopyFrom(doc, allocator);
    out.AddMember("undo_action", value, allocator);
  } else {
    out.AddMember("undo_action", rapidjson::Value(rapidjson::kNullType),
                  allocator);
  }
  if (result.redoAction.has_value()) {
    rapidjson::Value value(rapidjson::kObjectType);
    value.AddMember("redo_action_id",
                    jsonString(result.redoAction->redoActionId, allocator),
                    allocator);
    value.AddMember("undo_action_id",
                    jsonString(result.redoAction->undoActionId, allocator),
                    allocator);
    value.AddMember("thread_id",
                    jsonString(result.redoAction->threadId, allocator), allocator);
    value.AddMember("agent_id",
                    jsonString(result.redoAction->agentId, allocator), allocator);
    value.AddMember("created_at", result.redoAction->createdAt, allocator);
    value.AddMember("result_json",
                    jsonString(result.redoAction->resultJson, allocator), allocator);
    out.AddMember("redo_action", value, allocator);
  } else {
    out.AddMember("redo_action", rapidjson::Value(rapidjson::kNullType),
                  allocator);
  }
  if (result.redoEligibility.has_value()) {
    auto doc = firmius::shared::toJson(result.redoEligibility.value());
    rapidjson::Value value(rapidjson::kObjectType);
    value.CopyFrom(doc, allocator);
    out.AddMember("redo_eligibility", value, allocator);
  } else {
    out.AddMember("redo_eligibility", rapidjson::Value(rapidjson::kNullType),
                  allocator);
  }
  out.AddMember("message", jsonString(result.message, allocator), allocator);
  out.AddMember("history", toJsonValue(result.history, allocator), allocator);
  return out;
}

HistoryMutationResult historyMutationResultFromJson(const rapidjson::Value &value) {
  HistoryMutationResult result;
  if (!value.IsObject()) {
    return result;
  }
  if (value.HasMember("applied") && value["applied"].IsBool()) {
    result.applied = value["applied"].GetBool();
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    result.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    result.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("undo_action") && value["undo_action"].IsObject()) {
    result.undoAction =
        firmius::shared::transcriptUndoActionFromJson(value["undo_action"]);
  }
  if (value.HasMember("redo_action") && value["redo_action"].IsObject()) {
    firmius::shared::TranscriptRedoAction action;
    const auto &redo = value["redo_action"];
    action.redoActionId =
        redo.HasMember("redo_action_id") && redo["redo_action_id"].IsString()
            ? redo["redo_action_id"].GetString()
            : "";
    action.undoActionId =
        redo.HasMember("undo_action_id") && redo["undo_action_id"].IsString()
            ? redo["undo_action_id"].GetString()
            : "";
    action.threadId = redo.HasMember("thread_id") && redo["thread_id"].IsString()
                          ? redo["thread_id"].GetString()
                          : "";
    action.agentId = redo.HasMember("agent_id") && redo["agent_id"].IsString()
                         ? redo["agent_id"].GetString()
                         : "";
    action.createdAt = redo.HasMember("created_at") &&
                               redo["created_at"].IsUint64()
                           ? redo["created_at"].GetUint64()
                           : 0;
    action.resultJson =
        redo.HasMember("result_json") && redo["result_json"].IsString()
            ? redo["result_json"].GetString()
            : "";
    result.redoAction = action;
  }
  if (value.HasMember("redo_eligibility") &&
      value["redo_eligibility"].IsObject()) {
    result.redoEligibility = firmius::shared::transcriptRedoEligibilityFromJson(
        value["redo_eligibility"]);
  }
  if (value.HasMember("message") && value["message"].IsString()) {
    result.message = value["message"].GetString();
  }
  if (value.HasMember("history") && value["history"].IsObject()) {
    result.history = historySnapshotFromJson(value["history"]);
  }
  return result;
}

rapidjson::Value toJsonValue(const RouterConfigSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value categories(rapidjson::kObjectType);
  for (const auto &[name, route] : snapshot.categories) {
    categories.AddMember(jsonString(name, allocator),
                         toJsonValue(route, allocator), allocator);
  }
  out.AddMember("categories", categories, allocator);
  out.AddMember("default_route_category",
                jsonString(snapshot.defaultRouteCategory, allocator), allocator);
  out.AddMember("enable_subagent_route_fallback",
                snapshot.enableSubagentRouteFallback, allocator);
  rapidjson::Value fallbackOrder(rapidjson::kArrayType);
  for (const auto &category : snapshot.subagentRouteFallbackOrder) {
    fallbackOrder.PushBack(jsonString(category, allocator), allocator);
  }
  out.AddMember("subagent_route_fallback_order", fallbackOrder, allocator);
  return out;
}

RouterConfigSnapshot routerConfigSnapshotFromJson(const rapidjson::Value &value) {
  RouterConfigSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("categories") && value["categories"].IsObject()) {
    for (auto it = value["categories"].MemberBegin();
         it != value["categories"].MemberEnd(); ++it) {
      snapshot.categories[it->name.GetString()] =
          modelRouteCategoryFromJson(it->value);
    }
  }
  if (value.HasMember("default_route_category") &&
      value["default_route_category"].IsString()) {
    snapshot.defaultRouteCategory = value["default_route_category"].GetString();
  }
  if (value.HasMember("enable_subagent_route_fallback") &&
      value["enable_subagent_route_fallback"].IsBool()) {
    snapshot.enableSubagentRouteFallback =
        value["enable_subagent_route_fallback"].GetBool();
  }
  if (value.HasMember("subagent_route_fallback_order") &&
      value["subagent_route_fallback_order"].IsArray()) {
    for (const auto &item : value["subagent_route_fallback_order"].GetArray()) {
      if (item.IsString()) {
        snapshot.subagentRouteFallbackOrder.push_back(item.GetString());
      }
    }
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const RouterConfigUpdateRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  return toJsonValue(
      RouterConfigSnapshot{request.categories, request.defaultRouteCategory,
                           request.enableSubagentRouteFallback,
                           request.subagentRouteFallbackOrder},
      allocator);
}

RouterConfigUpdateRequest
routerConfigUpdateRequestFromJson(const rapidjson::Value &value) {
  RouterConfigUpdateRequest request;
  auto snapshot = routerConfigSnapshotFromJson(value);
  request.categories = snapshot.categories;
  request.defaultRouteCategory = snapshot.defaultRouteCategory;
  request.enableSubagentRouteFallback =
      snapshot.enableSubagentRouteFallback;
  request.subagentRouteFallbackOrder = snapshot.subagentRouteFallbackOrder;
  return request;
}

rapidjson::Value toJsonValue(const PurposesConfigSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value routes(rapidjson::kObjectType);
  for (const auto &[purpose, category] : snapshot.purposeRoutes) {
    routes.AddMember(jsonString(purpose, allocator),
                     jsonString(category, allocator), allocator);
  }
  out.AddMember("purpose_routes", routes, allocator);
  return out;
}

PurposesConfigSnapshot
purposesConfigSnapshotFromJson(const rapidjson::Value &value) {
  PurposesConfigSnapshot snapshot;
  if (!value.IsObject() || !value.HasMember("purpose_routes") ||
      !value["purpose_routes"].IsObject()) {
    return snapshot;
  }
  for (auto it = value["purpose_routes"].MemberBegin();
       it != value["purpose_routes"].MemberEnd(); ++it) {
    if (it->value.IsString()) {
      snapshot.purposeRoutes[it->name.GetString()] = it->value.GetString();
    }
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const PurposesConfigUpdateRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  return toJsonValue(PurposesConfigSnapshot{request.purposeRoutes}, allocator);
}

PurposesConfigUpdateRequest
purposesConfigUpdateRequestFromJson(const rapidjson::Value &value) {
  PurposesConfigUpdateRequest request;
  request.purposeRoutes = purposesConfigSnapshotFromJson(value).purposeRoutes;
  return request;
}

rapidjson::Value toJsonValue(const RollingMemoryConfigSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("rolling_memory",
                toJsonValue(snapshot.rollingMemory, allocator), allocator);
  return out;
}

RollingMemoryConfigSnapshot
rollingMemoryConfigSnapshotFromJson(const rapidjson::Value &value) {
  RollingMemoryConfigSnapshot snapshot;
  if (!value.IsObject() || !value.HasMember("rolling_memory")) {
    return snapshot;
  }
  snapshot.rollingMemory = rollingMemoryConfigFromJson(value["rolling_memory"]);
  return snapshot;
}

rapidjson::Value toJsonValue(
    const RollingMemoryConfigUpdateRequest &request,
    rapidjson::Document::AllocatorType &allocator) {
  return toJsonValue(RollingMemoryConfigSnapshot{request.rollingMemory},
                     allocator);
}

RollingMemoryConfigUpdateRequest
rollingMemoryConfigUpdateRequestFromJson(const rapidjson::Value &value) {
  RollingMemoryConfigUpdateRequest request;
  request.rollingMemory =
      rollingMemoryConfigSnapshotFromJson(value).rollingMemory;
  return request;
}

rapidjson::Value toJsonValue(const McpConfigSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value servers(rapidjson::kObjectType);
  for (const auto &[name, server] : snapshot.servers) {
    servers.AddMember(jsonString(name, allocator),
                      toJsonValue(server, allocator), allocator);
  }
  out.AddMember("servers", servers, allocator);
  return out;
}

McpConfigSnapshot mcpConfigSnapshotFromJson(const rapidjson::Value &value) {
  McpConfigSnapshot snapshot;
  if (!value.IsObject() || !value.HasMember("servers") ||
      !value["servers"].IsObject()) {
    return snapshot;
  }
  for (auto it = value["servers"].MemberBegin();
       it != value["servers"].MemberEnd(); ++it) {
    snapshot.servers[it->name.GetString()] = mcpServerConfigFromJson(it->value);
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const McpConfigUpdateRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  return toJsonValue(McpConfigSnapshot{request.servers}, allocator);
}

McpConfigUpdateRequest mcpConfigUpdateRequestFromJson(const rapidjson::Value &value) {
  McpConfigUpdateRequest request;
  request.servers = mcpConfigSnapshotFromJson(value).servers;
  return request;
}

rapidjson::Value toJsonValue(const AccountsRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("provider_id", jsonString(request.providerId, allocator),
                allocator);
  return out;
}

AccountsRequest accountsRequestFromJson(const rapidjson::Value &value) {
  AccountsRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("provider_id") && value["provider_id"].IsString()) {
    request.providerId = value["provider_id"].GetString();
  }
  return request;
}

rapidjson::Value toJsonValue(const AccountDeleteRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out = toJsonValue(AccountsRequest{request.providerId}, allocator);
  out.AddMember("identifier", jsonString(request.identifier, allocator), allocator);
  return out;
}

AccountDeleteRequest accountDeleteRequestFromJson(const rapidjson::Value &value) {
  AccountDeleteRequest request;
  if (!value.IsObject()) {
    return request;
  }
  request.providerId = accountsRequestFromJson(value).providerId;
  if (value.HasMember("identifier") && value["identifier"].IsString()) {
    request.identifier = value["identifier"].GetString();
  }
  return request;
}

rapidjson::Value toJsonValue(const AccountSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("provider_id", jsonString(snapshot.providerId, allocator),
                allocator);
  out.AddMember("identifier", jsonString(snapshot.identifier, allocator),
                allocator);
  out.AddMember("rate_limited", snapshot.rateLimited, allocator);
  out.AddMember("backoff_until", snapshot.backoffUntil, allocator);
  rapidjson::Value metadata(rapidjson::kObjectType);
  for (const auto &[key, value] : snapshot.metadata) {
    metadata.AddMember(jsonString(key, allocator), jsonString(value, allocator),
                       allocator);
  }
  out.AddMember("metadata", metadata, allocator);
  return out;
}

AccountSnapshot accountSnapshotFromJson(const rapidjson::Value &value) {
  AccountSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("provider_id") && value["provider_id"].IsString()) {
    snapshot.providerId = value["provider_id"].GetString();
  }
  if (value.HasMember("identifier") && value["identifier"].IsString()) {
    snapshot.identifier = value["identifier"].GetString();
  }
  if (value.HasMember("rate_limited") && value["rate_limited"].IsBool()) {
    snapshot.rateLimited = value["rate_limited"].GetBool();
  }
  if (value.HasMember("backoff_until") && value["backoff_until"].IsInt64()) {
    snapshot.backoffUntil = value["backoff_until"].GetInt64();
  }
  if (value.HasMember("metadata") && value["metadata"].IsObject()) {
    for (auto it = value["metadata"].MemberBegin();
         it != value["metadata"].MemberEnd(); ++it) {
      if (it->name.IsString() && it->value.IsString()) {
        snapshot.metadata[it->name.GetString()] = it->value.GetString();
      }
    }
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const QuotasRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  return toJsonValue(AccountsRequest{request.providerId}, allocator);
}

QuotasRequest quotasRequestFromJson(const rapidjson::Value &value) {
  QuotasRequest request;
  request.providerId = accountsRequestFromJson(value).providerId;
  return request;
}

rapidjson::Value toJsonValue(const QuotaSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("provider_id", jsonString(snapshot.providerId, allocator),
                allocator);
  rapidjson::Value buckets(rapidjson::kObjectType);
  for (const auto &[group, bucketList] : snapshot.buckets) {
    rapidjson::Value entries(rapidjson::kArrayType);
    for (const auto &bucket : bucketList) {
      rapidjson::Value item(rapidjson::kObjectType);
      item.AddMember("name", jsonString(bucket.name, allocator), allocator);
      item.AddMember("remaining_fraction", bucket.remainingFraction, allocator);
      item.AddMember("reset_time", jsonString(bucket.resetTime, allocator),
                     allocator);
      item.AddMember("note", jsonString(bucket.note, allocator), allocator);
      entries.PushBack(item, allocator);
    }
    buckets.AddMember(jsonString(group, allocator), entries, allocator);
  }
  out.AddMember("buckets", buckets, allocator);
  return out;
}

QuotaSnapshot quotaSnapshotFromJson(const rapidjson::Value &value) {
  QuotaSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("provider_id") && value["provider_id"].IsString()) {
    snapshot.providerId = value["provider_id"].GetString();
  }
  if (value.HasMember("buckets") && value["buckets"].IsObject()) {
    for (auto it = value["buckets"].MemberBegin();
         it != value["buckets"].MemberEnd(); ++it) {
      if (!it->value.IsArray()) continue;
      std::vector<firmius::shared::QuotaBucket> bucketList;
      for (const auto &item : it->value.GetArray()) {
        if (!item.IsObject()) continue;
        firmius::shared::QuotaBucket bucket;
        if (item.HasMember("name") && item["name"].IsString()) {
          bucket.name = item["name"].GetString();
        }
        if (item.HasMember("remaining_fraction") &&
            item["remaining_fraction"].IsFloat()) {
          bucket.remainingFraction = item["remaining_fraction"].GetFloat();
        }
        if (item.HasMember("reset_time") && item["reset_time"].IsString()) {
          bucket.resetTime = item["reset_time"].GetString();
        }
        if (item.HasMember("note") && item["note"].IsString()) {
          bucket.note = item["note"].GetString();
        }
        bucketList.push_back(std::move(bucket));
      }
      snapshot.buckets[it->name.GetString()] = std::move(bucketList);
    }
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const HooksStateRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  out.AddMember("hook_id", jsonString(request.hookId, allocator), allocator);
  out.AddMember("limit", request.limit, allocator);
  return out;
}

HooksStateRequest hooksStateRequestFromJson(const rapidjson::Value &value) {
  HooksStateRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("hook_id") && value["hook_id"].IsString()) {
    request.hookId = value["hook_id"].GetString();
  }
  if (value.HasMember("limit") && value["limit"].IsInt()) {
    request.limit = value["limit"].GetInt();
  }
  return request;
}

rapidjson::Value toJsonValue(const HookStatusSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value hookIds(rapidjson::kArrayType);
  for (const auto &hookId : snapshot.hookIds) {
    hookIds.PushBack(jsonString(hookId, allocator), allocator);
  }
  out.AddMember("hook_ids", hookIds, allocator);
  rapidjson::Value hookDirs(rapidjson::kArrayType);
  for (const auto &hookDir : snapshot.hookDirs) {
    hookDirs.PushBack(jsonString(hookDir, allocator), allocator);
  }
  out.AddMember("hook_dirs", hookDirs, allocator);
  out.AddMember("hook_count", static_cast<std::uint64_t>(snapshot.hookCount),
                allocator);
  return out;
}

HookStatusSnapshot hookStatusSnapshotFromJson(const rapidjson::Value &value) {
  HookStatusSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("hook_ids") && value["hook_ids"].IsArray()) {
    for (const auto &id : value["hook_ids"].GetArray()) {
      if (id.IsString()) snapshot.hookIds.push_back(id.GetString());
    }
  }
  if (value.HasMember("hook_dirs") && value["hook_dirs"].IsArray()) {
    for (const auto &dir : value["hook_dirs"].GetArray()) {
      if (dir.IsString()) snapshot.hookDirs.push_back(dir.GetString());
    }
  }
  if (value.HasMember("hook_count") && value["hook_count"].IsUint64()) {
    snapshot.hookCount = value["hook_count"].GetUint64();
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const HookStateSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(snapshot.agentId, allocator), allocator);
  out.AddMember("hook_id", jsonString(snapshot.hookId, allocator), allocator);
  out.AddMember("snapshot_json", jsonString(snapshot.snapshotJson, allocator),
                allocator);
  rapidjson::Value activity(rapidjson::kArrayType);
  for (const auto &entry : snapshot.recentActivity) {
    rapidjson::Value item(rapidjson::kObjectType);
    item.AddMember("hook_id", jsonString(entry.hookId, allocator), allocator);
    item.AddMember("thread_id", jsonString(entry.threadId, allocator), allocator);
    item.AddMember("agent_id", jsonString(entry.agentId, allocator), allocator);
    item.AddMember("event_name", jsonString(entry.eventName, allocator),
                   allocator);
    item.AddMember("decision", jsonString(entry.decision, allocator), allocator);
    item.AddMember("outcome_label", jsonString(entry.outcomeLabel, allocator),
                   allocator);
    item.AddMember("block_reason", jsonString(entry.blockReason, allocator),
                   allocator);
    item.AddMember("status_line", jsonString(entry.statusLine, allocator),
                   allocator);
    item.AddMember("timestamp_ms", entry.timestampMs, allocator);
    item.AddMember("state_write_count", entry.stateWriteCount, allocator);
    activity.PushBack(item, allocator);
  }
  out.AddMember("recent_activity", activity, allocator);
  rapidjson::Value statusLines(rapidjson::kArrayType);
  for (const auto &line : snapshot.currentStatusLines) {
    statusLines.PushBack(jsonString(line, allocator), allocator);
  }
  out.AddMember("current_status_lines", statusLines, allocator);
  rapidjson::Value blocking(rapidjson::kArrayType);
  for (const auto &reason : snapshot.blockingReasons) {
    blocking.PushBack(jsonString(reason, allocator), allocator);
  }
  out.AddMember("blocking_reasons", blocking, allocator);
  out.AddMember("latest_decision", jsonString(snapshot.latestDecision, allocator),
                allocator);
  out.AddMember("latest_outcome_label",
                jsonString(snapshot.latestOutcomeLabel, allocator), allocator);
  out.AddMember("latest_status_line",
                jsonString(snapshot.latestStatusLine, allocator), allocator);
  out.AddMember("latest_timestamp_ms", snapshot.latestTimestampMs, allocator);
  out.AddMember("total_state_write_count", snapshot.totalStateWriteCount,
                allocator);
  return out;
}

rapidjson::Value toJsonValue(const HooksRecentActivitySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value activities(rapidjson::kArrayType);
  for (const auto &entry : snapshot.activities) {
    rapidjson::Value item(rapidjson::kObjectType);
    item.AddMember("hook_id", jsonString(entry.hookId, allocator), allocator);
    item.AddMember("thread_id", jsonString(entry.threadId, allocator), allocator);
    item.AddMember("agent_id", jsonString(entry.agentId, allocator), allocator);
    item.AddMember("event_name", jsonString(entry.eventName, allocator),
                   allocator);
    item.AddMember("decision", jsonString(entry.decision, allocator), allocator);
    item.AddMember("outcome_label", jsonString(entry.outcomeLabel, allocator),
                   allocator);
    item.AddMember("block_reason", jsonString(entry.blockReason, allocator),
                   allocator);
    item.AddMember("status_line", jsonString(entry.statusLine, allocator),
                   allocator);
    item.AddMember("timestamp_ms", entry.timestampMs, allocator);
    item.AddMember("state_write_count", entry.stateWriteCount, allocator);
    activities.PushBack(item, allocator);
  }
  out.AddMember("activities", activities, allocator);
  return out;
}

HooksRecentActivitySnapshot
hooksRecentActivitySnapshotFromJson(const rapidjson::Value &value) {
  HooksRecentActivitySnapshot snapshot;
  if (!value.IsObject() || !value.HasMember("activities") ||
      !value["activities"].IsArray()) {
    return snapshot;
  }
  for (const auto &entry : value["activities"].GetArray()) {
    if (!entry.IsObject()) {
      continue;
    }
    HookActivitySnapshot activity;
    if (entry.HasMember("hook_id") && entry["hook_id"].IsString()) {
      activity.hookId = entry["hook_id"].GetString();
    }
    if (entry.HasMember("thread_id") && entry["thread_id"].IsString()) {
      activity.threadId = entry["thread_id"].GetString();
    }
    if (entry.HasMember("agent_id") && entry["agent_id"].IsString()) {
      activity.agentId = entry["agent_id"].GetString();
    }
    if (entry.HasMember("event_name") && entry["event_name"].IsString()) {
      activity.eventName = entry["event_name"].GetString();
    }
    if (entry.HasMember("decision") && entry["decision"].IsString()) {
      activity.decision = entry["decision"].GetString();
    }
    if (entry.HasMember("outcome_label") &&
        entry["outcome_label"].IsString()) {
      activity.outcomeLabel = entry["outcome_label"].GetString();
    }
    if (entry.HasMember("block_reason") &&
        entry["block_reason"].IsString()) {
      activity.blockReason = entry["block_reason"].GetString();
    }
    if (entry.HasMember("status_line") && entry["status_line"].IsString()) {
      activity.statusLine = entry["status_line"].GetString();
    }
    if (entry.HasMember("timestamp_ms") && entry["timestamp_ms"].IsUint64()) {
      activity.timestampMs = entry["timestamp_ms"].GetUint64();
    }
    if (entry.HasMember("state_write_count") &&
        entry["state_write_count"].IsInt()) {
      activity.stateWriteCount = entry["state_write_count"].GetInt();
    }
    snapshot.activities.push_back(std::move(activity));
  }
  return snapshot;
}
HookStateSnapshot hookStateSnapshotFromJson(const rapidjson::Value &value) {
  HookStateSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    snapshot.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("hook_id") && value["hook_id"].IsString()) {
    snapshot.hookId = value["hook_id"].GetString();
  }
  if (value.HasMember("snapshot_json") && value["snapshot_json"].IsString()) {
    snapshot.snapshotJson = value["snapshot_json"].GetString();
  }
  if (value.HasMember("recent_activity") && value["recent_activity"].IsArray()) {
    for (const auto &entry : value["recent_activity"].GetArray()) {
      if (!entry.IsObject()) {
        continue;
      }
      HookActivitySnapshot activity;
      if (entry.HasMember("hook_id") && entry["hook_id"].IsString()) {
        activity.hookId = entry["hook_id"].GetString();
      }
      if (entry.HasMember("thread_id") && entry["thread_id"].IsString()) {
        activity.threadId = entry["thread_id"].GetString();
      }
      if (entry.HasMember("agent_id") && entry["agent_id"].IsString()) {
        activity.agentId = entry["agent_id"].GetString();
      }
      if (entry.HasMember("event_name") && entry["event_name"].IsString()) {
        activity.eventName = entry["event_name"].GetString();
      }
      if (entry.HasMember("decision") && entry["decision"].IsString()) {
        activity.decision = entry["decision"].GetString();
      }
      if (entry.HasMember("outcome_label") &&
          entry["outcome_label"].IsString()) {
        activity.outcomeLabel = entry["outcome_label"].GetString();
      }
      if (entry.HasMember("block_reason") &&
          entry["block_reason"].IsString()) {
        activity.blockReason = entry["block_reason"].GetString();
      }
      if (entry.HasMember("status_line") && entry["status_line"].IsString()) {
        activity.statusLine = entry["status_line"].GetString();
      }
      if (entry.HasMember("timestamp_ms") && entry["timestamp_ms"].IsUint64()) {
        activity.timestampMs = entry["timestamp_ms"].GetUint64();
      }
      if (entry.HasMember("state_write_count") &&
          entry["state_write_count"].IsInt()) {
        activity.stateWriteCount = entry["state_write_count"].GetInt();
      }
      snapshot.recentActivity.push_back(std::move(activity));
    }
  }
  if (value.HasMember("current_status_lines") &&
      value["current_status_lines"].IsArray()) {
    for (const auto &line : value["current_status_lines"].GetArray()) {
      if (line.IsString()) {
        snapshot.currentStatusLines.push_back(line.GetString());
      }
    }
  }
  if (value.HasMember("blocking_reasons") &&
      value["blocking_reasons"].IsArray()) {
    for (const auto &reason : value["blocking_reasons"].GetArray()) {
      if (reason.IsString()) {
        snapshot.blockingReasons.push_back(reason.GetString());
      }
    }
  }
  if (value.HasMember("latest_decision") &&
      value["latest_decision"].IsString()) {
    snapshot.latestDecision = value["latest_decision"].GetString();
  }
  if (value.HasMember("latest_outcome_label") &&
      value["latest_outcome_label"].IsString()) {
    snapshot.latestOutcomeLabel = value["latest_outcome_label"].GetString();
  }
  if (value.HasMember("latest_status_line") &&
      value["latest_status_line"].IsString()) {
    snapshot.latestStatusLine = value["latest_status_line"].GetString();
  }
  if (value.HasMember("latest_timestamp_ms") &&
      value["latest_timestamp_ms"].IsUint64()) {
    snapshot.latestTimestampMs = value["latest_timestamp_ms"].GetUint64();
  }
  if (value.HasMember("total_state_write_count") &&
      value["total_state_write_count"].IsInt()) {
    snapshot.totalStateWriteCount = value["total_state_write_count"].GetInt();
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const PactsListRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  return out;
}

PactsListRequest pactsListRequestFromJson(const rapidjson::Value &value) {
  PactsListRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  return request;
}

rapidjson::Value toJsonValue(const PactsGetRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("pact_id", jsonString(request.pactId, allocator), allocator);
  return out;
}

PactsGetRequest pactsGetRequestFromJson(const rapidjson::Value &value) {
  PactsGetRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("pact_id") && value["pact_id"].IsString()) {
    request.pactId = value["pact_id"].GetString();
  }
  return request;
}

rapidjson::Value toJsonValue(const PactHistoryEntrySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("iteration", snapshot.iteration, allocator);
  out.AddMember("validator", jsonString(snapshot.validator, allocator), allocator);
  out.AddMember("validator_agent_id",
                jsonString(snapshot.validatorAgentId, allocator), allocator);
  out.AddMember("verdict", jsonString(snapshot.verdict, allocator), allocator);
  out.AddMember("suggestion", jsonString(snapshot.suggestion, allocator), allocator);
  out.AddMember("evidence_json", jsonString(snapshot.evidenceJson, allocator),
                allocator);
  return out;
}

PactHistoryEntrySnapshot
pactHistoryEntrySnapshotFromJson(const rapidjson::Value &value) {
  PactHistoryEntrySnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("iteration") && value["iteration"].IsInt()) {
    snapshot.iteration = value["iteration"].GetInt();
  }
  if (value.HasMember("validator") && value["validator"].IsString()) {
    snapshot.validator = value["validator"].GetString();
  }
  if (value.HasMember("validator_agent_id") &&
      value["validator_agent_id"].IsString()) {
    snapshot.validatorAgentId = value["validator_agent_id"].GetString();
  }
  if (value.HasMember("verdict") && value["verdict"].IsString()) {
    snapshot.verdict = value["verdict"].GetString();
  }
  if (value.HasMember("suggestion") && value["suggestion"].IsString()) {
    snapshot.suggestion = value["suggestion"].GetString();
  }
  if (value.HasMember("evidence_json") && value["evidence_json"].IsString()) {
    snapshot.evidenceJson = value["evidence_json"].GetString();
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const PactSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(snapshot.agentId, allocator), allocator);
  out.AddMember("pact_id", jsonString(snapshot.pactId, allocator), allocator);
  out.AddMember("status", jsonString(snapshot.status, allocator), allocator);
  out.AddMember("title", jsonString(snapshot.title, allocator), allocator);
  out.AddMember("summary", jsonString(snapshot.summary, allocator), allocator);
  out.AddMember("description", jsonString(snapshot.description, allocator),
                allocator);
  out.AddMember("validator", jsonString(snapshot.validator, allocator), allocator);
  out.AddMember("last_verdict", jsonString(snapshot.lastVerdict, allocator),
                allocator);
  out.AddMember("last_suggestion",
                jsonString(snapshot.lastSuggestion, allocator), allocator);
  out.AddMember("sealed_by", jsonString(snapshot.sealedBy, allocator), allocator);
  out.AddMember("status_line", jsonString(snapshot.statusLine, allocator),
                allocator);
  out.AddMember("blocking_reason",
                jsonString(snapshot.blockingReason, allocator), allocator);
  out.AddMember("state_payload_json",
                jsonString(snapshot.statePayloadJson, allocator), allocator);
  out.AddMember("created_at_ms", snapshot.createdAtMs, allocator);
  out.AddMember("updated_at_ms", snapshot.updatedAtMs, allocator);
  out.AddMember("iteration", snapshot.iteration, allocator);
  out.AddMember("max_iterations", snapshot.maxIterations, allocator);
  out.AddMember("active", snapshot.active, allocator);
  out.AddMember("resolved", snapshot.resolved, allocator);
  out.AddMember("failed", snapshot.failed, allocator);
  out.AddMember("stale", snapshot.stale, allocator);
  rapidjson::Value doneWhen(rapidjson::kArrayType);
  for (const auto &line : snapshot.doneWhen) {
    doneWhen.PushBack(jsonString(line, allocator), allocator);
  }
  out.AddMember("done_when", doneWhen, allocator);
  rapidjson::Value history(rapidjson::kArrayType);
  for (const auto &entry : snapshot.history) {
    history.PushBack(toJsonValue(entry, allocator), allocator);
  }
  out.AddMember("history", history, allocator);
  return out;
}

PactSnapshot pactSnapshotFromJson(const rapidjson::Value &value) {
  PactSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    snapshot.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("pact_id") && value["pact_id"].IsString()) {
    snapshot.pactId = value["pact_id"].GetString();
  }
  if (value.HasMember("status") && value["status"].IsString()) {
    snapshot.status = value["status"].GetString();
  }
  if (value.HasMember("title") && value["title"].IsString()) {
    snapshot.title = value["title"].GetString();
  }
  if (value.HasMember("summary") && value["summary"].IsString()) {
    snapshot.summary = value["summary"].GetString();
  }
  if (value.HasMember("description") && value["description"].IsString()) {
    snapshot.description = value["description"].GetString();
  }
  if (value.HasMember("validator") && value["validator"].IsString()) {
    snapshot.validator = value["validator"].GetString();
  }
  if (value.HasMember("last_verdict") && value["last_verdict"].IsString()) {
    snapshot.lastVerdict = value["last_verdict"].GetString();
  }
  if (value.HasMember("last_suggestion") &&
      value["last_suggestion"].IsString()) {
    snapshot.lastSuggestion = value["last_suggestion"].GetString();
  }
  if (value.HasMember("sealed_by") && value["sealed_by"].IsString()) {
    snapshot.sealedBy = value["sealed_by"].GetString();
  }
  if (value.HasMember("status_line") && value["status_line"].IsString()) {
    snapshot.statusLine = value["status_line"].GetString();
  }
  if (value.HasMember("blocking_reason") &&
      value["blocking_reason"].IsString()) {
    snapshot.blockingReason = value["blocking_reason"].GetString();
  }
  if (value.HasMember("state_payload_json") &&
      value["state_payload_json"].IsString()) {
    snapshot.statePayloadJson = value["state_payload_json"].GetString();
  }
  if (value.HasMember("created_at_ms") && value["created_at_ms"].IsUint64()) {
    snapshot.createdAtMs = value["created_at_ms"].GetUint64();
  }
  if (value.HasMember("updated_at_ms") && value["updated_at_ms"].IsUint64()) {
    snapshot.updatedAtMs = value["updated_at_ms"].GetUint64();
  }
  if (value.HasMember("iteration") && value["iteration"].IsInt()) {
    snapshot.iteration = value["iteration"].GetInt();
  }
  if (value.HasMember("max_iterations") &&
      value["max_iterations"].IsInt()) {
    snapshot.maxIterations = value["max_iterations"].GetInt();
  }
  if (value.HasMember("active") && value["active"].IsBool()) {
    snapshot.active = value["active"].GetBool();
  }
  if (value.HasMember("resolved") && value["resolved"].IsBool()) {
    snapshot.resolved = value["resolved"].GetBool();
  }
  if (value.HasMember("failed") && value["failed"].IsBool()) {
    snapshot.failed = value["failed"].GetBool();
  }
  if (value.HasMember("stale") && value["stale"].IsBool()) {
    snapshot.stale = value["stale"].GetBool();
  }
  if (value.HasMember("done_when") && value["done_when"].IsArray()) {
    for (const auto &line : value["done_when"].GetArray()) {
      if (line.IsString()) {
        snapshot.doneWhen.push_back(line.GetString());
      }
    }
  }
  if (value.HasMember("history") && value["history"].IsArray()) {
    for (const auto &entry : value["history"].GetArray()) {
      snapshot.history.push_back(pactHistoryEntrySnapshotFromJson(entry));
    }
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const WorkflowExecuteRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("workflow_id", jsonString(request.workflowId, allocator),
                allocator);
  rapidjson::Value args(rapidjson::kArrayType);
  for (const auto &arg : request.args) {
    args.PushBack(jsonString(arg, allocator), allocator);
  }
  out.AddMember("args", args, allocator);
  return out;
}

WorkflowExecuteRequest workflowExecuteRequestFromJson(const rapidjson::Value &value) {
  WorkflowExecuteRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("workflow_id") && value["workflow_id"].IsString()) {
    request.workflowId = value["workflow_id"].GetString();
  }
  if (value.HasMember("args") && value["args"].IsArray()) {
    for (const auto &arg : value["args"].GetArray()) {
      if (arg.IsString()) {
        request.args.push_back(arg.GetString());
      }
    }
  }
  return request;
}

rapidjson::Value toJsonValue(const WorkflowExecutionSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("workflow_id", jsonString(snapshot.workflowId, allocator),
                allocator);
  out.AddMember("name", jsonString(snapshot.name, allocator), allocator);
  out.AddMember("description", jsonString(snapshot.description, allocator),
                allocator);
  out.AddMember("slash_command", jsonString(snapshot.slashCommand, allocator),
                allocator);
  out.AddMember("hook", snapshot.hook, allocator);
  return out;
}

WorkflowExecutionSnapshot
workflowExecutionSnapshotFromJson(const rapidjson::Value &value) {
  WorkflowExecutionSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("workflow_id") && value["workflow_id"].IsString()) {
    snapshot.workflowId = value["workflow_id"].GetString();
  }
  if (value.HasMember("name") && value["name"].IsString()) {
    snapshot.name = value["name"].GetString();
  }
  if (value.HasMember("description") && value["description"].IsString()) {
    snapshot.description = value["description"].GetString();
  }
  if (value.HasMember("slash_command") && value["slash_command"].IsString()) {
    snapshot.slashCommand = value["slash_command"].GetString();
  }
  if (value.HasMember("hook") && value["hook"].IsBool()) {
    snapshot.hook = value["hook"].GetBool();
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const ArtifactCatalogSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  rapidjson::Value artifacts(rapidjson::kArrayType);
  for (const auto &artifact : snapshot.artifacts) {
    auto artifactDoc = firmius::shared::toJson(artifact);
    rapidjson::Value artifactValue(rapidjson::kObjectType);
    artifactValue.CopyFrom(artifactDoc, allocator);
    artifacts.PushBack(artifactValue, allocator);
  }
  out.AddMember("artifacts", artifacts, allocator);
  return out;
}

ArtifactCatalogSnapshot
artifactCatalogSnapshotFromJson(const rapidjson::Value &value) {
  ArtifactCatalogSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("artifacts") && value["artifacts"].IsArray()) {
    for (const auto &entry : value["artifacts"].GetArray()) {
      snapshot.artifacts.push_back(
          firmius::shared::threadArtifactMetadataFromJson(entry));
    }
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const EditsListRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  out.AddMember("include_undone", request.includeUndone, allocator);
  return out;
}

EditsListRequest editsListRequestFromJson(const rapidjson::Value &value) {
  EditsListRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("include_undone") && value["include_undone"].IsBool()) {
    request.includeUndone = value["include_undone"].GetBool();
  }
  return request;
}

rapidjson::Value toJsonValue(const EditsUndoRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  out.AddMember("edit_batch_id", jsonString(request.editBatchId, allocator),
                allocator);
  return out;
}

EditsUndoRequest editsUndoRequestFromJson(const rapidjson::Value &value) {
  EditsUndoRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("edit_batch_id") && value["edit_batch_id"].IsString()) {
    request.editBatchId = value["edit_batch_id"].GetString();
  }
  return request;
}

rapidjson::Value toJsonValue(const EditsRedoRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  out.AddMember("undo_action_id", jsonString(request.undoActionId, allocator),
                allocator);
  return out;
}

EditsRedoRequest editsRedoRequestFromJson(const rapidjson::Value &value) {
  EditsRedoRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("undo_action_id") &&
      value["undo_action_id"].IsString()) {
    request.undoActionId = value["undo_action_id"].GetString();
  }
  return request;
}

rapidjson::Value toJsonEditBatchSummaries(
    const std::vector<firmius::shared::EditBatchSummary> &batches,
    rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &batch : batches) {
    auto doc = firmius::shared::toJson(batch);
    rapidjson::Value value(rapidjson::kObjectType);
    value.CopyFrom(doc, allocator);
    out.PushBack(value, allocator);
  }
  return out;
}

std::vector<firmius::shared::EditBatchSummary>
editBatchSummariesFromJson(const rapidjson::Value &value) {
  std::vector<firmius::shared::EditBatchSummary> batches;
  if (!value.IsArray()) {
    return batches;
  }
  for (const auto &entry : value.GetArray()) {
    batches.push_back(firmius::shared::editBatchSummaryFromJson(entry));
  }
  return batches;
}

rapidjson::Value toJsonEditUndoEligibilities(
    const std::vector<firmius::shared::EditUndoEligibility> &eligibilities,
    rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &eligibility : eligibilities) {
    auto doc = firmius::shared::toJson(eligibility);
    rapidjson::Value value(rapidjson::kObjectType);
    value.CopyFrom(doc, allocator);
    out.PushBack(value, allocator);
  }
  return out;
}

std::vector<firmius::shared::EditUndoEligibility>
editUndoEligibilitiesFromJson(const rapidjson::Value &value) {
  std::vector<firmius::shared::EditUndoEligibility> eligibilities;
  if (!value.IsArray()) {
    return eligibilities;
  }
  for (const auto &entry : value.GetArray()) {
    eligibilities.push_back(firmius::shared::editUndoEligibilityFromJson(entry));
  }
  return eligibilities;
}

rapidjson::Value toJsonValue(const EditHistorySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(snapshot.agentId, allocator), allocator);
  out.AddMember("batches", toJsonEditBatchSummaries(snapshot.batches, allocator),
                allocator);
  out.AddMember("undo_eligibilities",
                toJsonEditUndoEligibilities(snapshot.undoEligibilities, allocator),
                allocator);
  return out;
}

EditHistorySnapshot editHistorySnapshotFromJson(const rapidjson::Value &value) {
  EditHistorySnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    snapshot.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("batches")) {
    snapshot.batches = editBatchSummariesFromJson(value["batches"]);
  }
  if (value.HasMember("undo_eligibilities")) {
    snapshot.undoEligibilities =
        editUndoEligibilitiesFromJson(value["undo_eligibilities"]);
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const EditMutationResult &result,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("applied", result.applied, allocator);
  out.AddMember("thread_id", jsonString(result.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(result.agentId, allocator), allocator);
  if (result.undoAction.has_value()) {
    auto doc = firmius::shared::toJson(result.undoAction.value());
    rapidjson::Value value(rapidjson::kObjectType);
    value.CopyFrom(doc, allocator);
    out.AddMember("undo_action", value, allocator);
  } else {
    out.AddMember("undo_action", rapidjson::Value(rapidjson::kNullType),
                  allocator);
  }
  if (result.redoAction.has_value()) {
    auto doc = firmius::shared::toJson(result.redoAction.value());
    rapidjson::Value value(rapidjson::kObjectType);
    value.CopyFrom(doc, allocator);
    out.AddMember("redo_action", value, allocator);
  } else {
    out.AddMember("redo_action", rapidjson::Value(rapidjson::kNullType),
                  allocator);
  }
  if (result.undoEligibility.has_value()) {
    auto doc = firmius::shared::toJson(result.undoEligibility.value());
    rapidjson::Value value(rapidjson::kObjectType);
    value.CopyFrom(doc, allocator);
    out.AddMember("undo_eligibility", value, allocator);
  } else {
    out.AddMember("undo_eligibility", rapidjson::Value(rapidjson::kNullType),
                  allocator);
  }
  if (result.redoEligibility.has_value()) {
    auto doc = firmius::shared::toJson(result.redoEligibility.value());
    rapidjson::Value value(rapidjson::kObjectType);
    value.CopyFrom(doc, allocator);
    out.AddMember("redo_eligibility", value, allocator);
  } else {
    out.AddMember("redo_eligibility", rapidjson::Value(rapidjson::kNullType),
                  allocator);
  }
  out.AddMember("message", jsonString(result.message, allocator), allocator);
  out.AddMember("edits", toJsonValue(result.edits, allocator), allocator);
  return out;
}

EditMutationResult editMutationResultFromJson(const rapidjson::Value &value) {
  EditMutationResult result;
  if (!value.IsObject()) {
    return result;
  }
  if (value.HasMember("applied") && value["applied"].IsBool()) {
    result.applied = value["applied"].GetBool();
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    result.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    result.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("undo_action") && value["undo_action"].IsObject()) {
    result.undoAction = firmius::shared::editUndoActionFromJson(value["undo_action"]);
  }
  if (value.HasMember("redo_action") && value["redo_action"].IsObject()) {
    result.redoAction = firmius::shared::editRedoActionFromJson(value["redo_action"]);
  }
  if (value.HasMember("undo_eligibility") &&
      value["undo_eligibility"].IsObject()) {
    result.undoEligibility =
        firmius::shared::editUndoEligibilityFromJson(value["undo_eligibility"]);
  }
  if (value.HasMember("redo_eligibility") &&
      value["redo_eligibility"].IsObject()) {
    result.redoEligibility =
        firmius::shared::editRedoEligibilityFromJson(value["redo_eligibility"]);
  }
  if (value.HasMember("message") && value["message"].IsString()) {
    result.message = value["message"].GetString();
  }
  if (value.HasMember("edits") && value["edits"].IsObject()) {
    result.edits = editHistorySnapshotFromJson(value["edits"]);
  }
  return result;
}

rapidjson::Value toJsonValue(const TranscriptGetRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(request.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(request.agentId, allocator), allocator);
  return out;
}

TranscriptGetRequest transcriptGetRequestFromJson(const rapidjson::Value &value) {
  TranscriptGetRequest request;
  if (!value.IsObject()) {
    return request;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    request.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    request.agentId = value["agent_id"].GetString();
  }
  return request;
}

rapidjson::Value toJsonTurns(const std::vector<firmius::shared::AgentTurn> &turns,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &turn : turns) {
    auto turnDoc = firmius::shared::toJson(turn);
    rapidjson::Value turnValue(rapidjson::kObjectType);
    turnValue.CopyFrom(turnDoc, allocator);
    out.PushBack(turnValue, allocator);
  }
  return out;
}

std::vector<firmius::shared::AgentTurn>
turnListFromJson(const rapidjson::Value &value) {
  std::vector<firmius::shared::AgentTurn> turns;
  if (!value.IsArray()) {
    return turns;
  }
  for (const auto &turn : value.GetArray()) {
    turns.push_back(firmius::shared::agentTurnFromJsonValue(turn));
  }
  return turns;
}

rapidjson::Value toJsonValue(const TranscriptSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(snapshot.agentId, allocator), allocator);
  out.AddMember("agent_title", jsonString(snapshot.agentTitle, allocator), allocator);
  out.AddMember("agent_friendly_name",
                jsonString(snapshot.agentFriendlyName, allocator), allocator);
  out.AddMember("raw_turns", toJsonTurns(snapshot.rawTurns, allocator), allocator);
  out.AddMember("expanded_turns", toJsonTurns(snapshot.expandedTurns, allocator),
                allocator);
  return out;
}

TranscriptSnapshot transcriptSnapshotFromJson(const rapidjson::Value &value) {
  TranscriptSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    snapshot.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("agent_title") && value["agent_title"].IsString()) {
    snapshot.agentTitle = value["agent_title"].GetString();
  }
  if (value.HasMember("agent_friendly_name") &&
      value["agent_friendly_name"].IsString()) {
    snapshot.agentFriendlyName = value["agent_friendly_name"].GetString();
  }
  if (value.HasMember("raw_turns")) {
    snapshot.rawTurns = turnListFromJson(value["raw_turns"]);
  }
  if (value.HasMember("expanded_turns")) {
    snapshot.expandedTurns = turnListFromJson(value["expanded_turns"]);
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const ToolCallsListRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  return toJsonValue(
      TranscriptGetRequest{request.threadId, request.agentId}, allocator);
}

ToolCallsListRequest toolCallsListRequestFromJson(const rapidjson::Value &value) {
  ToolCallsListRequest request;
  auto base = transcriptGetRequestFromJson(value);
  request.threadId = base.threadId;
  request.agentId = base.agentId;
  return request;
}

rapidjson::Value toJsonValue(const ToolCallSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(snapshot.agentId, allocator), allocator);
  out.AddMember("tool_call_id", jsonString(snapshot.toolCallId, allocator), allocator);
  out.AddMember("tool_name", jsonString(snapshot.toolName, allocator), allocator);
  out.AddMember("tool_args_json", jsonString(snapshot.toolArgsJson, allocator),
                allocator);
  out.AddMember("summary", jsonString(snapshot.summary, allocator), allocator);
  out.AddMember("status", jsonString(snapshot.status, allocator), allocator);
  if (snapshot.success.has_value()) {
    out.AddMember("success", snapshot.success.value(), allocator);
  } else {
    out.AddMember("success", rapidjson::Value(rapidjson::kNullType), allocator);
  }
  out.AddMember("result_json", jsonString(snapshot.resultJson, allocator), allocator);
  out.AddMember("result_summary", jsonString(snapshot.resultSummary, allocator),
                allocator);
  out.AddMember("error_summary", jsonString(snapshot.errorSummary, allocator),
                allocator);
  out.AddMember("process_id", jsonString(snapshot.processId, allocator), allocator);
  out.AddMember("subagent_id", jsonString(snapshot.subagentId, allocator), allocator);
  out.AddMember("issued_at_ms", snapshot.issuedAtMs, allocator);
  out.AddMember("completed_at_ms", snapshot.completedAtMs, allocator);
  return out;
}

ToolCallSnapshot toolCallSnapshotFromJson(const rapidjson::Value &value) {
  ToolCallSnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    snapshot.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("tool_call_id") && value["tool_call_id"].IsString()) {
    snapshot.toolCallId = value["tool_call_id"].GetString();
  }
  if (value.HasMember("tool_name") && value["tool_name"].IsString()) {
    snapshot.toolName = value["tool_name"].GetString();
  }
  if (value.HasMember("tool_args_json") && value["tool_args_json"].IsString()) {
    snapshot.toolArgsJson = value["tool_args_json"].GetString();
  }
  if (value.HasMember("summary") && value["summary"].IsString()) {
    snapshot.summary = value["summary"].GetString();
  }
  if (value.HasMember("status") && value["status"].IsString()) {
    snapshot.status = value["status"].GetString();
  }
  if (value.HasMember("success") && value["success"].IsBool()) {
    snapshot.success = value["success"].GetBool();
  }
  if (value.HasMember("result_json") && value["result_json"].IsString()) {
    snapshot.resultJson = value["result_json"].GetString();
  }
  if (value.HasMember("result_summary") && value["result_summary"].IsString()) {
    snapshot.resultSummary = value["result_summary"].GetString();
  }
  if (value.HasMember("error_summary") && value["error_summary"].IsString()) {
    snapshot.errorSummary = value["error_summary"].GetString();
  }
  if (value.HasMember("process_id") && value["process_id"].IsString()) {
    snapshot.processId = value["process_id"].GetString();
  }
  if (value.HasMember("subagent_id") && value["subagent_id"].IsString()) {
    snapshot.subagentId = value["subagent_id"].GetString();
  }
  if (value.HasMember("issued_at_ms") && value["issued_at_ms"].IsUint64()) {
    snapshot.issuedAtMs = value["issued_at_ms"].GetUint64();
  }
  if (value.HasMember("completed_at_ms") &&
      value["completed_at_ms"].IsUint64()) {
    snapshot.completedAtMs = value["completed_at_ms"].GetUint64();
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const SubagentsActivityRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  return toJsonValue(
      TranscriptGetRequest{request.threadId, request.agentId}, allocator);
}

SubagentsActivityRequest
subagentsActivityRequestFromJson(const rapidjson::Value &value) {
  SubagentsActivityRequest request;
  auto base = transcriptGetRequestFromJson(value);
  request.threadId = base.threadId;
  request.agentId = base.agentId;
  return request;
}

rapidjson::Value toJsonValue(const BenchmarksStartRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("benchmark_id", jsonString(request.benchmarkId, allocator), allocator);
  out.AddMember("task_id", jsonString(request.taskId, allocator), allocator);
  {
    auto hostDoc = firmius::shared::toJson(request.hostOptions);
    out.AddMember("host_options", rapidjson::Value(hostDoc, allocator), allocator);
  }
  out.AddMember("cwd", jsonString(request.cwd, allocator), allocator);
  out.AddMember("persona_name", jsonString(request.personaName, allocator), allocator);
  return out;
}

BenchmarksStartRequest benchmarksStartRequestFromJson(const rapidjson::Value &value) {
  BenchmarksStartRequest request;
  if (value.HasMember("benchmark_id") && value["benchmark_id"].IsString()) {
    request.benchmarkId = value["benchmark_id"].GetString();
  }
  if (value.HasMember("task_id") && value["task_id"].IsString()) {
    request.taskId = value["task_id"].GetString();
  }
  if (value.HasMember("host_options") && value["host_options"].IsObject()) {
    request.hostOptions = firmius::shared::hostCreationOptionsFromJsonValue(value["host_options"]);
  }
  if (value.HasMember("cwd") && value["cwd"].IsString()) {
    request.cwd = value["cwd"].GetString();
  }
  if (value.HasMember("persona_name") && value["persona_name"].IsString()) {
    request.personaName = value["persona_name"].GetString();
  }
  return request;
}

rapidjson::Value toJsonValue(const BenchmarksStartResponse &response,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("started", response.started, allocator);
  out.AddMember("thread_id", jsonString(response.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(response.agentId, allocator), allocator);
  out.AddMember("benchmark_id", jsonString(response.benchmarkId, allocator), allocator);
  out.AddMember("task_id", jsonString(response.taskId, allocator), allocator);
  return out;
}

BenchmarksStartResponse benchmarksStartResponseFromJson(const rapidjson::Value &value) {
  BenchmarksStartResponse response;
  if (value.HasMember("started") && value["started"].IsBool()) {
    response.started = value["started"].GetBool();
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    response.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    response.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("benchmark_id") && value["benchmark_id"].IsString()) {
    response.benchmarkId = value["benchmark_id"].GetString();
  }
  if (value.HasMember("task_id") && value["task_id"].IsString()) {
    response.taskId = value["task_id"].GetString();
  }
  return response;
}

rapidjson::Value toJsonValue(const BenchmarksStatusRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  return toJsonValue(TranscriptGetRequest{request.threadId, request.agentId}, allocator);
}

BenchmarksStatusRequest benchmarksStatusRequestFromJson(const rapidjson::Value &value) {
  BenchmarksStatusRequest request;
  auto base = transcriptGetRequestFromJson(value);
  request.threadId = base.threadId;
  request.agentId = base.agentId;
  return request;
}

rapidjson::Value toJsonValue(const BenchmarkStatusSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(snapshot.agentId, allocator), allocator);
  out.AddMember("is_benchmark_run", snapshot.isBenchmarkRun, allocator);
  out.AddMember("benchmark_id", jsonString(snapshot.benchmarkId, allocator), allocator);
  out.AddMember("task_id", jsonString(snapshot.taskId, allocator), allocator);
  out.AddMember("agent_live", snapshot.agentLive, allocator);
  return out;
}

BenchmarkStatusSnapshot benchmarkStatusSnapshotFromJson(const rapidjson::Value &value) {
  BenchmarkStatusSnapshot snapshot;
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    snapshot.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("is_benchmark_run") && value["is_benchmark_run"].IsBool()) {
    snapshot.isBenchmarkRun = value["is_benchmark_run"].GetBool();
  }
  if (value.HasMember("benchmark_id") && value["benchmark_id"].IsString()) {
    snapshot.benchmarkId = value["benchmark_id"].GetString();
  }
  if (value.HasMember("task_id") && value["task_id"].IsString()) {
    snapshot.taskId = value["task_id"].GetString();
  }
  if (value.HasMember("agent_live") && value["agent_live"].IsBool()) {
    snapshot.agentLive = value["agent_live"].GetBool();
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const BenchmarksLogsRequest &request,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out = toJsonValue(TranscriptGetRequest{request.threadId, request.agentId}, allocator);
  out.AddMember("limit", request.limit, allocator);
  return out;
}

BenchmarksLogsRequest benchmarksLogsRequestFromJson(const rapidjson::Value &value) {
  BenchmarksLogsRequest request;
  auto base = transcriptGetRequestFromJson(value);
  request.threadId = base.threadId;
  request.agentId = base.agentId;
  if (value.HasMember("limit") && value["limit"].IsInt()) {
    request.limit = value["limit"].GetInt();
  }
  return request;
}

rapidjson::Value toJsonValue(const BenchmarkLogsSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(snapshot.agentId, allocator), allocator);
  rapidjson::Value lines(rapidjson::kArrayType);
  for (const auto &line : snapshot.lines) {
    lines.PushBack(jsonString(line, allocator), allocator);
  }
  out.AddMember("lines", lines, allocator);
  return out;
}

BenchmarkLogsSnapshot benchmarkLogsSnapshotFromJson(const rapidjson::Value &value) {
  BenchmarkLogsSnapshot snapshot;
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    snapshot.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("lines") && value["lines"].IsArray()) {
    for (const auto &entry : value["lines"].GetArray()) {
      if (entry.IsString()) {
        snapshot.lines.emplace_back(entry.GetString());
      }
    }
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const SubagentActivityLogEntrySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("summary", jsonString(snapshot.summary, allocator), allocator);
  out.AddMember("phase", jsonString(snapshot.phase, allocator), allocator);
  out.AddMember("tool_call_id", jsonString(snapshot.toolCallId, allocator),
                allocator);
  out.AddMember("tool_name", jsonString(snapshot.toolName, allocator), allocator);
  out.AddMember("tool_args_json",
                jsonString(snapshot.toolArgsJson, allocator), allocator);
  return out;
}

SubagentActivityLogEntrySnapshot
subagentActivityLogEntrySnapshotFromJson(const rapidjson::Value &value) {
  SubagentActivityLogEntrySnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("summary") && value["summary"].IsString()) {
    snapshot.summary = value["summary"].GetString();
  }
  if (value.HasMember("phase") && value["phase"].IsString()) {
    snapshot.phase = value["phase"].GetString();
  }
  if (value.HasMember("tool_call_id") && value["tool_call_id"].IsString()) {
    snapshot.toolCallId = value["tool_call_id"].GetString();
  }
  if (value.HasMember("tool_name") && value["tool_name"].IsString()) {
    snapshot.toolName = value["tool_name"].GetString();
  }
  if (value.HasMember("tool_args_json") && value["tool_args_json"].IsString()) {
    snapshot.toolArgsJson = value["tool_args_json"].GetString();
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const SubagentActivityEntrySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("parent_agent_id", jsonString(snapshot.parentAgentId, allocator),
                allocator);
  out.AddMember("parent_tool_call_id",
                jsonString(snapshot.parentToolCallId, allocator), allocator);
  out.AddMember("child_agent_id", jsonString(snapshot.childAgentId, allocator),
                allocator);
  out.AddMember("child_title", jsonString(snapshot.childTitle, allocator), allocator);
  out.AddMember("child_friendly_name",
                jsonString(snapshot.childFriendlyName, allocator), allocator);
  out.AddMember("task", jsonString(snapshot.task, allocator), allocator);
  out.AddMember("running", snapshot.running, allocator);
  out.AddMember("waiting", snapshot.waiting, allocator);
  out.AddMember("provider_waiting", snapshot.providerWaiting, allocator);
  out.AddMember("retrying", snapshot.retrying, allocator);
  out.AddMember("account_switched", snapshot.accountSwitched, allocator);
  out.AddMember("fallback_used", snapshot.fallbackUsed, allocator);
  out.AddMember("wait_state", jsonString(snapshot.waitState, allocator), allocator);
  out.AddMember("route_category", jsonString(snapshot.routeCategory, allocator),
                allocator);
  rapidjson::Value attempted(rapidjson::kArrayType);
  for (const auto &category : snapshot.attemptedCategories) {
    attempted.PushBack(jsonString(category, allocator), allocator);
  }
  out.AddMember("attempted_categories", attempted, allocator);
  out.AddMember("outcome", jsonString(snapshot.outcome, allocator), allocator);
  out.AddMember("final_summary", jsonString(snapshot.finalSummary, allocator),
                allocator);
  out.AddMember("error_text", jsonString(snapshot.errorText, allocator), allocator);
  rapidjson::Value created(rapidjson::kArrayType);
  for (const auto &artifact : snapshot.artifactsCreated) {
    created.PushBack(jsonString(artifact, allocator), allocator);
  }
  out.AddMember("artifacts_created", created, allocator);
  rapidjson::Value updated(rapidjson::kArrayType);
  for (const auto &artifact : snapshot.artifactsUpdated) {
    updated.PushBack(jsonString(artifact, allocator), allocator);
  }
  out.AddMember("artifacts_updated", updated, allocator);
  rapidjson::Value activity(rapidjson::kArrayType);
  for (const auto &entry : snapshot.activityLog) {
    activity.PushBack(toJsonValue(entry, allocator), allocator);
  }
  out.AddMember("activity_log", activity, allocator);
  return out;
}

SubagentActivityEntrySnapshot
subagentActivityEntrySnapshotFromJson(const rapidjson::Value &value) {
  SubagentActivityEntrySnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("parent_agent_id") &&
      value["parent_agent_id"].IsString()) {
    snapshot.parentAgentId = value["parent_agent_id"].GetString();
  }
  if (value.HasMember("parent_tool_call_id") &&
      value["parent_tool_call_id"].IsString()) {
    snapshot.parentToolCallId = value["parent_tool_call_id"].GetString();
  }
  if (value.HasMember("child_agent_id") && value["child_agent_id"].IsString()) {
    snapshot.childAgentId = value["child_agent_id"].GetString();
  }
  if (value.HasMember("child_title") && value["child_title"].IsString()) {
    snapshot.childTitle = value["child_title"].GetString();
  }
  if (value.HasMember("child_friendly_name") &&
      value["child_friendly_name"].IsString()) {
    snapshot.childFriendlyName = value["child_friendly_name"].GetString();
  }
  if (value.HasMember("task") && value["task"].IsString()) {
    snapshot.task = value["task"].GetString();
  }
  if (value.HasMember("running") && value["running"].IsBool()) {
    snapshot.running = value["running"].GetBool();
  }
  if (value.HasMember("waiting") && value["waiting"].IsBool()) {
    snapshot.waiting = value["waiting"].GetBool();
  }
  if (value.HasMember("provider_waiting") &&
      value["provider_waiting"].IsBool()) {
    snapshot.providerWaiting = value["provider_waiting"].GetBool();
  }
  if (value.HasMember("retrying") && value["retrying"].IsBool()) {
    snapshot.retrying = value["retrying"].GetBool();
  }
  if (value.HasMember("account_switched") &&
      value["account_switched"].IsBool()) {
    snapshot.accountSwitched = value["account_switched"].GetBool();
  }
  if (value.HasMember("fallback_used") && value["fallback_used"].IsBool()) {
    snapshot.fallbackUsed = value["fallback_used"].GetBool();
  }
  if (value.HasMember("wait_state") && value["wait_state"].IsString()) {
    snapshot.waitState = value["wait_state"].GetString();
  }
  if (value.HasMember("route_category") && value["route_category"].IsString()) {
    snapshot.routeCategory = value["route_category"].GetString();
  }
  if (value.HasMember("attempted_categories") &&
      value["attempted_categories"].IsArray()) {
    for (const auto &category : value["attempted_categories"].GetArray()) {
      if (category.IsString()) {
        snapshot.attemptedCategories.push_back(category.GetString());
      }
    }
  }
  if (value.HasMember("outcome") && value["outcome"].IsString()) {
    snapshot.outcome = value["outcome"].GetString();
  }
  if (value.HasMember("final_summary") && value["final_summary"].IsString()) {
    snapshot.finalSummary = value["final_summary"].GetString();
  }
  if (value.HasMember("error_text") && value["error_text"].IsString()) {
    snapshot.errorText = value["error_text"].GetString();
  }
  if (value.HasMember("artifacts_created") &&
      value["artifacts_created"].IsArray()) {
    for (const auto &artifact : value["artifacts_created"].GetArray()) {
      if (artifact.IsString()) {
        snapshot.artifactsCreated.push_back(artifact.GetString());
      }
    }
  }
  if (value.HasMember("artifacts_updated") &&
      value["artifacts_updated"].IsArray()) {
    for (const auto &artifact : value["artifacts_updated"].GetArray()) {
      if (artifact.IsString()) {
        snapshot.artifactsUpdated.push_back(artifact.GetString());
      }
    }
  }
  if (value.HasMember("activity_log") && value["activity_log"].IsArray()) {
    for (const auto &entry : value["activity_log"].GetArray()) {
      snapshot.activityLog.push_back(
          subagentActivityLogEntrySnapshotFromJson(entry));
    }
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const SubagentActivitySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(snapshot.threadId, allocator), allocator);
  out.AddMember("agent_id", jsonString(snapshot.agentId, allocator), allocator);
  out.AddMember("activities",
                toJsonValue(snapshot.activities, allocator), allocator);
  return out;
}

SubagentActivitySnapshot
subagentActivitySnapshotFromJson(const rapidjson::Value &value) {
  SubagentActivitySnapshot snapshot;
  if (!value.IsObject()) {
    return snapshot;
  }
  if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
    snapshot.threadId = value["thread_id"].GetString();
  }
  if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
    snapshot.agentId = value["agent_id"].GetString();
  }
  if (value.HasMember("activities")) {
    snapshot.activities =
        subagentActivityEntrySnapshotListFromJson(value["activities"]);
  }
  return snapshot;
}

rapidjson::Value toJsonValue(const std::vector<AgentRuntimeSnapshot> &snapshots,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &snapshot : snapshots) {
    out.PushBack(toJsonValue(snapshot, allocator), allocator);
  }
  return out;
}

std::vector<AgentRuntimeSnapshot>
agentRuntimeSnapshotListFromJson(const rapidjson::Value &value) {
  std::vector<AgentRuntimeSnapshot> snapshots;
  if (!value.IsArray()) {
    return snapshots;
  }
  for (const auto &snapshot : value.GetArray()) {
    snapshots.push_back(agentRuntimeSnapshotFromJson(snapshot));
  }
  return snapshots;
}

rapidjson::Value toJsonValue(const std::vector<ProcessSnapshot> &snapshots,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &snapshot : snapshots) {
    out.PushBack(toJsonValue(snapshot, allocator), allocator);
  }
  return out;
}

std::vector<ProcessSnapshot> processSnapshotListFromJson(
    const rapidjson::Value &value) {
  std::vector<ProcessSnapshot> snapshots;
  if (!value.IsArray()) {
    return snapshots;
  }
  for (const auto &snapshot : value.GetArray()) {
    snapshots.push_back(processSnapshotFromJson(snapshot));
  }
  return snapshots;
}

rapidjson::Value toJsonValue(const std::vector<AccountSnapshot> &snapshots,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &snapshot : snapshots) {
    out.PushBack(toJsonValue(snapshot, allocator), allocator);
  }
  return out;
}

std::vector<AccountSnapshot> accountSnapshotListFromJson(
    const rapidjson::Value &value) {
  std::vector<AccountSnapshot> snapshots;
  if (!value.IsArray()) {
    return snapshots;
  }
  for (const auto &snapshot : value.GetArray()) {
    snapshots.push_back(accountSnapshotFromJson(snapshot));
  }
  return snapshots;
}

rapidjson::Value toJsonValue(
    const std::vector<WorkflowExecutionSnapshot> &snapshots,
    rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &snapshot : snapshots) {
    out.PushBack(toJsonValue(snapshot, allocator), allocator);
  }
  return out;
}

std::vector<WorkflowExecutionSnapshot>
workflowExecutionSnapshotListFromJson(const rapidjson::Value &value) {
  std::vector<WorkflowExecutionSnapshot> snapshots;
  if (!value.IsArray()) {
    return snapshots;
  }
  for (const auto &snapshot : value.GetArray()) {
    snapshots.push_back(workflowExecutionSnapshotFromJson(snapshot));
  }
  return snapshots;
}

rapidjson::Value toJsonValue(const std::vector<PactSnapshot> &snapshots,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &snapshot : snapshots) {
    out.PushBack(toJsonValue(snapshot, allocator), allocator);
  }
  return out;
}

std::vector<PactSnapshot> pactSnapshotListFromJson(const rapidjson::Value &value) {
  std::vector<PactSnapshot> snapshots;
  if (!value.IsArray()) {
    return snapshots;
  }
  for (const auto &snapshot : value.GetArray()) {
    snapshots.push_back(pactSnapshotFromJson(snapshot));
  }
  return snapshots;
}

rapidjson::Value toJsonValue(const std::vector<ToolCallSnapshot> &snapshots,
                             rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &snapshot : snapshots) {
    out.PushBack(toJsonValue(snapshot, allocator), allocator);
  }
  return out;
}

std::vector<ToolCallSnapshot> toolCallSnapshotListFromJson(
    const rapidjson::Value &value) {
  std::vector<ToolCallSnapshot> snapshots;
  if (!value.IsArray()) {
    return snapshots;
  }
  for (const auto &snapshot : value.GetArray()) {
    snapshots.push_back(toolCallSnapshotFromJson(snapshot));
  }
  return snapshots;
}

rapidjson::Value toJsonValue(
    const std::vector<SubagentActivityEntrySnapshot> &snapshots,
    rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &snapshot : snapshots) {
    out.PushBack(toJsonValue(snapshot, allocator), allocator);
  }
  return out;
}

std::vector<SubagentActivityEntrySnapshot>
subagentActivityEntrySnapshotListFromJson(const rapidjson::Value &value) {
  std::vector<SubagentActivityEntrySnapshot> snapshots;
  if (!value.IsArray()) {
    return snapshots;
  }
  for (const auto &snapshot : value.GetArray()) {
    snapshots.push_back(subagentActivityEntrySnapshotFromJson(snapshot));
  }
  return snapshots;
}

rapidjson::Value toJsonValue(const ModesListRequest &, rapidjson::Document::AllocatorType &) { return rapidjson::Value(rapidjson::kObjectType); }
ModesListRequest modesListRequestFromJson(const rapidjson::Value &) { return {}; }

rapidjson::Value toJsonValue(const ModesGetRequest &r, rapidjson::Document::AllocatorType &a) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("mode_id", jsonString(r.modeId, a), a);
  return out;
}
ModesGetRequest modesGetRequestFromJson(const rapidjson::Value &v) {
  ModesGetRequest r;
  if (v.IsObject() && v.HasMember("mode_id") && v["mode_id"].IsString()) { r.modeId = v["mode_id"].GetString(); }
  return r;
}

rapidjson::Value toJsonValue(const AgentsSetModeRequest &r, rapidjson::Document::AllocatorType &a) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("thread_id", jsonString(r.threadId, a), a);
  out.AddMember("agent_id", jsonString(r.agentId, a), a);
  out.AddMember("mode_id", jsonString(r.modeId, a), a);
  return out;
}
AgentsSetModeRequest agentsSetModeRequestFromJson(const rapidjson::Value &v) {
  AgentsSetModeRequest r;
  if (!v.IsObject()) return r;
  if (v.HasMember("thread_id") && v["thread_id"].IsString()) r.threadId = v["thread_id"].GetString();
  if (v.HasMember("agent_id") && v["agent_id"].IsString()) r.agentId = v["agent_id"].GetString();
  if (v.HasMember("mode_id") && v["mode_id"].IsString()) r.modeId = v["mode_id"].GetString();
  return r;
}

rapidjson::Value toJsonValue(const PersonasListRequest &, rapidjson::Document::AllocatorType &) { return rapidjson::Value(rapidjson::kObjectType); }
PersonasListRequest personasListRequestFromJson(const rapidjson::Value &) { return {}; }

rapidjson::Value toJsonValue(const ToolsCatalogRequest &, rapidjson::Document::AllocatorType &) { return rapidjson::Value(rapidjson::kObjectType); }
ToolsCatalogRequest toolsCatalogRequestFromJson(const rapidjson::Value &) { return {}; }

rapidjson::Value toJsonValue(const BenchmarksListSupportedRequest &, rapidjson::Document::AllocatorType &) { return rapidjson::Value(rapidjson::kObjectType); }
BenchmarksListSupportedRequest benchmarksListSupportedRequestFromJson(const rapidjson::Value &) { return {}; }

rapidjson::Value toJsonValue(const HooksRecentActivityRequest &, rapidjson::Document::AllocatorType &) { return rapidjson::Value(rapidjson::kObjectType); }
HooksRecentActivityRequest hooksRecentActivityRequestFromJson(const rapidjson::Value &) { return {}; }

rapidjson::Value toJsonValue(const ModeSnapshot &r, rapidjson::Document::AllocatorType &a) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("mode_id", jsonString(r.modeId, a), a);
  out.AddMember("name", jsonString(r.name, a), a);
  out.AddMember("description", jsonString(r.description, a), a);
  return out;
}
ModeSnapshot modeSnapshotFromJson(const rapidjson::Value &v) {
  ModeSnapshot r;
  if (!v.IsObject()) return r;
  if (v.HasMember("mode_id") && v["mode_id"].IsString()) r.modeId = v["mode_id"].GetString();
  if (v.HasMember("name") && v["name"].IsString()) r.name = v["name"].GetString();
  if (v.HasMember("description") && v["description"].IsString()) r.description = v["description"].GetString();
  return r;
}

rapidjson::Value toJsonValue(const ModeCatalogSnapshot &r, rapidjson::Document::AllocatorType &a) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value list(rapidjson::kArrayType);
  for (const auto& mode : r.modes) { list.PushBack(toJsonValue(mode, a), a); }
  out.AddMember("modes", list, a);
  return out;
}
ModeCatalogSnapshot modeCatalogSnapshotFromJson(const rapidjson::Value &v) {
  ModeCatalogSnapshot r;
  if (v.IsObject() && v.HasMember("modes") && v["modes"].IsArray()) {
    for (const auto& item : v["modes"].GetArray()) { r.modes.push_back(modeSnapshotFromJson(item)); }
  }
  return r;
}

rapidjson::Value toJsonValue(const PersonaSnapshot &r, rapidjson::Document::AllocatorType &a) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("id", jsonString(r.id, a), a);
  out.AddMember("name", jsonString(r.name, a), a);
  out.AddMember("title", jsonString(r.title, a), a);
  out.AddMember("description", jsonString(r.description, a), a);
  rapidjson::Value scopes(rapidjson::kArrayType);
  for (const auto& s : r.allowedScopes) { scopes.PushBack(static_cast<int>(s), a); }
  out.AddMember("allowed_scopes", scopes, a);
  return out;
}
PersonaSnapshot personaSnapshotFromJson(const rapidjson::Value &v) {
  PersonaSnapshot r;
  if (!v.IsObject()) return r;
  if (v.HasMember("id") && v["id"].IsString()) r.id = v["id"].GetString();
  if (v.HasMember("name") && v["name"].IsString()) r.name = v["name"].GetString();
  if (v.HasMember("title") && v["title"].IsString()) r.title = v["title"].GetString();
  if (v.HasMember("description") && v["description"].IsString()) r.description = v["description"].GetString();
  if (v.HasMember("allowed_scopes") && v["allowed_scopes"].IsArray()) {
    for (const auto& item : v["allowed_scopes"].GetArray()) { r.allowedScopes.push_back(static_cast<firmius::shared::ToolScope>(item.GetInt())); }
  }
  return r;
}

rapidjson::Value toJsonValue(const PersonaCatalogSnapshot &r, rapidjson::Document::AllocatorType &a) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value list(rapidjson::kArrayType);
  for (const auto& persona : r.personas) { list.PushBack(toJsonValue(persona, a), a); }
  out.AddMember("personas", list, a);
  return out;
}
PersonaCatalogSnapshot personaCatalogSnapshotFromJson(const rapidjson::Value &v) {
  PersonaCatalogSnapshot r;
  if (v.IsObject() && v.HasMember("personas") && v["personas"].IsArray()) {
    for (const auto& item : v["personas"].GetArray()) { r.personas.push_back(personaSnapshotFromJson(item)); }
  }
  return r;
}

rapidjson::Value toJsonValue(const ToolSnapshot &r, rapidjson::Document::AllocatorType &a) {
  rapidjson::Value out(rapidjson::kObjectType);
  out.AddMember("name", jsonString(r.name, a), a);
  out.AddMember("description", jsonString(r.description, a), a);
  rapidjson::Value scopes(rapidjson::kArrayType);
  for (const auto& s : r.scopes) { scopes.PushBack(static_cast<int>(s), a); }
  out.AddMember("scopes", scopes, a);
  return out;
}
ToolSnapshot toolSnapshotFromJson(const rapidjson::Value &v) {
  ToolSnapshot r;
  if (!v.IsObject()) return r;
  if (v.HasMember("name") && v["name"].IsString()) r.name = v["name"].GetString();
  if (v.HasMember("description") && v["description"].IsString()) r.description = v["description"].GetString();
  if (v.HasMember("scopes") && v["scopes"].IsArray()) {
    for (const auto& item : v["scopes"].GetArray()) { r.scopes.push_back(static_cast<firmius::shared::ToolScope>(item.GetInt())); }
  }
  return r;
}

rapidjson::Value toJsonValue(const ToolCatalogSnapshot &r, rapidjson::Document::AllocatorType &a) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value list(rapidjson::kArrayType);
  for (const auto& tool : r.tools) { list.PushBack(toJsonValue(tool, a), a); }
  out.AddMember("tools", list, a);
  return out;
}
ToolCatalogSnapshot toolCatalogSnapshotFromJson(const rapidjson::Value &v) {
  ToolCatalogSnapshot r;
  if (v.IsObject() && v.HasMember("tools") && v["tools"].IsArray()) {
    for (const auto& item : v["tools"].GetArray()) { r.tools.push_back(toolSnapshotFromJson(item)); }
  }
  return r;
}

rapidjson::Value toJsonValue(const BenchmarkCatalogSnapshot &r, rapidjson::Document::AllocatorType &a) {
  rapidjson::Value out(rapidjson::kObjectType);
  rapidjson::Value list(rapidjson::kArrayType);
  for (const auto& bench : r.availableBenchmarks) { list.PushBack(jsonString(bench, a), a); }
  out.AddMember("available_benchmarks", list, a);
  return out;
}
BenchmarkCatalogSnapshot benchmarkCatalogSnapshotFromJson(const rapidjson::Value &v) {
  BenchmarkCatalogSnapshot r;
  if (v.IsObject() && v.HasMember("available_benchmarks") && v["available_benchmarks"].IsArray()) {
    for (const auto& item : v["available_benchmarks"].GetArray()) { r.availableBenchmarks.push_back(item.GetString()); }
  }
  return r;
}

} // namespace firmius::daemon
