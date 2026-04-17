#include "ConfigLoader.hpp"

#include "utils/PlatformPaths.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/prettywriter.h>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace firmius::shared {

namespace {

bool ensureWritableDirectory(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec || !std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        return false;
    }

    const auto probe = dir / ".write_probe";
    std::ofstream out(probe, std::ios::app);
    if (!out.is_open()) {
        return false;
    }
    out.close();
    std::filesystem::remove(probe, ec);
    return true;
}

bool isReadableFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    return file.good();
}

std::filesystem::path resolveFirmiusHomeForConfig() {
    const std::filesystem::path homeDir = PlatformPaths::firmiusHomeDir();
    if (ensureWritableDirectory(homeDir) || isReadableFile(homeDir / "config.json")) {
        return homeDir;
    }

    const std::filesystem::path dataDir = PlatformPaths::firmiusDataDir();
    if (ensureWritableDirectory(dataDir) || isReadableFile(dataDir / "config.json")) {
        return dataDir;
    }

    const std::filesystem::path tempDir = PlatformPaths::firmiusTempDir();
    if (ensureWritableDirectory(tempDir) || isReadableFile(tempDir / "config.json")) {
        return tempDir;
    }

    return homeDir;
}

rapidjson::Value toJson(const AgentConfig::RollingModelConfig& model,
                        rapidjson::Document::AllocatorType& allocator) {
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("enabled", model.enabled, allocator);
    v.AddMember("providerId", rapidjson::Value(model.providerId.c_str(), allocator), allocator);
    v.AddMember("modelId", rapidjson::Value(model.modelId.c_str(), allocator), allocator);
    v.AddMember("variantName", rapidjson::Value(model.variantName.c_str(), allocator), allocator);
    return v;
}

void rollingModelConfigFromJson(const rapidjson::Value& v,
                                AgentConfig::RollingModelConfig& model) {
    if (!v.IsObject()) {
        return;
    }
    if (v.HasMember("enabled") && v["enabled"].IsBool()) {
        model.enabled = v["enabled"].GetBool();
    }
    if (v.HasMember("providerId") && v["providerId"].IsString()) {
        model.providerId = v["providerId"].GetString();
    }
    if (v.HasMember("modelId") && v["modelId"].IsString()) {
        model.modelId = v["modelId"].GetString();
    }
    if (v.HasMember("variantName") && v["variantName"].IsString()) {
        model.variantName = v["variantName"].GetString();
    }
}

rapidjson::Value toJson(const UserConfig::RollingMemoryConfig& config,
                        rapidjson::Document::AllocatorType& allocator) {
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("enabled", config.enabled, allocator);
    v.AddMember("mode", rapidjson::Value(config.mode.c_str(), allocator), allocator);
    v.AddMember("preset", rapidjson::Value(config.preset.c_str(), allocator), allocator);
    v.AddMember("targetOccupancyRatio", config.targetOccupancyRatio, allocator);
    v.AddMember("bufferOccupancyRatio", config.bufferOccupancyRatio, allocator);
    v.AddMember("emergencyOccupancyRatio", config.emergencyOccupancyRatio, allocator);
    v.AddMember("reflectionOccupancyRatio", config.reflectionOccupancyRatio, allocator);
    v.AddMember("retainTailRatio", config.retainTailRatio, allocator);
    v.AddMember("minimumRetainedTailTokens", config.minimumRetainedTailTokens, allocator);
    v.AddMember("minimumChunkTokens", config.minimumChunkTokens, allocator);
    v.AddMember("emitEventTurns", config.emitEventTurns, allocator);
    v.AddMember("observer", toJson(config.observer, allocator), allocator);
    v.AddMember("reflector", toJson(config.reflector, allocator), allocator);
    v.AddMember("workingMemoryUpdater", toJson(config.workingMemoryUpdater, allocator), allocator);
    return v;
}

void rollingMemoryConfigFromJson(const rapidjson::Value& v,
                                 UserConfig::RollingMemoryConfig& config) {
    if (!v.IsObject()) {
        return;
    }
    if (v.HasMember("enabled") && v["enabled"].IsBool()) {
        config.enabled = v["enabled"].GetBool();
    }
    if (v.HasMember("mode") && v["mode"].IsString()) {
        config.mode = v["mode"].GetString();
    }
    if (v.HasMember("preset") && v["preset"].IsString()) {
        config.preset = v["preset"].GetString();
    }
    if (v.HasMember("targetOccupancyRatio") && v["targetOccupancyRatio"].IsNumber()) {
        config.targetOccupancyRatio = v["targetOccupancyRatio"].GetFloat();
    }
    if (v.HasMember("bufferOccupancyRatio") && v["bufferOccupancyRatio"].IsNumber()) {
        config.bufferOccupancyRatio = v["bufferOccupancyRatio"].GetFloat();
    }
    if (v.HasMember("emergencyOccupancyRatio") && v["emergencyOccupancyRatio"].IsNumber()) {
        config.emergencyOccupancyRatio = v["emergencyOccupancyRatio"].GetFloat();
    }
    if (v.HasMember("reflectionOccupancyRatio") && v["reflectionOccupancyRatio"].IsNumber()) {
        config.reflectionOccupancyRatio = v["reflectionOccupancyRatio"].GetFloat();
    }
    if (v.HasMember("retainTailRatio") && v["retainTailRatio"].IsNumber()) {
        config.retainTailRatio = v["retainTailRatio"].GetFloat();
    }
    if (v.HasMember("minimumRetainedTailTokens") && v["minimumRetainedTailTokens"].IsUint()) {
        config.minimumRetainedTailTokens = v["minimumRetainedTailTokens"].GetUint();
    }
    if (v.HasMember("minimumChunkTokens") && v["minimumChunkTokens"].IsUint()) {
        config.minimumChunkTokens = v["minimumChunkTokens"].GetUint();
    }
    if (v.HasMember("emitEventTurns") && v["emitEventTurns"].IsBool()) {
        config.emitEventTurns = v["emitEventTurns"].GetBool();
    }
    if (v.HasMember("observer")) {
        rollingModelConfigFromJson(v["observer"], config.observer);
    }
    if (v.HasMember("reflector")) {
        rollingModelConfigFromJson(v["reflector"], config.reflector);
    }
    if (v.HasMember("workingMemoryUpdater")) {
        rollingModelConfigFromJson(v["workingMemoryUpdater"], config.workingMemoryUpdater);
    }
}

rapidjson::Value toJsonMcpStdioPayload(const McpServerConfig& config,
                                   rapidjson::Document::AllocatorType& allocator) {
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("command", rapidjson::Value(config.command.c_str(), allocator), allocator);
    rapidjson::Value args(rapidjson::kArrayType);
    for (const auto& arg : config.args) {
        args.PushBack(rapidjson::Value(arg.c_str(), allocator), allocator);
    }
    v.AddMember("args", args, allocator);
    rapidjson::Value env(rapidjson::kObjectType);
    for (const auto& [key, value] : config.env) {
        env.AddMember(rapidjson::Value(key.c_str(), allocator),
                      rapidjson::Value(value.c_str(), allocator),
                      allocator);
    }
    v.AddMember("env", env, allocator);
    v.AddMember("cwd", rapidjson::Value(config.cwd.c_str(), allocator), allocator);
    v.AddMember("enabled", config.enabled, allocator);
    return v;
}

rapidjson::Value toJsonMcpHttpPayload(const McpServerConfig& config,
                                  rapidjson::Document::AllocatorType& allocator) {
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("url", rapidjson::Value(config.url.c_str(), allocator), allocator);
    v.AddMember("authHeader", rapidjson::Value(config.authHeader.c_str(), allocator), allocator);
    v.AddMember("authBearerToken", rapidjson::Value(config.authBearerToken.c_str(), allocator), allocator);
    v.AddMember("allowInsecureTls", config.allowInsecureTls, allocator);
    v.AddMember("caCertPath", rapidjson::Value(config.caCertPath.c_str(), allocator), allocator);
    v.AddMember("enabled", config.enabled, allocator);
    return v;
}

rapidjson::Value toJsonMcpServerConfig(const McpServerConfig& config,
                                   rapidjson::Document::AllocatorType& allocator) {
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("transport", rapidjson::Value(config.transport.c_str(), allocator), allocator);
    if (config.transport == "http") {
        v.AddMember("http", toJsonMcpHttpPayload(config, allocator), allocator);
    } else {
        v.AddMember("stdio", toJsonMcpStdioPayload(config, allocator), allocator);
    }
    return v;
}

void mcpStdioPayloadFromJson(const rapidjson::Value& v,
                                  McpServerConfig& config) {
    if (!v.IsObject()) {
        return;
    }
    if (v.HasMember("command") && v["command"].IsString()) {
        config.command = v["command"].GetString();
    }
    if (v.HasMember("args") && v["args"].IsArray()) {
        config.args.clear();
        for (const auto& value : v["args"].GetArray()) {
            if (value.IsString()) {
                config.args.push_back(value.GetString());
            }
        }
    }
    if (v.HasMember("env") && v["env"].IsObject()) {
        config.env.clear();
        for (auto it = v["env"].MemberBegin(); it != v["env"].MemberEnd(); ++it) {
            if (it->value.IsString()) {
                config.env[it->name.GetString()] = it->value.GetString();
            }
        }
    }
    if (v.HasMember("cwd") && v["cwd"].IsString()) {
        config.cwd = v["cwd"].GetString();
    }
    if (v.HasMember("enabled") && v["enabled"].IsBool()) {
        config.enabled = v["enabled"].GetBool();
    }
}

void mcpHttpPayloadFromJson(const rapidjson::Value& v,
                                 McpServerConfig& config) {
    if (!v.IsObject()) {
        return;
    }
    if (v.HasMember("url") && v["url"].IsString()) {
        config.url = v["url"].GetString();
    }
    if (v.HasMember("authHeader") && v["authHeader"].IsString()) {
        config.authHeader = v["authHeader"].GetString();
    }
    if (v.HasMember("authBearerToken") && v["authBearerToken"].IsString()) {
        config.authBearerToken = v["authBearerToken"].GetString();
    }
    if (v.HasMember("allowInsecureTls") && v["allowInsecureTls"].IsBool()) {
        config.allowInsecureTls = v["allowInsecureTls"].GetBool();
    }
    if (v.HasMember("caCertPath") && v["caCertPath"].IsString()) {
        config.caCertPath = v["caCertPath"].GetString();
    }
    if (v.HasMember("enabled") && v["enabled"].IsBool()) {
        config.enabled = v["enabled"].GetBool();
    }
}

void mcpServerConfigFromJson(const rapidjson::Value& v,
                             McpServerConfig& config) {
    if (!v.IsObject()) {
        return;
    }

    if (v.HasMember("transport") && v["transport"].IsString()) {
        config.transport = v["transport"].GetString();
    }

    if (v.HasMember("http")) {
        config.transport = "http";
        mcpHttpPayloadFromJson(v["http"], config);
        return;
    }

    if (v.HasMember("stdio")) {
        config.transport = "stdio";
        mcpStdioPayloadFromJson(v["stdio"], config);
        return;
    }

    const bool hasLegacyHttpFields =
        v.HasMember("url") || v.HasMember("authHeader") ||
        v.HasMember("authBearerToken") || v.HasMember("allowInsecureTls") ||
        v.HasMember("caCertPath");
    if (hasLegacyHttpFields || config.transport == "http") {
        config.transport = "http";
        mcpHttpPayloadFromJson(v, config);
        return;
    }

    const bool hasLegacyStdioFields =
        v.HasMember("command") || v.HasMember("args") || v.HasMember("env") ||
        v.HasMember("cwd") || v.HasMember("enabled");
    if (hasLegacyStdioFields || config.transport == "stdio") {
        config.transport = "stdio";
        mcpStdioPayloadFromJson(v, config);
    }
}

} // namespace

ConfigLoader& ConfigLoader::instance() {
    static ConfigLoader instance;
    return instance;
}

std::string ConfigLoader::getConfigPath() const {
    return (resolveFirmiusHomeForConfig() / "config.json").string();
}

void ConfigLoader::loadImpl() {
    std::string configPath = getConfigPath();
    if (!std::filesystem::exists(configPath)) {
        loaded_ = true;
        return;
    }

    std::ifstream file(configPath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open config file: " + configPath);
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    rapidjson::Document doc;
    doc.Parse(content.c_str());
    if (doc.HasParseError()) {
        throw std::runtime_error("Failed to parse config JSON");
    }

    if (doc.HasMember("defaultProviderId") && doc["defaultProviderId"].IsString()) {
        config_.defaultProviderId = doc["defaultProviderId"].GetString();
    }
    if (doc.HasMember("defaultModelId") && doc["defaultModelId"].IsString()) {
        config_.defaultModelId = doc["defaultModelId"].GetString();
    }
    if (doc.HasMember("defaultModelVariant") && doc["defaultModelVariant"].IsString()) {
        config_.defaultModelVariant = doc["defaultModelVariant"].GetString();
    }
    if (doc.HasMember("defaultLeadPersona") && doc["defaultLeadPersona"].IsString()) {
        config_.defaultLeadPersona = doc["defaultLeadPersona"].GetString();
    }
    if (doc.HasMember("defaultTemperature") && doc["defaultTemperature"].IsFloat()) {
        config_.defaultTemperature = doc["defaultTemperature"].GetFloat();
    }
    if (doc.HasMember("dangerouslySkipPermissions") && doc["dangerouslySkipPermissions"].IsBool()) {
        config_.dangerouslySkipPermissions = doc["dangerouslySkipPermissions"].GetBool();
    }
    if (doc.HasMember("defaultMaxTokens")) {
        if (doc["defaultMaxTokens"].IsUint()) {
            config_.defaultMaxTokens = doc["defaultMaxTokens"].GetUint();
        } else if (doc["defaultMaxTokens"].IsNull()) {
            config_.defaultMaxTokens = std::nullopt;
        }
    }
    if (doc.HasMember("apiKeys") && doc["apiKeys"].IsObject()) {
        config_.apiKeys.clear();
        for (auto it = doc["apiKeys"].MemberBegin(); it != doc["apiKeys"].MemberEnd(); ++it) {
            if (it->value.IsString()) {
                config_.apiKeys[it->name.GetString()] = it->value.GetString();
            }
        }
    }
    if (doc.HasMember("providerOptions") && doc["providerOptions"].IsObject()) {
        config_.providerOptions.clear();
        for (auto it = doc["providerOptions"].MemberBegin(); it != doc["providerOptions"].MemberEnd(); ++it) {
            if (it->value.IsString()) {
                config_.providerOptions[it->name.GetString()] = it->value.GetString();
            }
        }
    }
    if (doc.HasMember("modelRouterCategories") &&
        doc["modelRouterCategories"].IsObject()) {
        config_.modelRouterCategories.clear();
        for (auto it = doc["modelRouterCategories"].MemberBegin();
             it != doc["modelRouterCategories"].MemberEnd(); ++it) {
            if (!it->value.IsObject()) {
                continue;
            }
            ModelRouteCategory category;
            if (it->value.HasMember("models") && it->value["models"].IsArray()) {
                const auto &modelsArr = it->value["models"];
                for (rapidjson::SizeType i = 0; i < modelsArr.Size(); ++i) {
                    const auto &m = modelsArr[i];
                    if (m.IsObject() && m.HasMember("providerId") &&
                        m["providerId"].IsString() && m.HasMember("modelId") &&
                        m["modelId"].IsString()) {
                        ModelOption opt;
                        opt.providerId = m["providerId"].GetString();
                        opt.modelId = m["modelId"].GetString();
                        if (m.HasMember("variantName") && m["variantName"].IsString()) {
                            opt.variantName = m["variantName"].GetString();
                        }
                        category.models.push_back(opt);
                    }
                }
            } else if (it->value.IsObject()) {
                // Legacy single-model format
                ModelOption opt;
                if (it->value.HasMember("providerId") &&
                    it->value["providerId"].IsString()) {
                    opt.providerId = it->value["providerId"].GetString();
                }
                if (it->value.HasMember("modelId") &&
                    it->value["modelId"].IsString()) {
                    opt.modelId = it->value["modelId"].GetString();
                }
                if (it->value.HasMember("variantName") &&
                    it->value["variantName"].IsString()) {
                    opt.variantName = it->value["variantName"].GetString();
                }
                if (!opt.providerId.empty() && !opt.modelId.empty()) {
                    category.models.push_back(opt);
                }
            }

            if (!category.models.empty()) {
                config_.modelRouterCategories[it->name.GetString()] = category;
            }
        }
    }
    if (doc.HasMember("purposeRoutes") && doc["purposeRoutes"].IsObject()) {
        config_.purposeRoutes.clear();
        for (auto it = doc["purposeRoutes"].MemberBegin();
             it != doc["purposeRoutes"].MemberEnd(); ++it) {
            if (it->value.IsString()) {
                config_.purposeRoutes[it->name.GetString()] = it->value.GetString();
            }
        }
    }
    if (doc.HasMember("defaultRouteCategory") &&
        doc["defaultRouteCategory"].IsString()) {
        config_.defaultRouteCategory = doc["defaultRouteCategory"].GetString();
    }
    if (doc.HasMember("enableSubagentRouteFallback") &&
        doc["enableSubagentRouteFallback"].IsBool()) {
        config_.enableSubagentRouteFallback =
            doc["enableSubagentRouteFallback"].GetBool();
    }
    if (doc.HasMember("subagentRouteFallbackOrder") &&
        doc["subagentRouteFallbackOrder"].IsArray()) {
        config_.subagentRouteFallbackOrder.clear();
        for (const auto &value : doc["subagentRouteFallbackOrder"].GetArray()) {
            if (value.IsString()) {
                config_.subagentRouteFallbackOrder.push_back(value.GetString());
            }
        }
    }
    if (doc.HasMember("showInternalNudges") &&
        doc["showInternalNudges"].IsBool()) {
        config_.showInternalNudges = doc["showInternalNudges"].GetBool();
    }
    if (doc.HasMember("hideErrors") &&
        doc["hideErrors"].IsBool()) {
        config_.hideErrors = doc["hideErrors"].GetBool();
    }
    if (doc.HasMember("rollingMemory")) {
        rollingMemoryConfigFromJson(doc["rollingMemory"], config_.rollingMemory);
    }
    if (doc.HasMember("mcpServers") && doc["mcpServers"].IsObject()) {
        config_.mcpServers.clear();
        for (auto it = doc["mcpServers"].MemberBegin();
             it != doc["mcpServers"].MemberEnd(); ++it) {
            if (!it->value.IsObject()) {
                continue;
            }
            McpServerConfig server;
            mcpServerConfigFromJson(it->value, server);
            config_.mcpServers[it->name.GetString()] = server;
        }
    }

    loaded_ = true;
}

void ConfigLoader::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    loadImpl();
}

void ConfigLoader::save() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string configPath = getConfigPath();
    std::filesystem::path dir = std::filesystem::path(configPath).parent_path();
    if (!std::filesystem::exists(dir)) {
        std::filesystem::create_directories(dir);
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();

    doc.AddMember("defaultProviderId", rapidjson::Value(config_.defaultProviderId.c_str(), allocator), allocator);
    doc.AddMember("defaultModelId", rapidjson::Value(config_.defaultModelId.c_str(), allocator), allocator);
    doc.AddMember("defaultModelVariant", rapidjson::Value(config_.defaultModelVariant.c_str(), allocator), allocator);
    doc.AddMember("defaultLeadPersona", rapidjson::Value(config_.defaultLeadPersona.c_str(), allocator), allocator);
    doc.AddMember("defaultTemperature", config_.defaultTemperature, allocator);
    doc.AddMember("dangerouslySkipPermissions", config_.dangerouslySkipPermissions, allocator);

    if (config_.defaultMaxTokens.has_value()) {
        doc.AddMember("defaultMaxTokens", config_.defaultMaxTokens.value(), allocator);
    } else {
        doc.AddMember("defaultMaxTokens", rapidjson::Value(rapidjson::kNullType), allocator);
    }

    rapidjson::Value apiKeys(rapidjson::kObjectType);
    for (const auto& [key, value] : config_.apiKeys) {
        apiKeys.AddMember(rapidjson::Value(key.c_str(), allocator), rapidjson::Value(value.c_str(), allocator), allocator);
    }
    doc.AddMember("apiKeys", apiKeys, allocator);

    rapidjson::Value providerOptions(rapidjson::kObjectType);
    for (const auto& [key, value] : config_.providerOptions) {
        providerOptions.AddMember(rapidjson::Value(key.c_str(), allocator), rapidjson::Value(value.c_str(), allocator), allocator);
    }
    doc.AddMember("providerOptions", providerOptions, allocator);

    rapidjson::Value routerCategories(rapidjson::kObjectType);
    for (const auto &[name, route] : config_.modelRouterCategories) {
        rapidjson::Value routeObj(rapidjson::kObjectType);
        rapidjson::Value modelsArr(rapidjson::kArrayType);
        for (const auto &opt : route.models) {
            rapidjson::Value optObj(rapidjson::kObjectType);
            optObj.AddMember("providerId",
                             rapidjson::Value(opt.providerId.c_str(), allocator),
                             allocator);
            optObj.AddMember("modelId",
                             rapidjson::Value(opt.modelId.c_str(), allocator),
                             allocator);
            optObj.AddMember("variantName",
                             rapidjson::Value(opt.variantName.c_str(), allocator),
                             allocator);
            modelsArr.PushBack(optObj, allocator);
        }
        routeObj.AddMember("models", modelsArr, allocator);
        routerCategories.AddMember(rapidjson::Value(name.c_str(), allocator),
                                   routeObj, allocator);
    }
    doc.AddMember("modelRouterCategories", routerCategories, allocator);

    rapidjson::Value purposeRoutes(rapidjson::kObjectType);
    for (const auto &[purpose, category] : config_.purposeRoutes) {
        purposeRoutes.AddMember(rapidjson::Value(purpose.c_str(), allocator),
                                rapidjson::Value(category.c_str(), allocator),
                                allocator);
    }
    doc.AddMember("purposeRoutes", purposeRoutes, allocator);
    doc.AddMember("defaultRouteCategory",
                  rapidjson::Value(config_.defaultRouteCategory.c_str(), allocator),
                  allocator);
    doc.AddMember("enableSubagentRouteFallback",
                  config_.enableSubagentRouteFallback, allocator);
    rapidjson::Value fallbackOrder(rapidjson::kArrayType);
    for (const auto &category : config_.subagentRouteFallbackOrder) {
        fallbackOrder.PushBack(rapidjson::Value(category.c_str(), allocator),
                               allocator);
    }
    doc.AddMember("subagentRouteFallbackOrder", fallbackOrder, allocator);
    doc.AddMember("showInternalNudges", config_.showInternalNudges, allocator);
    doc.AddMember("hideErrors", config_.hideErrors, allocator);
    doc.AddMember("rollingMemory", toJson(config_.rollingMemory, allocator),
                  allocator);
    rapidjson::Value mcpServers(rapidjson::kObjectType);
    for (const auto& [name, server] : config_.mcpServers) {
        mcpServers.AddMember(rapidjson::Value(name.c_str(), allocator),
                             toJsonMcpServerConfig(server, allocator),
                             allocator);
    }
    doc.AddMember("mcpServers", mcpServers, allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    std::ofstream file(configPath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open config file for writing: " + configPath);
    }
    file << buffer.GetString();
    file.close();
}

const UserConfig& ConfigLoader::getConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) {
        const_cast<ConfigLoader*>(this)->loadImpl();
    }
    return config_;
}

void ConfigLoader::updateConfig(const UserConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    loaded_ = true;
}

void ConfigLoader::setPreferredModelKey(const std::string& category, const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    preferredModelKey_[category] = key;
}

std::string ConfigLoader::getPreferredModelKey(const std::string& category) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = preferredModelKey_.find(category);
    if (it != preferredModelKey_.end()) {
        return it->second;
    }
    return "";
}

void ConfigLoader::clearPreferredModelKey(const std::string& category) const {
    std::lock_guard<std::mutex> lock(mutex_);
    preferredModelKey_.erase(category);
}
// test insertion
} // namespace firmius::shared
