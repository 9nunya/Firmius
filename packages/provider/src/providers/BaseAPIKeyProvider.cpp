#include "providers/BaseAPIKeyProvider.hpp"
#include "providers/BackoffConstants.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::provider {

namespace {
std::string getKeysJsonPath() {
  const char *homedir;
  if ((homedir = getenv("HOME")) == NULL) {
    homedir = getpwuid(getuid())->pw_dir;
  }
  std::filesystem::path dir = std::string(homedir) + "/.firmius";
  if (!std::filesystem::exists(dir)) {
    std::filesystem::create_directories(dir);
  }
  return (dir / "keys.json").string();
}
} // namespace

BaseAPIKeyProvider::BaseAPIKeyProvider(std::string providerId)
    : providerId_(std::move(providerId)) {
  loadAccounts();
}

std::string BaseAPIKeyProvider::getId() const { return providerId_; }

firmius::provider::ProviderType BaseAPIKeyProvider::getProviderType() const {
  return firmius::provider::ProviderType::APIKey;
}

bool BaseAPIKeyProvider::isConfigured() const {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  return !accounts_.empty();
}

int64_t BaseAPIKeyProvider::getNowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string BaseAPIKeyProvider::extractKeyPrefix(const std::string &apiKey) {
  if (apiKey.empty()) {
    return "?????";
  }
  return apiKey.substr(0, std::min(size_t(5), apiKey.size()));
}

std::string BaseAPIKeyProvider::generateIdentifier() const {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  int nextNum = static_cast<int>(accounts_.size()) + 1;
  return "Key #" + std::to_string(nextNum);
}

void BaseAPIKeyProvider::markAccountRateLimited(APIKeyAccount &acc,
                                                 int backoffSeconds) {
  acc.rateLimited = true;
  acc.backoffUntil = getNowSeconds() + backoffSeconds;
}

std::optional<APIKeyAccount *> BaseAPIKeyProvider::getAvailableAccount(
    const std::optional<std::string> & /*modelId*/) {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  if (accounts_.empty()) {
    return std::nullopt;
  }

  int startIdx = (lastUsedIndex_ >= 0) ? lastUsedIndex_ : 0;
  if (startIdx >= static_cast<int>(accounts_.size())) {
    startIdx = 0;
  }
  int currentIdx = startIdx;
  int64_t now = getNowSeconds();

  do {
    auto &acc = accounts_[currentIdx];

    // Reset rate limit if backoff expired
    if (acc.rateLimited && now > acc.backoffUntil) {
      acc.rateLimited = false;
    }

    if (!acc.rateLimited) {
      // Update lastUsedIndex_ for round-robin rotation
      lastUsedIndex_ = currentIdx;
      return &acc;
    }

    currentIdx = (currentIdx + 1) % static_cast<int>(accounts_.size());
  } while (currentIdx != startIdx);

  // All accounts rate-limited - return nullopt
  return std::nullopt;
}

std::vector<APIKeyAccount> BaseAPIKeyProvider::getAccounts() const {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  return accounts_;
}

size_t BaseAPIKeyProvider::getAccountCount() const {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  return accounts_.size();
}

void BaseAPIKeyProvider::addAccount(const APIKeyAccount &acc) {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  accounts_.push_back(acc);
  saveAccounts();
}

void BaseAPIKeyProvider::addApiKey(const std::string &apiKey) {
  if (apiKey.empty())
    return;
  APIKeyAccount acc;
  acc.apiKey = apiKey;
  acc.keyPrefix = extractKeyPrefix(apiKey);
  acc.identifier = generateIdentifier();
  addAccount(acc);
}

void BaseAPIKeyProvider::deleteAccount(const std::string &identifier) {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  auto it = std::remove_if(accounts_.begin(), accounts_.end(),
                           [&](const APIKeyAccount &acc) {
                             return acc.getIdentifier() == identifier;
                           });
  if (it != accounts_.end()) {
    accounts_.erase(it, accounts_.end());
    saveAccounts();
  }
}

void BaseAPIKeyProvider::loadAccounts() {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  accounts_.clear();
  std::string path = getKeysJsonPath();
  if (!std::filesystem::exists(std::filesystem::path(path))) {
    return;
  }

  std::ifstream ifs(path);
  if (!ifs.is_open())
    return;

  std::string content((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
  rapidjson::Document doc;
  doc.Parse(content.c_str());

  if (doc.HasParseError() || !doc.IsObject())
    return;

  if (doc.HasMember("lastUsedIndex") && doc["lastUsedIndex"].IsInt()) {
    lastUsedIndex_ = doc["lastUsedIndex"].GetInt();
  }

  if (doc.HasMember(providerId_.c_str()) &&
      doc[providerId_.c_str()].IsArray()) {
    const auto &arr = doc[providerId_.c_str()];
    for (rapidjson::SizeType i = 0; i < arr.Size(); i++) {
      const auto &item = arr[i];
      APIKeyAccount acc;
      if (item.HasMember("identifier") && item["identifier"].IsString()) {
        acc.identifier = item["identifier"].GetString();
      }
      if (item.HasMember("keyPrefix") && item["keyPrefix"].IsString()) {
        acc.keyPrefix = item["keyPrefix"].GetString();
      }
      if (item.HasMember("apiKey") && item["apiKey"].IsString()) {
        acc.apiKey = item["apiKey"].GetString();
      }
      if (item.HasMember("metadata") && item["metadata"].IsObject()) {
        for (auto it = item["metadata"].MemberBegin();
             it != item["metadata"].MemberEnd(); ++it) {
          if (it->name.IsString() && it->value.IsString()) {
            acc.metadata[it->name.GetString()] = it->value.GetString();
          }
        }
      }

      // Ensure keyPrefix is set if missing
      if (acc.keyPrefix.empty() && !acc.apiKey.empty()) {
        acc.keyPrefix = extractKeyPrefix(acc.apiKey);
      }

      // Generate identifier if missing
      if (acc.identifier.empty()) {
        acc.identifier = generateIdentifier();
      }

      accounts_.push_back(acc);
    }
  }
}

void BaseAPIKeyProvider::saveAccounts() {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  std::string path = getKeysJsonPath();
  rapidjson::Document doc;

  // Load existing file to not overwrite other providers
  if (std::filesystem::exists(std::filesystem::path(path))) {
    std::ifstream ifs(path);
    if (ifs.is_open()) {
      std::string content((std::istreambuf_iterator<char>(ifs)),
                          std::istreambuf_iterator<char>());
      doc.Parse(content.c_str());
    }
  }

  if (doc.HasParseError() || !doc.IsObject()) {
    doc.SetObject();
  }

  rapidjson::Value arr(rapidjson::kArrayType);
  for (const auto &acc : accounts_) {
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("identifier",
                  rapidjson::Value(acc.identifier.c_str(), doc.GetAllocator()),
                  doc.GetAllocator());
    obj.AddMember("keyPrefix",
                  rapidjson::Value(acc.keyPrefix.c_str(), doc.GetAllocator()),
                  doc.GetAllocator());
    obj.AddMember("apiKey",
                  rapidjson::Value(acc.apiKey.c_str(), doc.GetAllocator()),
                  doc.GetAllocator());

    if (!acc.metadata.empty()) {
      rapidjson::Value meta(rapidjson::kObjectType);
      for (const auto &[k, v] : acc.metadata) {
        meta.AddMember(rapidjson::Value(k.c_str(), doc.GetAllocator()),
                       rapidjson::Value(v.c_str(), doc.GetAllocator()),
                       doc.GetAllocator());
      }
      obj.AddMember("metadata", meta, doc.GetAllocator());
    }

    arr.PushBack(obj, doc.GetAllocator());
  }

  if (doc.HasMember(providerId_.c_str())) {
    doc.RemoveMember(providerId_.c_str());
  }
  doc.AddMember(rapidjson::Value(providerId_.c_str(), doc.GetAllocator()), arr,
                doc.GetAllocator());

  if (doc.HasMember("lastUsedIndex")) {
    doc.RemoveMember("lastUsedIndex");
  }
  doc.AddMember("lastUsedIndex", lastUsedIndex_, doc.GetAllocator());

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  std::ofstream ofs(path);
  if (ofs.is_open()) {
    ofs << buffer.GetString();
  }
}

} // namespace firmius::provider
