#include "ConfigLoader.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/prettywriter.h>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <unistd.h>

namespace firmius::shared {

namespace {

bool ensureWritableDirectory(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec || !std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        return false;
    }

    auto probe = dir / (".write_probe_" + std::to_string(static_cast<long long>(getpid())));
    std::ofstream out(probe);
    if (!out.is_open()) {
        return false;
    }
    out << "ok";
    out.close();
    std::filesystem::remove(probe, ec);
    return true;
}

bool isReadableFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    return file.good();
}

std::filesystem::path resolveFirmiusHomeForConfig() {
    if (const char* home = getenv("HOME")) {
        const std::filesystem::path userHome = std::filesystem::path(home) / ".firmius";
        if (ensureWritableDirectory(userHome) || isReadableFile(userHome / "config.json")) {
            return userHome;
        }
    }

    const std::filesystem::path localHome = std::filesystem::current_path() / ".firmius";
    if (ensureWritableDirectory(localHome) || isReadableFile(localHome / "config.json")) {
        return localHome;
    }

    const std::filesystem::path tempHome =
        std::filesystem::temp_directory_path() /
        ("firmius-" + std::to_string(static_cast<long long>(getuid())));
    if (ensureWritableDirectory(tempHome) || isReadableFile(tempHome / "config.json")) {
        return tempHome;
    }

    return std::filesystem::temp_directory_path() / "firmius";
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
            if (it->value.HasMember("providerId") &&
                it->value["providerId"].IsString()) {
                category.providerId = it->value["providerId"].GetString();
            }
            if (it->value.HasMember("modelId") &&
                it->value["modelId"].IsString()) {
                category.modelId = it->value["modelId"].GetString();
            }
            if (it->value.HasMember("variantName") &&
                it->value["variantName"].IsString()) {
                category.variantName = it->value["variantName"].GetString();
            }
            if (!category.providerId.empty() && !category.modelId.empty()) {
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
        routeObj.AddMember("providerId", rapidjson::Value(route.providerId.c_str(), allocator), allocator);
        routeObj.AddMember("modelId", rapidjson::Value(route.modelId.c_str(), allocator), allocator);
        routeObj.AddMember("variantName", rapidjson::Value(route.variantName.c_str(), allocator), allocator);
        routerCategories.AddMember(rapidjson::Value(name.c_str(), allocator), routeObj, allocator);
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

} // namespace firmius::shared
