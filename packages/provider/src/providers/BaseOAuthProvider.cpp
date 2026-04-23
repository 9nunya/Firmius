#include "providers/BaseOAuthProvider.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <mutex>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace firmius::provider {

namespace {
constexpr auto kBackgroundQuotaRefreshInterval = std::chrono::minutes(5);
std::mutex g_oauth_json_mutex;

std::filesystem::path makeOAuthTempPath(const std::filesystem::path &path) {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return path.string() + ".tmp." + std::to_string(::getpid()) + "." +
         std::to_string(nonce);
}

std::string getOAuthJsonPath() {
  const char *homedir = getenv("HOME");
#if defined(_WIN32)
  if (homedir == nullptr || *homedir == '\0') {
    homedir = getenv("USERPROFILE");
  }
  if (homedir == nullptr || *homedir == '\0') {
    homedir = getenv("APPDATA");
  }
  if (homedir == nullptr || *homedir == '\0') {
    throw std::runtime_error("unable to resolve home directory for oauth.json");
  }
#else
  if (homedir == nullptr || *homedir == '\0') {
    passwd *pw = getpwuid(getuid());
    if (pw != nullptr && pw->pw_dir != nullptr) {
      homedir = pw->pw_dir;
    }
  }
  if (homedir == nullptr || *homedir == '\0') {
    throw std::runtime_error("unable to resolve home directory for oauth.json");
  }
#endif

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
  startBackgroundQuotaRefresh();
}

BaseOAuthProvider::~BaseOAuthProvider() { stopBackgroundQuotaRefresh(); }

void BaseOAuthProvider::startBackgroundQuotaRefresh() {
  stopQuotaRefresh_ = false;
  quotaRefreshThread_ = std::thread([this]() {
    try {
      refreshQuotas();
    } catch (...) {
    }

    while (!stopQuotaRefresh_) {
      std::unique_lock<std::mutex> lock(quotaRefreshMutex_);
      if (quotaRefreshCv_.wait_for(lock, kBackgroundQuotaRefreshInterval, [this] {
            return stopQuotaRefresh_.load();
          })) {
        break;
      }

      try {
        refreshQuotas();
      } catch (...) {
      }
    }
  });
}

void BaseOAuthProvider::stopBackgroundQuotaRefresh() {
  stopQuotaRefresh_ = true;
  quotaRefreshCv_.notify_all();
  if (quotaRefreshThread_.joinable()) {
    quotaRefreshThread_.join();
  }
}

std::string BaseOAuthProvider::getId() const { return providerId_; }

bool BaseOAuthProvider::supportsStreamUsage() const {
  return true; // Assume standard streaming by default
}

firmius::provider::ProviderType BaseOAuthProvider::getProviderType() const {
  return firmius::provider::ProviderType::OAuth;
}

bool BaseOAuthProvider::isConfigured() const {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  return !accounts_.empty();
}

std::vector<OAuthAccount> BaseOAuthProvider::getAccounts() const {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  return accounts_;
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

std::optional<OAuthAccount> BaseOAuthProvider::getAvailableAccount(
    const std::optional<std::string> & /*modelId*/) {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  if (accounts_.empty()) {
    return std::nullopt;
  }

  int startIdx = (lastUsedIndex_.load(std::memory_order_relaxed) >= 0) 
                 ? lastUsedIndex_.load(std::memory_order_relaxed) 
                 : 0;
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
      // Check token expiration but DON'T update lastUsedIndex_ yet
      // (only update on successful request to avoid sticking to failed accounts)
      if (isTokenExpired(acc) && !refreshAccessToken(acc)) {
        // Token expired and refresh failed, skip this account
        currentIdx = (currentIdx + 1) % static_cast<int>(accounts_.size());
        continue;
      }
      // Return account without updating lastUsedIndex_
      return acc;
    }

    currentIdx = (currentIdx + 1) % static_cast<int>(accounts_.size());
  } while (currentIdx != startIdx);

  // If all are rate limited, return the one closest to unlocking (or just the
  // next one as fallback)
  return std::nullopt;
}

void BaseOAuthProvider::updateAccount(const OAuthAccount &acc) {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  for (auto &existing : accounts_) {
    if (existing.getIdentifier() == acc.getIdentifier()) {
      existing = acc;
      saveAccounts();
      return;
    }
  }
}

void BaseOAuthProvider::addAccount(const OAuthAccount &acc) {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  accounts_.push_back(acc);
  saveAccounts();
}

void BaseOAuthProvider::loadAccounts() {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  std::lock_guard<std::mutex> file_lock(g_oauth_json_mutex);
  std::string path = getOAuthJsonPath();
  if (!std::filesystem::exists(std::filesystem::path(path))) {
    accounts_.clear();
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

  std::vector<OAuthAccount> loaded_accounts;

  std::string luiKey = "lastUsedIndex_" + providerId_;
  if (doc.HasMember(luiKey.c_str()) && doc[luiKey.c_str()].IsInt()) {
    lastUsedIndex_.store(doc[luiKey.c_str()].GetInt(), std::memory_order_relaxed);
  } else if (doc.HasMember("lastUsedIndex") && doc["lastUsedIndex"].IsInt()) {
    // Migration: read old global key as fallback
    lastUsedIndex_.store(doc["lastUsedIndex"].GetInt(), std::memory_order_relaxed);
  }

  if (doc.HasMember(providerId_.c_str()) &&
      doc[providerId_.c_str()].IsArray()) {
    const auto &arr = doc[providerId_.c_str()];
    for (rapidjson::SizeType i = 0; i < arr.Size(); i++) {
      const auto &item = arr[i];
      OAuthAccount acc;
      // Migration: load from "identifier" (new) or "email" (legacy)
      if (item.HasMember("identifier") && item["identifier"].IsString()) {
        acc.identifier = item["identifier"].GetString();
      } else if (item.HasMember("email") && item["email"].IsString()) {
        acc.identifier = item["email"].GetString();
      }
      // If identifier is empty, generate from access token hash
      if (acc.identifier.empty() && item.HasMember("accessToken") && item["accessToken"].IsString()) {
        std::string token = item["accessToken"].GetString();
        if (!token.empty()) {
          // Use first 12 chars of token as temporary identifier
          acc.identifier = token.substr(0, std::min(size_t(12), token.size()));
        }
      }
      if (item.HasMember("refreshToken") && item["refreshToken"].IsString())
        acc.refreshToken = item["refreshToken"].GetString();
      
      // Legacy migration for providers that encoded project IDs into the
      // refresh token as refreshToken|projectId|managedProjectId.
      if (providerId_ == "antigravity") {
        size_t firstPipe = acc.refreshToken.find('|');
        if (firstPipe != std::string::npos) {
          std::string actualRefreshToken = acc.refreshToken.substr(0, firstPipe);
          std::string remaining = acc.refreshToken.substr(firstPipe + 1);
          acc.refreshToken = actualRefreshToken;

          size_t secondPipe = remaining.find('|');
          std::string projectId, managedProjectId;
          if (secondPipe != std::string::npos) {
            projectId = remaining.substr(0, secondPipe);
            managedProjectId = remaining.substr(secondPipe + 1);
          } else {
            projectId = remaining;
          }

          if (!projectId.empty() && !acc.metadata.count("projectId"))
            acc.metadata["projectId"] = projectId;
          if (!managedProjectId.empty() &&
              !acc.metadata.count("managedProjectId"))
            acc.metadata["managedProjectId"] = managedProjectId;
        }
      }
      
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

      loaded_accounts.push_back(std::move(acc));
    }
  }
  accounts_ = std::move(loaded_accounts);
}

void BaseOAuthProvider::saveAccounts() {
  std::lock_guard<std::recursive_mutex> accounts_lock(accountsMutex_);
  std::lock_guard<std::mutex> file_lock(g_oauth_json_mutex);

  const auto accountsSnapshot = accounts_;
  const int lastUsedIdx = lastUsedIndex_.load(std::memory_order_relaxed);
  const std::filesystem::path finalPath = getOAuthJsonPath();
  rapidjson::Document doc;

  // Load existing file to not overwrite other providers
  if (std::filesystem::exists(finalPath)) {
    std::ifstream ifs(finalPath);
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
  for (const auto &acc : accountsSnapshot) {
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("identifier",
                  rapidjson::Value(acc.identifier.c_str(), doc.GetAllocator()),
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

  std::string luiKey = "lastUsedIndex_" + providerId_;
  if (doc.HasMember(luiKey.c_str())) {
    doc.RemoveMember(luiKey.c_str());
  }
  {
    rapidjson::Value name(luiKey.c_str(), doc.GetAllocator());
    doc.AddMember(name, lastUsedIdx, doc.GetAllocator());
  }
  // Also clean up old global key if present
  if (doc.HasMember("lastUsedIndex")) {
    doc.RemoveMember("lastUsedIndex");
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  const auto parent = finalPath.parent_path();
  std::error_code ec;
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      throw std::runtime_error("Cannot prepare OAuth state directory: " +
                               parent.string() + " (" + ec.message() + ")");
    }
  }

  const std::filesystem::path tempPath = makeOAuthTempPath(finalPath);
  {
    std::ofstream ofs(tempPath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
      throw std::runtime_error("Cannot open temporary OAuth state file: " +
                               tempPath.string());
    }
    ofs << buffer.GetString();
    ofs.flush();
    if (!ofs) {
      std::filesystem::remove(tempPath, ec);
      throw std::runtime_error("Cannot write temporary OAuth state file: " +
                               tempPath.string());
    }
  }

  std::filesystem::rename(tempPath, finalPath, ec);
  if (ec) {
    std::error_code cleanupEc;
    std::filesystem::remove(tempPath, cleanupEc);
    throw std::runtime_error("Cannot replace OAuth state file: " +
                             finalPath.string() + " (" + ec.message() + ")");
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
