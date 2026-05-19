#include "providers/ConfigurableOpenAIProvider.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::provider {

using namespace firmius::shared;

namespace {

std::string joinUrl(const std::string &baseUrl, const std::string &endpoint,
                    const std::string &fallback) {
  const std::string path = endpoint.empty() ? fallback : endpoint;
  if (baseUrl.empty()) {
    return path;
  }

  const bool baseEndsWithSlash = !baseUrl.empty() && baseUrl.back() == '/';
  const bool pathStartsWithSlash = !path.empty() && path.front() == '/';

  if (baseEndsWithSlash && pathStartsWithSlash) {
    return baseUrl + path.substr(1);
  }
  if (!baseEndsWithSlash && !pathStartsWithSlash) {
    return baseUrl + "/" + path;
  }
  return baseUrl + path;
}

void mergeObjectInto(rapidjson::Value &target, const rapidjson::Value &patch,
                     rapidjson::Document::AllocatorType &allocator) {
  if (!target.IsObject() || !patch.IsObject()) {
    return;
  }

  for (auto it = patch.MemberBegin(); it != patch.MemberEnd(); ++it) {
    const std::string key = it->name.GetString();
    if (target.HasMember(key.c_str()) && target[key.c_str()].IsObject() &&
        it->value.IsObject()) {
      mergeObjectInto(target[key.c_str()], it->value, allocator);
      continue;
    }

    rapidjson::Value name(it->name.GetString(), allocator);
    rapidjson::Value value;
    value.CopyFrom(it->value, allocator);

    if (target.HasMember(key.c_str())) {
      target[key.c_str()] = value;
    } else {
      target.AddMember(name, value, allocator);
    }
  }
}

std::string mergeJsonObjectStrings(const std::string &baseJson,
                                   const std::string &patchJson) {
  if (patchJson.empty()) {
    return baseJson;
  }

  rapidjson::Document baseDoc;
  baseDoc.Parse(baseJson.c_str());
  if (baseDoc.HasParseError() || !baseDoc.IsObject()) {
    return baseJson;
  }

  rapidjson::Document patchDoc;
  patchDoc.Parse(patchJson.c_str());
  if (patchDoc.HasParseError() || !patchDoc.IsObject()) {
    return baseJson;
  }

  mergeObjectInto(baseDoc, patchDoc, baseDoc.GetAllocator());

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  baseDoc.Accept(writer);
  return buffer.GetString();
}

} // namespace

ConfigurableOpenAIProvider::ConfigurableOpenAIProvider(
    const std::string &id, const shared::ProviderProfileConfig &profile)
    : BaseOpenAIProvider(id, profile.baseUrl, ""), profile_(profile) {
  fallbackAccount_.identifier = "default";
  fallbackAccount_.keyPrefix = "cfg";
  fallbackAccount_.apiKey = profile_.defaultApiKey;
}

std::unique_ptr<APIKeyWizard> ConfigurableOpenAIProvider::beginConnectionWizard() {
  if (profile_.authMode == "none") {
    return nullptr;
  }
  return BaseOpenAIProvider::beginConnectionWizard();
}

bool ConfigurableOpenAIProvider::isConfigured() const {
  if (profile_.authMode == "none") {
    return true;
  }
  if (!profile_.defaultApiKey.empty()) {
    return true;
  }
  if (profile_.allowMissingApiKey) {
    return true;
  }
  return BaseOpenAIProvider::isConfigured();
}

std::optional<APIKeyAccount *>
ConfigurableOpenAIProvider::getAvailableAccount(
    const std::optional<std::string> &modelId) {
  auto baseAccount = BaseOpenAIProvider::getAvailableAccount(modelId);
  if (baseAccount.has_value()) {
    return baseAccount;
  }
  if (profile_.authMode == "none" || profile_.allowMissingApiKey) {
    fallbackAccount_.apiKey.clear();
    return &fallbackAccount_;
  }
  if (!profile_.defaultApiKey.empty()) {
    fallbackAccount_.apiKey = profile_.defaultApiKey;
    return &fallbackAccount_;
  }
  return std::nullopt;
}

std::map<std::string, std::string> ConfigurableOpenAIProvider::getHeaders() {
  auto headers = BaseOpenAIProvider::getHeaders();
  for (const auto &[key, value] : profile_.headers) {
    headers[key] = value;
  }
  return headers;
}

std::map<std::string, std::string>
ConfigurableOpenAIProvider::buildHeadersForApiKey(const std::string &apiKey) {
  if (profile_.authMode == "none" || profile_.allowMissingApiKey) {
    std::map<std::string, std::string> headers{{"Content-Type",
                                                "application/json"}};
    for (const auto &[key, value] : profile_.headers) {
      headers[key] = value;
    }
    return headers;
  }
  const std::string effectiveKey =
      !apiKey.empty() ? apiKey : profile_.defaultApiKey;
  auto headers = BaseOpenAIProvider::buildHeadersForApiKey(effectiveKey);
  for (const auto &[key, value] : profile_.headers) {
    headers[key] = value;
  }
  return headers;
}

std::string ConfigurableOpenAIProvider::getReasoningFieldName() const {
  if (!profile_.reasoningFieldName.empty()) {
    return profile_.reasoningFieldName;
  }
  return BaseOpenAIProvider::getReasoningFieldName();
}

std::string ConfigurableOpenAIProvider::getChatUrl() const {
  return joinUrl(baseUrl, profile_.chatEndpoint, "/chat/completions");
}

std::string ConfigurableOpenAIProvider::getModelsUrl() const {
  return joinUrl(baseUrl, profile_.modelsEndpoint, "/models");
}

bool ConfigurableOpenAIProvider::supportsStreamUsage() const {
  return profile_.defaults.streamUsage;
}

std::string
ConfigurableOpenAIProvider::prepareRequestBody(const AgentHistory &history,
                                               const ProviderOptions &opts) {
  std::string body = BaseOpenAIProvider::prepareRequestBody(history, opts);
  return mergeJsonObjectStrings(body, opts.modelVariantJson);
}

std::vector<ModelInfo> ConfigurableOpenAIProvider::listModels() {
  auto models = BaseOpenAIProvider::listModels();
  for (auto &model : models) {
    applyModelOverrides(model);
  }
  for (const auto &[modelId, cfg] : profile_.modelVariants) {
    auto it = std::find_if(models.begin(), models.end(),
                           [&](const ModelInfo &model) {
                             return model.id == modelId;
                           });
    if (it != models.end()) {
      continue;
    }
    ModelInfo model;
    model.id = modelId;
    model.provider = getId();
    applyModelOverrides(model);
    models.push_back(std::move(model));
  }
  return models;
}

ModelInfo ConfigurableOpenAIProvider::getModelInfo(const std::string &modelId) {
  auto model = BaseOpenAIProvider::getModelInfo(modelId);
  applyModelOverrides(model);
  return model;
}

void ConfigurableOpenAIProvider::applyModelOverrides(ModelInfo &model) const {
  auto it = profile_.modelVariants.find(model.id);
  if (it == profile_.modelVariants.end()) {
    return;
  }
  if (it->second.overrideContextWindow) {
    model.contextWindow = it->second.contextWindow;
  }
  if (it->second.overrideMaxOutputTokens) {
    model.maxOutputTokens = it->second.maxOutputTokens;
  }
  if (it->second.overrideModalities) {
    model.modalities = it->second.modalities;
  }
  if (it->second.overrideSupportsReasoning) {
    model.supportsReasoning = it->second.supportsReasoning;
  }

  model.variants.clear();
  for (const auto &[variantName, variantConfig] : it->second.variants) {
    if (variantName.empty()) {
      continue;
    }
    ModelVariant variant;
    variant.variantName = variantName;
    variant.extraMetadataJson = variantConfig.requestJson;
    model.variants.push_back(variant);
  }
}

} // namespace firmius::provider
