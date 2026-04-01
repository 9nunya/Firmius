#include "WebRunner.hpp"

#include "Panic.hpp"
#include "WebState.hpp"
#include "ConfigLoader.hpp"
#include "Enums.hpp"
#include "harness/Harness.hpp"
#include "providers/BaseAPIKeyProvider.hpp"
#include "providers/BaseOAuthProvider.hpp"
#include "providers/ProviderRegistry.hpp"
#include "workflow/WorkflowLoader.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/drogon.h>

#include <json/json.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>

namespace firmius::web {
namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;

struct ConnectionWizardSession {
  enum class Kind { OAuth, APIKey };

  std::string sessionId;
  std::string providerId;
  Kind kind = Kind::OAuth;
  std::unique_ptr<firmius::OAuthWizard> oauthWizard;
  std::unique_ptr<firmius::provider::APIKeyWizard> apiKeyWizard;
  std::string lastPrompt;
  bool lastPromptSecret = false;
};

std::mutex wizardMutex;
std::unordered_map<std::string, ConnectionWizardSession> wizardSessions;
std::uint64_t nextWizardId = 1;

std::filesystem::path executablePath() {
  char buffer[4096];
  const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (len <= 0) {
    return {};
  }
  buffer[len] = '\0';
  return std::filesystem::path(buffer);
}

std::vector<std::filesystem::path> candidateWebRoots() {
  std::vector<std::filesystem::path> roots;
  if (const char *env = std::getenv("FIRMIUS_WEB_DIST_DIR"); env && *env) {
    roots.emplace_back(env);
  }
#ifdef FIRMIUS_WEB_FRONTEND_DIST
  roots.emplace_back(FIRMIUS_WEB_FRONTEND_DIST);
#endif
#ifdef FIRMIUS_WEB_FRONTEND_INSTALL_DIR
  roots.emplace_back(FIRMIUS_WEB_FRONTEND_INSTALL_DIR);
#endif
  const std::filesystem::path exe = executablePath();
  if (!exe.empty()) {
    const std::filesystem::path binDir = exe.parent_path();
    roots.push_back(binDir / "../share/firmius/web");
    roots.push_back(binDir / "share/firmius/web");
    roots.push_back(binDir / "web");
  }
  return roots;
}

std::filesystem::path frontendDistPath() {
  for (const auto &root : candidateWebRoots()) {
    if (root.empty()) {
      continue;
    }
    std::error_code ec;
    const auto indexPath = root / "index.html";
    if (std::filesystem::exists(indexPath, ec) &&
        std::filesystem::is_regular_file(indexPath, ec)) {
      return root.lexically_normal();
    }
  }
  return {};
}

Json::Value parseRequestJson(const drogon::HttpRequestPtr &req) {
  Json::CharReaderBuilder builder;
  Json::Value parsed(Json::objectValue);
  std::string errs;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  const std::string_view body = req->body();
  if (body.empty()) {
    return parsed;
  }
  reader->parse(body.data(), body.data() + body.size(), &parsed, &errs);
  return parsed;
}

std::optional<std::string> jsonString(const Json::Value &json, const char *key) {
  if (!json.isObject() || !json.isMember(key) || !json[key].isString()) {
    return std::nullopt;
  }
  return json[key].asString();
}

Json::Value successPayload(Json::Value payload = Json::Value(Json::objectValue)) {
  payload["ok"] = true;
  return payload;
}

Json::Value errorPayload(const std::string &message) {
  Json::Value payload(Json::objectValue);
  payload["ok"] = false;
  payload["error"] = message;
  return payload;
}

void replyJson(Callback &&callback, const Json::Value &payload,
               drogon::HttpStatusCode code = drogon::k200OK) {
  auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
  response->setStatusCode(code);
  callback(response);
}

std::uint64_t parseUnsigned(const std::string &value) {
  if (value.empty()) {
    return 0;
  }
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

std::string toJsonString(const Json::Value &value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

Json::Value serializeConnectSession(const ConnectionWizardSession &session,
                                    bool complete = false,
                                    bool success = false,
                                    const std::string &message = "",
                                    const std::string &error = "") {
  Json::Value payload(Json::objectValue);
  payload["sessionId"] = session.sessionId;
  payload["providerId"] = session.providerId;
  payload["wizardType"] =
      session.kind == ConnectionWizardSession::Kind::OAuth ? "OAuth" : "APIKey";
  payload["complete"] = complete;
  payload["success"] = success;
  payload["prompt"] = session.lastPrompt;
  payload["isSecret"] = session.lastPromptSecret;
  payload["message"] = message;
  payload["error"] = error;
  return payload;
}

Json::Value finalizeOAuthSession(ConnectionWizardSession &session) {
  std::string error;
  if (!session.oauthWizard->finalizeExchange(error)) {
    return serializeConnectSession(session, true, false, "", error);
  }
  return serializeConnectSession(session, true, true,
                                 session.oauthWizard->getFinalMessage(), "");
}

Json::Value finalizeAPIKeySession(ConnectionWizardSession &session) {
  std::string apiKey;
  std::string error;
  if (!session.apiKeyWizard->finalizeExchange(apiKey, error)) {
    return serializeConnectSession(session, true, false, "", error);
  }

  auto provider =
      firmius::provider::ProviderRegistry::instance().getProvider(session.providerId);
  auto apiKeyProvider =
      std::dynamic_pointer_cast<firmius::provider::BaseAPIKeyProvider>(provider);
  if (!apiKeyProvider) {
    return serializeConnectSession(session, true, false, "",
                                   "Provider no longer supports API key setup");
  }
  if (!apiKey.empty()) {
    apiKeyProvider->addApiKey(apiKey);
  }
  return serializeConnectSession(session, true, true,
                                 session.apiKeyWizard->getFinalMessage(), "");
}

firmius::shared::ThreadPermissionMode parsePermissionMode(
    const std::string &value) {
  if (value == "AlwaysAllow") {
    return firmius::shared::ThreadPermissionMode::AlwaysAllow;
  }
  if (value == "DenyAll") {
    return firmius::shared::ThreadPermissionMode::DenyAll;
  }
  return firmius::shared::ThreadPermissionMode::Request;
}

firmius::shared::PermissionResponse parsePermissionResponse(
    const std::string &value) {
  if (value == "AllowOnce") {
    return firmius::shared::PermissionResponse::AllowOnce;
  }
  if (value == "AllowAlways") {
    return firmius::shared::PermissionResponse::AllowAlways;
  }
  return firmius::shared::PermissionResponse::Deny;
}

firmius::shared::UserConfig parseUserConfig(const Json::Value &json,
                                            const firmius::shared::UserConfig &base) {
  using firmius::shared::ModelRouteCategory;
  using firmius::shared::UserConfig;

  UserConfig config = base;
  if (!json.isObject()) {
    return config;
  }

  if (json.isMember("defaultProviderId") && json["defaultProviderId"].isString()) {
    config.defaultProviderId = json["defaultProviderId"].asString();
  }
  if (json.isMember("defaultModelId") && json["defaultModelId"].isString()) {
    config.defaultModelId = json["defaultModelId"].asString();
  }
  if (json.isMember("defaultModelVariant") &&
      json["defaultModelVariant"].isString()) {
    config.defaultModelVariant = json["defaultModelVariant"].asString();
  }
  if (json.isMember("defaultLeadPersona") &&
      json["defaultLeadPersona"].isString()) {
    config.defaultLeadPersona = json["defaultLeadPersona"].asString();
  }
  if (json.isMember("defaultTemperature") &&
      json["defaultTemperature"].isNumeric()) {
    config.defaultTemperature = json["defaultTemperature"].asFloat();
  }
  if (json.isMember("defaultMaxTokens")) {
    if (json["defaultMaxTokens"].isUInt()) {
      config.defaultMaxTokens = json["defaultMaxTokens"].asUInt();
    } else if (json["defaultMaxTokens"].isNull()) {
      config.defaultMaxTokens.reset();
    }
  }
  if (json.isMember("dangerouslySkipPermissions") &&
      json["dangerouslySkipPermissions"].isBool()) {
    config.dangerouslySkipPermissions =
        json["dangerouslySkipPermissions"].asBool();
  }
  if (json.isMember("showInternalNudges") &&
      json["showInternalNudges"].isBool()) {
    config.showInternalNudges = json["showInternalNudges"].asBool();
  }

  if (json.isMember("apiKeys") && json["apiKeys"].isObject()) {
    config.apiKeys.clear();
    for (const auto &name : json["apiKeys"].getMemberNames()) {
      if (json["apiKeys"][name].isString()) {
        config.apiKeys[name] = json["apiKeys"][name].asString();
      }
    }
  }

  if (json.isMember("providerOptions") && json["providerOptions"].isObject()) {
    config.providerOptions.clear();
    for (const auto &name : json["providerOptions"].getMemberNames()) {
      if (json["providerOptions"][name].isString()) {
        config.providerOptions[name] = json["providerOptions"][name].asString();
      }
    }
  }

  if (json.isMember("modelRouterCategories") &&
      json["modelRouterCategories"].isObject()) {
    config.modelRouterCategories.clear();
    for (const auto &name : json["modelRouterCategories"].getMemberNames()) {
      const Json::Value &value = json["modelRouterCategories"][name];
      if (!value.isObject()) {
        continue;
      }
      ModelRouteCategory category;
      category.providerId =
          value.isMember("providerId") && value["providerId"].isString()
              ? value["providerId"].asString()
              : "";
      category.modelId = value.isMember("modelId") && value["modelId"].isString()
                             ? value["modelId"].asString()
                             : "";
      category.variantName =
          value.isMember("variantName") && value["variantName"].isString()
              ? value["variantName"].asString()
              : "";
      if (!category.providerId.empty() && !category.modelId.empty()) {
        config.modelRouterCategories[name] = std::move(category);
      }
    }
  }

  if (json.isMember("purposeRoutes") && json["purposeRoutes"].isObject()) {
    config.purposeRoutes.clear();
    for (const auto &name : json["purposeRoutes"].getMemberNames()) {
      if (json["purposeRoutes"][name].isString()) {
        config.purposeRoutes[name] = json["purposeRoutes"][name].asString();
      }
    }
  }

  if (json.isMember("defaultRouteCategory") &&
      json["defaultRouteCategory"].isString()) {
    config.defaultRouteCategory = json["defaultRouteCategory"].asString();
  }
  if (json.isMember("enableSubagentRouteFallback") &&
      json["enableSubagentRouteFallback"].isBool()) {
    config.enableSubagentRouteFallback =
        json["enableSubagentRouteFallback"].asBool();
  }
  if (json.isMember("subagentRouteFallbackOrder") &&
      json["subagentRouteFallbackOrder"].isArray()) {
    config.subagentRouteFallbackOrder.clear();
    for (const auto &value : json["subagentRouteFallbackOrder"]) {
      if (value.isString()) {
        config.subagentRouteFallbackOrder.push_back(value.asString());
      }
    }
  }

  return config;
}

void registerApiRoutes() {
  auto &harness = firmius::core::Harness::instance();

  drogon::app().registerHandler(
      "/api/state",
      [](const drogon::HttpRequestPtr &, Callback &&callback) {
        replyJson(std::move(callback), successPayload(WebState::instance().buildStateSnapshot()));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/api/threads",
      [](const drogon::HttpRequestPtr &, Callback &&callback) {
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildThreadsSnapshot()));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/api/history/focused",
      [](const drogon::HttpRequestPtr &, Callback &&callback) {
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildFocusedHistorySnapshot()));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/api/themes",
      [](const drogon::HttpRequestPtr &, Callback &&callback) {
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildThemesSnapshot()));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/api/config",
      [](const drogon::HttpRequestPtr &, Callback &&callback) {
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildConfigSnapshot()));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/api/providers",
      [](const drogon::HttpRequestPtr &, Callback &&callback) {
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildProvidersSnapshot()));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/api/workflows",
      [](const drogon::HttpRequestPtr &, Callback &&callback) {
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildWorkflowsSnapshot()));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/api/work",
      [](const drogon::HttpRequestPtr &, Callback &&callback) {
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildWorkSnapshot()));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/api/connect/wizard/start",
      [](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string providerId = jsonString(json, "providerId").value_or("");
        if (providerId.empty()) {
          replyJson(std::move(callback), errorPayload("providerId is required"),
                    drogon::k400BadRequest);
          return;
        }

        auto provider =
            firmius::provider::ProviderRegistry::instance().getProvider(providerId);
        if (!provider) {
          replyJson(std::move(callback), errorPayload("Unknown provider"),
                    drogon::k404NotFound);
          return;
        }

        ConnectionWizardSession session;
        session.providerId = providerId;
        {
          std::lock_guard<std::mutex> lock(wizardMutex);
          session.sessionId =
              "connect-" + std::to_string(nextWizardId++);
        }

        if (auto oauthProvider =
                std::dynamic_pointer_cast<firmius::provider::BaseOAuthProvider>(
                    provider)) {
          session.kind = ConnectionWizardSession::Kind::OAuth;
          session.oauthWizard = oauthProvider->beginConnectionWizard();
          if (!session.oauthWizard) {
            replyJson(std::move(callback),
                      errorPayload("Provider does not expose an OAuth wizard"),
                      drogon::k409Conflict);
            return;
          }
          if (auto prompt = session.oauthWizard->nextPrompt()) {
            session.lastPrompt = prompt->message;
            session.lastPromptSecret = prompt->isSecret;
          }
        } else if (auto apiKeyProvider =
                       std::dynamic_pointer_cast<
                           firmius::provider::BaseAPIKeyProvider>(provider)) {
          session.kind = ConnectionWizardSession::Kind::APIKey;
          session.apiKeyWizard = apiKeyProvider->beginConnectionWizard();
          if (!session.apiKeyWizard) {
            replyJson(std::move(callback),
                      errorPayload("Provider does not expose an API key wizard"),
                      drogon::k409Conflict);
            return;
          }
          if (auto prompt = session.apiKeyWizard->nextPrompt()) {
            session.lastPrompt = *prompt;
            session.lastPromptSecret = true;
          }
        } else {
          replyJson(std::move(callback),
                    errorPayload("Provider does not support interactive connect"),
                    drogon::k409Conflict);
          return;
        }

        const std::string sessionId = session.sessionId;
        Json::Value payload = serializeConnectSession(session);
        {
          std::lock_guard<std::mutex> lock(wizardMutex);
          wizardSessions[sessionId] = std::move(session);
        }
        replyJson(std::move(callback), successPayload(std::move(payload)));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/connect/wizard/status",
      [](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const std::string sessionId =
            req->getOptionalParameter<std::string>("sessionId").value_or("");
        if (sessionId.empty()) {
          replyJson(std::move(callback), errorPayload("sessionId is required"),
                    drogon::k400BadRequest);
          return;
        }

        Json::Value payload;
        bool eraseSession = false;
        {
          std::lock_guard<std::mutex> lock(wizardMutex);
          auto it = wizardSessions.find(sessionId);
          if (it == wizardSessions.end()) {
            replyJson(std::move(callback), errorPayload("Unknown wizard session"),
                      drogon::k404NotFound);
            return;
          }

          auto &session = it->second;
          if (session.kind == ConnectionWizardSession::Kind::OAuth &&
              session.oauthWizard && session.oauthWizard->isComplete()) {
            payload = finalizeOAuthSession(session);
            eraseSession = true;
          } else if (session.kind == ConnectionWizardSession::Kind::APIKey &&
                     session.apiKeyWizard && session.apiKeyWizard->isComplete()) {
            payload = finalizeAPIKeySession(session);
            eraseSession = true;
          } else {
            payload = serializeConnectSession(session);
          }

          if (eraseSession) {
            wizardSessions.erase(it);
          }
        }
        replyJson(std::move(callback), successPayload(std::move(payload)));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/api/connect/wizard/submit",
      [](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string sessionId = jsonString(json, "sessionId").value_or("");
        const std::string answer = jsonString(json, "answer").value_or("");
        if (sessionId.empty()) {
          replyJson(std::move(callback), errorPayload("sessionId is required"),
                    drogon::k400BadRequest);
          return;
        }

        Json::Value payload;
        bool eraseSession = false;
        {
          std::lock_guard<std::mutex> lock(wizardMutex);
          auto it = wizardSessions.find(sessionId);
          if (it == wizardSessions.end()) {
            replyJson(std::move(callback), errorPayload("Unknown wizard session"),
                      drogon::k404NotFound);
            return;
          }

          auto &session = it->second;
          if (session.kind == ConnectionWizardSession::Kind::OAuth) {
            if (!session.oauthWizard) {
              replyJson(std::move(callback),
                        errorPayload("OAuth wizard is unavailable"),
                        drogon::k409Conflict);
              return;
            }
            session.oauthWizard->submitAnswer(answer);
            if (auto prompt = session.oauthWizard->nextPrompt()) {
              session.lastPrompt = prompt->message;
              session.lastPromptSecret = prompt->isSecret;
            }
            if (session.oauthWizard->isComplete()) {
              payload = finalizeOAuthSession(session);
              eraseSession = true;
            } else {
              payload = serializeConnectSession(session);
            }
          } else {
            if (!session.apiKeyWizard) {
              replyJson(std::move(callback),
                        errorPayload("API key wizard is unavailable"),
                        drogon::k409Conflict);
              return;
            }
            session.apiKeyWizard->submitAnswer(answer);
            if (auto prompt = session.apiKeyWizard->nextPrompt()) {
              session.lastPrompt = *prompt;
              session.lastPromptSecret = true;
            }
            if (session.apiKeyWizard->isComplete()) {
              payload = finalizeAPIKeySession(session);
              eraseSession = true;
            } else {
              payload = serializeConnectSession(session);
            }
          }

          if (eraseSession) {
            wizardSessions.erase(it);
          }
        }
        replyJson(std::move(callback), successPayload(std::move(payload)));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/connect/wizard/cancel",
      [](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string sessionId = jsonString(json, "sessionId").value_or("");
        if (sessionId.empty()) {
          replyJson(std::move(callback), errorPayload("sessionId is required"),
                    drogon::k400BadRequest);
          return;
        }
        {
          std::lock_guard<std::mutex> lock(wizardMutex);
          wizardSessions.erase(sessionId);
        }
        replyJson(std::move(callback), successPayload());
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/threads/new",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string cwd =
            jsonString(json, "cwd").value_or(std::filesystem::current_path().string());
        const std::string lead = jsonString(json, "leadPersona").value_or("");
        const std::string threadId = harness.newThread({}, cwd, lead);
        if (threadId.empty()) {
          replyJson(std::move(callback),
                    errorPayload("Failed to create thread"),
                    drogon::k500InternalServerError);
          return;
        }
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildStateSnapshot()));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/threads/switch",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string threadId = jsonString(json, "threadId").value_or("");
        if (threadId.empty()) {
          replyJson(std::move(callback), errorPayload("threadId is required"),
                    drogon::k400BadRequest);
          return;
        }
        if (!harness.switchThread(threadId)) {
          replyJson(std::move(callback), errorPayload("Failed to switch thread"),
                    drogon::k409Conflict);
          return;
        }
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildStateSnapshot()));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/threads/delete",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string threadId = jsonString(json, "threadId").value_or("");
        if (threadId.empty()) {
          replyJson(std::move(callback), errorPayload("threadId is required"),
                    drogon::k400BadRequest);
          return;
        }
        harness.deleteThread(threadId);
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildStateSnapshot()));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/threads/lead",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string leadPersona =
            jsonString(json, "leadPersona").value_or("");
        if (leadPersona.empty()) {
          replyJson(std::move(callback), errorPayload("leadPersona is required"),
                    drogon::k400BadRequest);
          return;
        }
        if (!harness.switchLeadPersona(leadPersona)) {
          replyJson(std::move(callback),
                    errorPayload("Failed to switch lead persona"),
                    drogon::k409Conflict);
          return;
        }
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildStateSnapshot()));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/agents/focus",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string agentId = jsonString(json, "agentId").value_or("");
        if (agentId.empty()) {
          replyJson(std::move(callback), errorPayload("agentId is required"),
                    drogon::k400BadRequest);
          return;
        }
        if (!harness.setFocusedAgent(agentId)) {
          replyJson(std::move(callback), errorPayload("Failed to focus agent"),
                    drogon::k409Conflict);
          return;
        }
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildStateSnapshot()));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/models/switch",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string providerId = jsonString(json, "providerId").value_or("");
        const std::string modelId = jsonString(json, "modelId").value_or("");
        const std::string variant = jsonString(json, "variant").value_or("");
        if (providerId.empty() || modelId.empty()) {
          replyJson(std::move(callback),
                    errorPayload("providerId and modelId are required"),
                    drogon::k400BadRequest);
          return;
        }
        harness.switchModel(providerId, modelId, variant);
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildStateSnapshot()));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/config/update",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const Json::Value &rawConfig =
            json.isMember("config") && json["config"].isObject() ? json["config"]
                                                                  : json;
        auto config = parseUserConfig(rawConfig, harness.getConfig());
        harness.updateConfig(config);
        harness.saveConfig();
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildStateSnapshot()));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/router/update",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        auto config = parseUserConfig(json, harness.getConfig());
        harness.updateConfig(config);
        harness.saveConfig();
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildConfigSnapshot()));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/purposes/update",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        auto config = parseUserConfig(json, harness.getConfig());
        harness.updateConfig(config);
        harness.saveConfig();
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildConfigSnapshot()));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/messages/send",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string text = jsonString(json, "text").value_or("");
        if (text.empty()) {
          replyJson(std::move(callback), errorPayload("text is required"),
                    drogon::k400BadRequest);
          return;
        }
        harness.send(text);
        replyJson(std::move(callback), successPayload());
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/messages/retry",
      [&harness](const drogon::HttpRequestPtr &, Callback &&callback) {
        std::string status;
        if (!harness.retryLastRequest(status)) {
          replyJson(std::move(callback), errorPayload(status), drogon::k409Conflict);
          return;
        }
        Json::Value payload(Json::objectValue);
        payload["status"] = status;
        replyJson(std::move(callback), successPayload(std::move(payload)));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/messages/undo",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        Json::Value payload(Json::objectValue);
        if (json.isMember("timestamp") && json["timestamp"].isUInt64()) {
          const auto result = harness.undoAfterTimestamp(json["timestamp"].asUInt64());
          payload["turnsRemoved"] = result.turnsRemoved;
          payload["compactionReversed"] = result.compactionReversed;
          payload["restoredTurns"] = result.restoredTurns;
          payload["willExceedContext"] = result.willExceedContext;
          replyJson(std::move(callback), successPayload(std::move(payload)));
          return;
        }

        const int count = json.isMember("count") && json["count"].isInt()
                              ? json["count"].asInt()
                              : 1;
        const bool messagesOnly =
            json.isMember("messages") && json["messages"].isBool()
                ? json["messages"].asBool()
                : false;
        const auto result =
            messagesOnly ? harness.undoMessages(count) : harness.undoTurns(count);
        payload["turnsRemoved"] = result.turnsRemoved;
        payload["compactionReversed"] = result.compactionReversed;
        payload["restoredTurns"] = result.restoredTurns;
        payload["willExceedContext"] = result.willExceedContext;
        replyJson(std::move(callback), successPayload(std::move(payload)));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/messages/compact",
      [&harness](const drogon::HttpRequestPtr &, Callback &&callback) {
        harness.compactFocusedAgent();
        replyJson(std::move(callback), successPayload());
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/messages/interrupt",
      [&harness](const drogon::HttpRequestPtr &, Callback &&callback) {
        harness.abort();
        replyJson(std::move(callback), successPayload());
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/permissions/respond",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string requestId = jsonString(json, "requestId").value_or("");
        const std::string responseValue =
            jsonString(json, "response").value_or("Deny");
        if (requestId.empty()) {
          replyJson(std::move(callback), errorPayload("requestId is required"),
                    drogon::k400BadRequest);
          return;
        }
        if (!harness.resolvePermissionEscalation(
                requestId, parsePermissionResponse(responseValue))) {
          replyJson(std::move(callback),
                    errorPayload("Failed to resolve permission request"),
                    drogon::k409Conflict);
          return;
        }
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildStateSnapshot()));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/permissions/mode",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string modeValue = jsonString(json, "mode").value_or("Request");
        if (!harness.setCurrentThreadPermissionMode(
                parsePermissionMode(modeValue))) {
          replyJson(std::move(callback),
                    errorPayload("Failed to set permission mode"),
                    drogon::k409Conflict);
          return;
        }
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildStateSnapshot()));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/accounts",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const std::string providerId =
            req->getOptionalParameter<std::string>("providerId").value_or("");
        if (providerId.empty()) {
          replyJson(std::move(callback),
                    successPayload(WebState::instance().buildProvidersSnapshot()));
          return;
        }
        Json::Value payload(Json::objectValue);
        payload["providerId"] = providerId;
        payload["accounts"] = Json::arrayValue;
        for (const auto &account : harness.getAccounts(providerId)) {
          Json::Value item(Json::objectValue);
          item["identifier"] = account.identifier;
          item["tokenExpiration"] = Json::Int64(account.tokenExpiration);
          item["lastQuotaRefresh"] = Json::Int64(account.lastQuotaRefresh);
          item["rateLimited"] = account.rateLimited;
          item["backoffUntil"] = Json::Int64(account.backoffUntil);
          item["metadata"] = Json::objectValue;
          for (const auto &[key, value] : account.metadata) {
            item["metadata"][key] = value;
          }
          payload["accounts"].append(std::move(item));
        }
        replyJson(std::move(callback), successPayload(std::move(payload)));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/api/accounts/delete",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string providerId = jsonString(json, "providerId").value_or("");
        const std::string identifier = jsonString(json, "identifier").value_or("");
        if (providerId.empty() || identifier.empty()) {
          replyJson(std::move(callback),
                    errorPayload("providerId and identifier are required"),
                    drogon::k400BadRequest);
          return;
        }
        harness.deleteAccount(providerId, identifier);
        replyJson(std::move(callback),
                  successPayload(WebState::instance().buildProvidersSnapshot()));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/quotas",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const std::string providerId =
            req->getOptionalParameter<std::string>("providerId").value_or("");
        if (providerId.empty()) {
          replyJson(std::move(callback),
                    successPayload(WebState::instance().buildProvidersSnapshot()));
          return;
        }
        Json::Value payload(Json::objectValue);
        payload["providerId"] = providerId;
        payload["quotas"] = Json::objectValue;
        for (const auto &[accountId, buckets] : harness.getAllQuotas(providerId)) {
          Json::Value bucketArray(Json::arrayValue);
          for (const auto &bucket : buckets) {
            Json::Value item(Json::objectValue);
            item["name"] = bucket.name;
            item["remainingFraction"] = bucket.remainingFraction;
            item["resetTime"] = bucket.resetTime;
            bucketArray.append(std::move(item));
          }
          payload["quotas"][accountId] = std::move(bucketArray);
        }
        replyJson(std::move(callback), successPayload(std::move(payload)));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/api/workflows/execute",
      [&harness](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const Json::Value json = parseRequestJson(req);
        const std::string workflowId =
            jsonString(json, "workflowId").value_or("");
        if (workflowId.empty()) {
          replyJson(std::move(callback), errorPayload("workflowId is required"),
                    drogon::k400BadRequest);
          return;
        }
        std::vector<std::string> args;
        if (json.isMember("args") && json["args"].isArray()) {
          args.reserve(json["args"].size());
          for (const auto &value : json["args"]) {
            if (value.isString()) {
              args.push_back(value.asString());
            }
          }
        }
        if (!harness.executeWorkflow(workflowId, args)) {
          replyJson(std::move(callback), errorPayload("Workflow not found"),
                    drogon::k404NotFound);
          return;
        }
        replyJson(std::move(callback), successPayload());
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/events/stream",
      [](const drogon::HttpRequestPtr &req, Callback &&callback) {
        const std::string threadId = req->getOptionalParameter<std::string>("threadId").value_or("");
        const std::string sinceParam =
            req->getOptionalParameter<std::string>("since").value_or("");
        const std::string lastEventIdHeader = req->getHeader("Last-Event-ID");
        const std::uint64_t afterId =
            std::max(parseUnsigned(sinceParam), parseUnsigned(lastEventIdHeader));

        std::thread([threadId, afterId, callback = std::move(callback)]() mutable {
          auto events = WebState::instance().waitForEventsAfter(afterId, threadId, 20000);
          std::ostringstream body;
          body << "retry: 1000\n";
          if (events.empty()) {
            body << ": keepalive\n\n";
          } else {
            for (const auto &event : events) {
              body << "id: " << event.id << "\n";
              body << "event: app\n";
              body << "data: " << toJsonString(event.payload) << "\n\n";
            }
          }

          auto response = drogon::HttpResponse::newHttpResponse();
          response->setStatusCode(drogon::k200OK);
          response->setContentTypeCodeAndCustomString(
              drogon::CT_CUSTOM, "text/event-stream; charset=utf-8");
          response->addHeader("Cache-Control", "no-cache");
          response->addHeader("X-Accel-Buffering", "no");
          response->addHeader("Connection", "keep-alive");
          response->setBody(body.str());
          callback(response);
        }).detach();
      },
      {drogon::Get});
}

} // namespace

void runWeb(std::string hostname, int port) {
  shared::Panic::init();

  auto &harness = firmius::core::Harness::instance();
  harness.init();
  firmius::core::WorkflowLoader::instance().init();
  WebState::instance().init();

  registerApiRoutes();
  const std::filesystem::path distPath = frontendDistPath();
  if (distPath.empty()) {
    FIRMIUS_PANIC(
        "Unable to locate web frontend assets. Set FIRMIUS_WEB_DIST_DIR or install "
        "the bundle under share/firmius/web.");
  }
  drogon::app().setDocumentRoot(distPath.string());
  drogon::app().setHomePage("index.html");
  drogon::app().registerHandler(
      "/{1:path}",
      [](const drogon::HttpRequestPtr &req, Callback &&callback,
         const std::string &path) {
        if (path.rfind("api/", 0) == 0) {
          auto response = drogon::HttpResponse::newNotFoundResponse();
          callback(response);
          return;
        }
        const std::filesystem::path distPath = frontendDistPath();
        auto file = distPath / path;
        if (std::filesystem::exists(file) && std::filesystem::is_regular_file(file)) {
          callback(drogon::HttpResponse::newFileResponse(file.string()));
          return;
        }
        callback(drogon::HttpResponse::newFileResponse(
            (distPath / "index.html").string()));
      },
      {drogon::Get});
  drogon::app().setThreadNum(2);
  drogon::app().addListener(std::move(hostname), port).run();
}

} // namespace firmius::web
