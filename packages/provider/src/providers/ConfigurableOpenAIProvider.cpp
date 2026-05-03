#include "providers/ConfigurableOpenAIProvider.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::provider {

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
    : BaseOpenAIProvider(id, profile.baseUrl, ""), profile_(profile) {}

std::map<std::string, std::string> ConfigurableOpenAIProvider::getHeaders() {
  auto headers = BaseOpenAIProvider::getHeaders();
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
    auto it = profile_.modelVariants.find(model.id);
    if (it == profile_.modelVariants.end()) {
      continue;
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
  return models;
}

} // namespace firmius::provider
