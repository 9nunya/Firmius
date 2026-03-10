#include "providers/oauth/BaseOAuthProvider.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sys/types.h>
#include <unistd.h>

namespace firmius::provider {

namespace {
std::string getOAuthJsonPath() {
  const char *homedir;
  if ((homedir = getenv("HOME")) == NULL) {
    homedir = getpwuid(getuid())->pw_dir;
  }
  std::filesystem::path dir = std::string(homedir) + "/.firmius";
  if (!std::filesystem::exists(dir)) {
    std::filesystem::create_directories(dir);
  }
  return (dir / "oauth.json").string();
}

int64_t getNowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
} // namespace

BaseOAuthProvider::BaseOAuthProvider(std::string providerId)
    : providerId_(std::move(providerId)) {
  loadAccounts();
}

std::string BaseOAuthProvider::getId() const { return providerId_; }

bool BaseOAuthProvider::supportsStreamUsage() const {
  return true; // Assume standard streaming by default
}

firmius::provider::ProviderType BaseOAuthProvider::getProviderType() const {
  return firmius::provider::ProviderType::OAuth;
}

bool BaseOAuthProvider::isTokenExpired(const OAuthAccount &acc) const {
  // Treat as expired if we are within 5 minutes of expiration
  return getNowSeconds() >= (acc.tokenExpiration - 300);
}

void BaseOAuthProvider::markAccountRateLimited(OAuthAccount &acc,
                                               int backoffSeconds) {
  acc.rateLimited = true;
  acc.backoffUntil = getNowSeconds() + backoffSeconds;
}

std::optional<OAuthAccount *> BaseOAuthProvider::getAvailableAccount(
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
      // Reset stale quota metadata so the account is not skipped again
      for (auto &[k, v] : acc.metadata) {
        if (k.rfind("quota:", 0) == 0 && v == "0") {
          v = "1";
        }
      }
    }

    if (!acc.rateLimited) {
      // Un-expired or we can refresh it successfully
      if (!isTokenExpired(acc) || refreshAccessToken(acc)) {
        lastUsedIndex_ = currentIdx;
        saveAccounts();
        return &acc;
      }
    }

    currentIdx = (currentIdx + 1) % static_cast<int>(accounts_.size());
  } while (currentIdx != startIdx);

  // If all are rate limited, return the one closest to unlocking (or just the
  // next one as fallback)
  return std::nullopt;
}

void BaseOAuthProvider::addAccount(const OAuthAccount &acc) {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  accounts_.push_back(acc);
  saveAccounts();
}

void BaseOAuthProvider::loadAccounts() {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  accounts_.clear();
  std::string path = getOAuthJsonPath();
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
      OAuthAccount acc;
      if (item.HasMember("email") && item["email"].IsString())
        acc.email = item["email"].GetString();
      if (item.HasMember("refreshToken") && item["refreshToken"].IsString())
        acc.refreshToken = item["refreshToken"].GetString();
      if (item.HasMember("accessToken") && item["accessToken"].IsString())
        acc.accessToken = item["accessToken"].GetString();
      if (item.HasMember("tokenExpiration") &&
          item["tokenExpiration"].IsInt64())
        acc.tokenExpiration = item["tokenExpiration"].GetInt64();
      if (item.HasMember("lastQuotaRefresh") &&
          item["lastQuotaRefresh"].IsInt64())
        acc.lastQuotaRefresh = item["lastQuotaRefresh"].GetInt64();

      if (item.HasMember("metadata") && item["metadata"].IsObject()) {
        for (auto it = item["metadata"].MemberBegin();
             it != item["metadata"].MemberEnd(); ++it) {
          if (it->name.IsString() && it->value.IsString()) {
            acc.metadata[it->name.GetString()] = it->value.GetString();
          }
        }
      }

      // Do not persist in-memory rate limit state intentionally, or optionally
      // carry it over: if (item.HasMember("backoffUntil") &&
      // item["backoffUntil"].IsInt64()) acc.backoffUntil =
      // item["backoffUntil"].GetInt64();

      accounts_.push_back(acc);
    }
  }
}

void BaseOAuthProvider::saveAccounts() {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  std::string path = getOAuthJsonPath();
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
    obj.AddMember("email",
                  rapidjson::Value(acc.email.c_str(), doc.GetAllocator()),
                  doc.GetAllocator());
    obj.AddMember(
        "refreshToken",
        rapidjson::Value(acc.refreshToken.c_str(), doc.GetAllocator()),
        doc.GetAllocator());
    obj.AddMember("accessToken",
                  rapidjson::Value(acc.accessToken.c_str(), doc.GetAllocator()),
                  doc.GetAllocator());
    obj.AddMember("tokenExpiration", acc.tokenExpiration, doc.GetAllocator());
    obj.AddMember("lastQuotaRefresh", acc.lastQuotaRefresh, doc.GetAllocator());

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

void BaseOAuthProvider::deleteAccount(const std::string &identifier) {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  auto it = std::remove_if(accounts_.begin(), accounts_.end(),
                           [&](const OAuthAccount &acc) {
                             return acc.getIdentifier() == identifier;
                           });
  if (it != accounts_.end()) {
    accounts_.erase(it, accounts_.end());
    saveAccounts();
  }
}

} // namespace firmius::provider
