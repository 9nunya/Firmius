#include "providers/ConfigurableAnthropicProvider.hpp"

#include <curl/curl.h>
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

ConfigurableAnthropicProvider::ConfigurableAnthropicProvider(
    const std::string &id, const shared::ProviderProfileConfig &profile)
    : BaseAnthropicProvider(id, profile.baseUrl, ""), profile_(profile) {}

std::map<std::string, std::string> ConfigurableAnthropicProvider::getHeaders() {
  auto headers = BaseAnthropicProvider::getHeaders();
  headers["anthropic-version"] = profile_.anthropicVersion.empty()
                                      ? "2023-06-01"
                                      : profile_.anthropicVersion;
  for (const auto &[key, value] : profile_.headers) {
    headers[key] = value;
  }
  return headers;
}

std::string ConfigurableAnthropicProvider::getMessagesUrl() const {
  return joinUrl(baseUrl, profile_.messagesEndpoint, "/v1/messages");
}

std::string ConfigurableAnthropicProvider::getAnthropicBetaHeader() const {
  return profile_.betaHeader;
}

std::string
ConfigurableAnthropicProvider::prepareRequestBody(const AgentHistory &history,
                                                  const ProviderOptions &opts) {
  std::string body = BaseAnthropicProvider::prepareRequestBody(history, opts);
  return mergeJsonObjectStrings(body, opts.modelVariantJson);
}

std::string ConfigurableAnthropicProvider::getModelsUrl() const {
  return joinUrl(baseUrl, profile_.modelsEndpoint, "/v1/models");
}

std::vector<ModelInfo> ConfigurableAnthropicProvider::listModels() {
  CURL *curl = curl_easy_init();
  if (!curl) {
    return {};
  }

  std::string response;
  auto writer = [](char *ptr, size_t size, size_t nmemb,
                   void *userdata) -> size_t {
    static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
  };

  struct curl_slist *headers = nullptr;
  auto headerMap = getHeaders();
  for (const auto &[k, v] : headerMap) {
    headers = curl_slist_append(headers, (k + ": " + v).c_str());
  }

  std::string url = getModelsUrl();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(
      curl, CURLOPT_WRITEFUNCTION,
      static_cast<size_t (*)(char *, size_t, size_t, void *)>(writer));
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

  curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  rapidjson::Document doc;
  doc.Parse(response.c_str());
  std::vector<ModelInfo> models;
  if (doc.IsObject() && doc.HasMember("data") && doc["data"].IsArray()) {
    for (const auto &item : doc["data"].GetArray()) {
      if (!item.IsObject() || !item.HasMember("id") ||
          !item["id"].IsString()) {
        continue;
      }
      ModelInfo model;
      model.id = item["id"].GetString();
      model.provider = getId();
      model.modalities = {"text"};
      models.push_back(model);
    }
  }

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
