#include "providers/NvidiaProvider.hpp"

#include "EnvLoader.hpp"

#include <curl/curl.h>
#include <rapidjson/document.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace firmius::provider {

namespace {

size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *buffer = static_cast<std::string *>(userdata);
  buffer->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string collapseWhitespace(const std::string &input) {
  std::string result;
  result.reserve(input.size());

  bool inWhitespace = false;
  for (unsigned char c : input) {
    if (std::isspace(c)) {
      if (!inWhitespace && !result.empty()) {
        result.push_back(' ');
      }
      inWhitespace = true;
      continue;
    }
    inWhitespace = false;
    result.push_back(static_cast<char>(c));
  }

  while (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }
  return result;
}

std::string stripHtmlTags(const std::string &html) {
  std::string text;
  text.reserve(html.size());

  bool inTag = false;
  for (char c : html) {
    if (c == '<') {
      inTag = true;
      if (!text.empty() && text.back() != ' ') {
        text.push_back(' ');
      }
      continue;
    }
    if (c == '>') {
      inTag = false;
      continue;
    }
    if (!inTag) {
      if (c == '&') {
        text.push_back(' ');
        continue;
      }
      text.push_back(c);
    }
  }

  return collapseWhitespace(text);
}

std::optional<std::string> extractField(const std::string &text,
                                        const std::regex &pattern) {
  std::smatch match;
  if (!std::regex_search(text, match, pattern) || match.size() < 2) {
    return std::nullopt;
  }
  return collapseWhitespace(match[1].str());
}

std::uint32_t parseTokenCount(const std::optional<std::string> &value) {
  if (!value || value->empty()) {
    return 0;
  }

  std::string digits;
  digits.reserve(value->size());
  for (char c : *value) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      digits.push_back(c);
    }
  }
  if (digits.empty()) {
    return 0;
  }

  try {
    return static_cast<std::uint32_t>(std::stoul(digits));
  } catch (...) {
    return 0;
  }
}

std::vector<std::string> parseModalities(const std::optional<std::string> &value) {
  std::vector<std::string> modalities;
  if (!value) {
    return {"text"};
  }

  const std::string lowered = toLower(*value);
  if (lowered.find("text") != std::string::npos) {
    modalities.push_back("text");
  }
  if (lowered.find("image") != std::string::npos ||
      lowered.find("vision") != std::string::npos) {
    modalities.push_back("image");
  }
  if (lowered.find("audio") != std::string::npos) {
    modalities.push_back("audio");
  }
  if (lowered.find("video") != std::string::npos) {
    modalities.push_back("video");
  }

  if (modalities.empty()) {
    modalities.push_back("text");
  }
  return modalities;
}

bool supportsTextOutput(const std::optional<std::string> &value) {
  if (!value) {
    return false;
  }
  return toLower(*value).find("text") != std::string::npos;
}

bool hasReasoningSignals(const std::string &modelId, const std::string &plainText,
                         const std::string &html) {
  const std::string loweredModelId = toLower(modelId);
  if (loweredModelId.find("reason") != std::string::npos ||
      loweredModelId.find("thinking") != std::string::npos) {
    return true;
  }

  const std::string loweredText = toLower(plainText);
  if (loweredText.find(" reasoning ") != std::string::npos ||
      loweredText.find("reasoning.") != std::string::npos ||
      loweredText.find("reasoning,") != std::string::npos ||
      loweredText.find("agentic") != std::string::npos) {
    return true;
  }

  return html.find(">Reasoning<") != std::string::npos ||
         html.find("reasoning_content") != std::string::npos;
}

std::optional<firmius::shared::ModelInfo>
parseModelCard(const std::string &providerId, const std::string &modelId,
               const std::string &html) {
  if (html.empty()) {
    return std::nullopt;
  }

  const std::string plainText = stripHtmlTags(html);
  const auto inputTypes = extractField(
      plainText,
      std::regex(R"(Input Types:\s*(.*?)\s+(?:Other Input Properties:|Input Context Length \(ISL\):|Output Types:))"));
  const auto outputTypes = extractField(
      plainText,
      std::regex(R"(Output Types:\s*(.*?)\s+(?:Other Output Properties:|Output Context Length \(OSL\):|Evaluation Properties:|Overview))"));

  if (!supportsTextOutput(outputTypes)) {
    return std::nullopt;
  }

  firmius::shared::ModelInfo info;
  info.id = modelId;
  info.provider = providerId;
  info.contextWindow = parseTokenCount(extractField(
      plainText, std::regex(R"(Input Context Length \(ISL\):\s*([0-9,]+))")));
  info.maxOutputTokens = parseTokenCount(extractField(
      plainText, std::regex(R"(Output Context Length \(OSL\):\s*([0-9,]+))")));
  info.modalities = parseModalities(inputTypes);
  info.supportsReasoning = hasReasoningSignals(modelId, plainText, html);
  return info;
}

} // namespace

NvidiaProvider::NvidiaProvider(const std::string &apiKey)
    : BaseOpenAIProvider("nvidia", "https://integrate.api.nvidia.com/v1",
                         apiKey) {
  if (getAccountCount() != 0) {
    return;
  }

  const std::vector<std::string> multiKeyPrefixes = {"NVIDIA_NIM_API_KEY_",
                                                     "NVIDIA_API_KEY_",
                                                     "NIM_API_KEY_"};
  for (const auto &prefix : multiKeyPrefixes) {
    for (int i = 1; i <= 10; ++i) {
      const std::string key =
          shared::EnvLoader::get(prefix + std::to_string(i));
      if (key.empty()) {
        continue;
      }
      APIKeyAccount account;
      account.apiKey = key;
      account.keyPrefix = extractKeyPrefix(key);
      account.identifier = generateIdentifier();
      addAccount(account);
    }
  }

  if (getAccountCount() != 0) {
    return;
  }

  for (const char *var :
       {"NVIDIA_NIM_API_KEY", "NVIDIA_API_KEY", "NIM_API_KEY"}) {
    const std::string key = shared::EnvLoader::get(var);
    if (key.empty()) {
      continue;
    }

    APIKeyAccount account;
    account.apiKey = key;
    account.keyPrefix = extractKeyPrefix(key);
    account.identifier = generateIdentifier();
    addAccount(account);
    break;
  }
}

std::string NvidiaProvider::fetchUrl(
    const std::string &url,
    const std::map<std::string, std::string> &headers) const {
  CURL *curl = curl_easy_init();
  if (!curl) {
    return "";
  }

  std::string response;
  struct curl_slist *curlHeaders = nullptr;
  for (const auto &[key, value] : headers) {
    curlHeaders = curl_slist_append(curlHeaders, (key + ": " + value).c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curlHeaders);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Firmius/1.0");

  curl_easy_perform(curl);
  curl_slist_free_all(curlHeaders);
  curl_easy_cleanup(curl);
  return response;
}

std::vector<std::string> NvidiaProvider::fetchModelIds() {
  std::set<std::string> uniqueIds;

  rapidjson::Document document;
  document.Parse(fetchUrl(baseUrl + "/models", getHeaders()).c_str());
  if (!document.IsObject() || !document.HasMember("data") ||
      !document["data"].IsArray()) {
    return {};
  }

  for (const auto &entry : document["data"].GetArray()) {
    if (!entry.IsObject() || !entry.HasMember("id") || !entry["id"].IsString()) {
      continue;
    }
    uniqueIds.insert(entry["id"].GetString());
  }

  return {uniqueIds.begin(), uniqueIds.end()};
}

std::optional<firmius::shared::ModelInfo>
NvidiaProvider::fetchModelInfo(const std::string &modelId) {
  const std::size_t slash = modelId.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 >= modelId.size()) {
    return std::nullopt;
  }

  const std::string publisher = modelId.substr(0, slash);
  const std::string modelName = modelId.substr(slash + 1);
  const std::string url =
      "https://build.nvidia.com/" + publisher + "/" + modelName + "/modelcard";

  return parseModelCard(getId(), modelId,
                        fetchUrl(url, {{"Accept", "text/html"}}));
}

void NvidiaProvider::discoverModels(
    std::function<void(const firmius::shared::ModelInfo &)> onModel) {
  {
    std::lock_guard<std::mutex> lock(modelCacheMutex_);
    if (modelCacheLoaded_) {
      for (const auto &model : modelCache_) {
        onModel(model);
      }
      return;
    }
  }

  std::vector<firmius::shared::ModelInfo> discovered;
  for (const auto &modelId : fetchModelIds()) {
    auto model = fetchModelInfo(modelId);
    if (!model.has_value()) {
      continue;
    }
    discovered.push_back(*model);
    onModel(*model);
  }

  std::lock_guard<std::mutex> lock(modelCacheMutex_);
  modelCache_ = discovered;
  modelCacheLoaded_ = true;
}

std::vector<firmius::shared::ModelInfo> NvidiaProvider::listModels() {
  std::vector<firmius::shared::ModelInfo> models;
  discoverModels([&models](const firmius::shared::ModelInfo &model) {
    models.push_back(model);
  });
  return models;
}

} // namespace firmius::provider
