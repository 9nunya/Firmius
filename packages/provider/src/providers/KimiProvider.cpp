#include "providers/KimiProvider.hpp"
#include "Enums.hpp"
#include "EnvLoader.hpp"
#include <curl/curl.h>
#include <iostream>
#include <rapidjson/document.h>
#include <sstream>
#include <vector>

namespace firmius::provider {

using namespace firmius::shared;

KimiProvider::KimiProvider(const std::vector<std::string>& initialKeys)
    : BaseAnthropicProvider("kimi", "https://api.kimi.com/coding/v1", "") {

  // Add initial keys if provided
  for (const auto& key : initialKeys) {
    if (!key.empty()) {
      APIKeyAccount acc;
      acc.apiKey = key;
      acc.keyPrefix = extractKeyPrefix(key);
      acc.identifier = generateIdentifier();
      addAccount(acc);
    }
  }

  // Load from environment if no keys provided
  if (getAccountCount() == 0) {
    for (int i = 1; i <= 10; ++i) {
      std::string key =
          shared::EnvLoader::get("KIMI_API_KEY_" + std::to_string(i));
      if (!key.empty()) {
        APIKeyAccount acc;
        acc.apiKey = key;
        acc.keyPrefix = extractKeyPrefix(key);
        acc.identifier = generateIdentifier();
        addAccount(acc);
      }
    }

    if (getAccountCount() == 0) {
      std::string primary = shared::EnvLoader::get("KIMI_API_KEY");
      if (!primary.empty()) {
        if (primary.find(',') != std::string::npos) {
          std::stringstream ss(primary);
          std::string item;
          while (std::getline(ss, item, ',')) {
            if (!item.empty()) {
              APIKeyAccount acc;
              acc.apiKey = item;
              acc.keyPrefix = extractKeyPrefix(item);
              acc.identifier = generateIdentifier();
              addAccount(acc);
            }
          }
        } else {
          APIKeyAccount acc;
          acc.apiKey = primary;
          acc.keyPrefix = extractKeyPrefix(primary);
          acc.identifier = generateIdentifier();
          addAccount(acc);
        }
      }
    }
  }
}

std::map<std::string, std::string> KimiProvider::getHeaders() {
  // Get base headers from parent class
  auto headers = BaseAnthropicProvider::getHeaders();
  
  // Add Kimi-specific headers
  headers["User-Agent"] = "claude-code/1.0";
  headers["X-Client-Name"] = "claude-code";
  
  return headers;
}

std::string KimiProvider::getBaseUrl() const {
  return "https://api.kimi.com/coding/v1";
}

std::vector<firmius::shared::ModelInfo> KimiProvider::listModels() {
  std::vector<ModelInfo> models;

  models.push_back({.id = "kimi-k2.5",
                    .provider = "kimi",
                    .contextWindow = 262144,
                    .modalities = {"text", "image"},
                    .variants = {},
                    .supportsReasoning = true,
                    .pricePer1MInput = 0.6,
                    .pricePer1MOutput = 3,
                    .pricePer1MCacheRead = 0,
                    .pricePer1MCacheWrite = 0.1});

  models.push_back({.id = "kimi-k2-thinking",
                    .provider = "kimi",
                    .contextWindow = 262144,
                    .modalities = {"text", "image"},
                    .variants = {},
                    .supportsReasoning = true,
                    .pricePer1MInput = 0.6,
                    .pricePer1MOutput = 3,
                    .pricePer1MCacheRead = 0,
                    .pricePer1MCacheWrite = 0.1});

  return models;
}

} // namespace firmius::provider
