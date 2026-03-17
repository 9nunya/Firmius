#include "persistence/ThreadManager.hpp"
#include "Serialization.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <chrono>

#include <iostream>

namespace firmius::core {

namespace {

std::string permissionRulesPathFor(const std::string& basePath,
                                   const std::string& threadId) {
    return basePath + "/" + threadId + "/permissions.json";
}

std::string plansDirectoryPathFor(const std::string& basePath,
                                  const std::string& threadId) {
    return basePath + "/" + threadId + "/plans";
}

std::string planPathFor(const std::string& basePath, const std::string& threadId,
                        const std::string& planId) {
    return plansDirectoryPathFor(basePath, threadId) + "/" + planId + ".json";
}

uint64_t nowEpochMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void writeJsonDocument(const rapidjson::Document& document,
                       const std::string& path) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);

    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot write JSON file: " + path);
    }
    file << buffer.GetString();
}

rapidjson::Document readJsonDocument(const std::string& path,
                                     const std::string& errorContext) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open " + errorContext + ": " + path);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    rapidjson::Document document;
    document.Parse(buffer.str().c_str());
    if (document.HasParseError()) {
        throw std::runtime_error("Invalid JSON in " + errorContext + ": " + path);
    }
    return document;
}

const char* severityToString(CommandSeverity severity) {
    switch (severity) {
    case CommandSeverity::LOW:
        return "LOW";
    case CommandSeverity::MEDIUM:
        return "MEDIUM";
    case CommandSeverity::HIGH:
        return "HIGH";
    case CommandSeverity::VULNERABLE:
        return "VULNERABLE";
    }
    return "LOW";
}

CommandSeverity severityFromString(const std::string& value) {
    if (value == "VULNERABLE") {
        return CommandSeverity::VULNERABLE;
    }
    if (value == "HIGH") {
        return CommandSeverity::HIGH;
    }
    if (value == "MEDIUM") {
        return CommandSeverity::MEDIUM;
    }
    return CommandSeverity::LOW;
}

} // namespace

ThreadManager::ThreadManager(std::string basePath)
    : basePath_(std::move(basePath)) {}

std::vector<std::string> ThreadManager::listThreads() const {
    std::vector<std::string> threads;
    std::string dir = basePath_;
    if (!std::filesystem::exists(dir)) return {};

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_directory()) {
            threads.push_back(entry.path().filename().string());
        }
    }
    return threads;
}

std::string ThreadManager::createThread(const ThreadMetadata& metadata) {
    std::string threadId = shared::StringUtil::generateUuid();
    std::string dir = basePath_ + "/" + threadId;
    std::filesystem::create_directories(dir);

    ThreadMetadata meta = metadata;
    meta.threadId = threadId;
    meta.createdAt = nowEpochMs();
    meta.lastActiveAt = meta.createdAt;

    writeJsonDocument(toJson(meta), dir + "/metadata.json");
    return threadId;
}

ThreadMetadata ThreadManager::getMetadata(const std::string& threadId) const {
    std::string path = basePath_ + "/" + threadId + "/metadata.json";
    rapidjson::Document d = readJsonDocument(path, "thread metadata");
    auto meta = threadMetadataFromJson(d);
    if (meta.threadId.empty()) {
        meta.threadId = threadId;
    }
    return meta;
}

AgentHistory ThreadManager::loadAgentHistory(const std::string& threadId, const std::string& agentId) const {
    std::string path = basePath_ + "/" + threadId + "/" + agentId + ".jsonl";
    std::ifstream file(path);
    std::string line;
    AgentHistory history;
    history.threadId = threadId;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        rapidjson::Document d;
        d.Parse(line.c_str());
        history.turns.push_back(agentTurnFromJsonValue(d));
    }
    return history;
}

std::vector<std::string> ThreadManager::listAgents(const std::string& threadId) const {
    std::vector<std::string> agents;
    std::string dir = basePath_ + "/" + threadId;
    if (!std::filesystem::exists(dir)) return {};

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
            agents.push_back(entry.path().stem().string());
        }
    }
    return agents;
}

void ThreadManager::updateHostIdentifier(const std::string& threadId, const std::string& hostIdentifier) {
    auto metadata = getMetadata(threadId);
    metadata.hostIdentifier = hostIdentifier;
    
    std::string dir = basePath_ + "/" + threadId;
    writeJsonDocument(toJson(metadata), dir + "/metadata.json");
}

void ThreadManager::deleteThread(const std::string& threadId) {
    std::string dir = basePath_ + "/" + threadId;
    if (!std::filesystem::exists(dir)) {
        throw std::runtime_error("Thread not found: " + threadId);
    }
    std::filesystem::remove_all(dir);
}

void ThreadManager::updateMetadata(const std::string& threadId, const ThreadMetadata& metadata) {
    std::string dir = basePath_ + "/" + threadId;
    if (!std::filesystem::exists(dir)) {
        throw std::runtime_error("Thread not found: " + threadId);
    }
    writeJsonDocument(toJson(metadata), dir + "/metadata.json");
}

std::vector<ThreadMetadata> ThreadManager::listThreadsWithMetadata() const {
    std::vector<ThreadMetadata> result;
    std::string dir = basePath_;
    if (!std::filesystem::exists(dir)) return {};
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_directory()) {
            try {
                auto meta = getMetadata(entry.path().filename().string());
                if (meta.threadId.empty()) {
                    meta.threadId = entry.path().filename().string();
                }
                result.push_back(meta);
            } catch (...) {
            }
        }
    }
    return result;
}

std::string ThreadManager::createPlan(const Plan& plan) {
    Plan persistedPlan = plan;
    if (persistedPlan.threadId.empty()) {
        throw std::runtime_error("Cannot create plan with empty threadId");
    }
    if (!std::filesystem::exists(basePath_ + "/" + persistedPlan.threadId)) {
        throw std::runtime_error("Thread not found: " + persistedPlan.threadId);
    }

    if (persistedPlan.id.empty()) {
        persistedPlan.id = shared::StringUtil::generateUuid();
    }
    const uint64_t timestamp = nowEpochMs();
    if (persistedPlan.createdAt == 0) {
        persistedPlan.createdAt = timestamp;
    }
    persistedPlan.updatedAt = timestamp;

    writePlan(persistedPlan.threadId, persistedPlan);
    return persistedPlan.id;
}

void ThreadManager::writePlan(const std::string& threadId, const Plan& plan) {
    if (plan.id.empty()) {
        throw std::runtime_error("Cannot write plan with empty id");
    }

    std::string threadDir = basePath_ + "/" + threadId;
    if (!std::filesystem::exists(threadDir)) {
        throw std::runtime_error("Thread not found: " + threadId);
    }

    std::string plansDir = plansDirectoryPathFor(basePath_, threadId);
    std::filesystem::create_directories(plansDir);

    Plan persistedPlan = plan;
    if (persistedPlan.threadId.empty()) {
        persistedPlan.threadId = threadId;
    }

    writeJsonDocument(toJson(persistedPlan),
                      planPathFor(basePath_, threadId, persistedPlan.id));
}

Plan ThreadManager::getPlan(const std::string& threadId,
                            const std::string& planId) const {
    std::string path = planPathFor(basePath_, threadId, planId);
    rapidjson::Document d = readJsonDocument(path, "plan");
    Plan plan = planFromJson(d);
    if (plan.id.empty()) {
        plan.id = planId;
    }
    if (plan.threadId.empty()) {
        plan.threadId = threadId;
    }
    return plan;
}

std::vector<Plan> ThreadManager::listPlans(const std::string& threadId) const {
    std::vector<Plan> plans;
    std::string plansDir = plansDirectoryPathFor(basePath_, threadId);
    if (!std::filesystem::exists(plansDir)) {
        return plans;
    }

    for (const auto& entry : std::filesystem::directory_iterator(plansDir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }

        Plan plan = getPlan(threadId, entry.path().stem().string());
        plans.push_back(std::move(plan));
    }

    std::sort(plans.begin(), plans.end(),
              [](const Plan& lhs, const Plan& rhs) { return lhs.id < rhs.id; });
    return plans;
}

void ThreadManager::updatePlan(const std::string& threadId, const Plan& plan) {
    if (plan.id.empty()) {
        throw std::runtime_error("Cannot update plan with empty id");
    }

    Plan persistedPlan = plan;
    if (persistedPlan.threadId.empty()) {
        persistedPlan.threadId = threadId;
    }
    if (persistedPlan.threadId != threadId) {
        throw std::runtime_error("Plan threadId does not match target thread");
    }

    Plan existing = getPlan(threadId, plan.id);
    if (persistedPlan.createdAt == 0) {
        persistedPlan.createdAt = existing.createdAt;
    }
    persistedPlan.updatedAt = nowEpochMs();
    writePlan(threadId, persistedPlan);
}

std::map<std::string, AgentManifestEntry> ThreadManager::readAgentManifest(const std::string& threadId) const {
    std::string path = basePath_ + "/" + threadId + "/agents.json";
    std::map<std::string, AgentManifestEntry> manifest;

    if (!std::filesystem::exists(path)) {
        return manifest; // empty
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open agent manifest: " + path);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();

    rapidjson::Document d;
    d.Parse(buffer.str().c_str());
    if (d.HasParseError()) {
        throw std::runtime_error("Invalid JSON in agent manifest: " + path);
    }

    if (!d.IsObject()) {
        return manifest;
    }

    for (auto& m : d.GetObject()) {
        AgentManifestEntry entry;
        auto& val = m.value;

        if (val.HasMember("persona") && val["persona"].IsString()) {
            entry.persona = val["persona"].GetString();
        }
        if (val.HasMember("parentId") && val["parentId"].IsString()) {
            entry.parentId = val["parentId"].GetString();
        }
        if (val.HasMember("friendlyName") && val["friendlyName"].IsString()) {
            entry.friendlyName = val["friendlyName"].GetString();
        }
        if (val.HasMember("title") && val["title"].IsString()) {
            entry.title = val["title"].GetString();
        }
        if (val.HasMember("persistHistory") && val["persistHistory"].IsBool()) {
            entry.persistHistory = val["persistHistory"].GetBool();
        } else {
            entry.persistHistory = true; // default
        }

        manifest[m.name.GetString()] = entry;
    }

    return manifest;
}

void ThreadManager::writeAgentManifest(const std::string& threadId, const std::map<std::string, AgentManifestEntry>& manifest) {
    std::string dir = basePath_ + "/" + threadId;
    std::filesystem::create_directories(dir);
    std::string path = dir + "/agents.json";

    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();

    for (const auto& [agentId, entry] : manifest) {
        rapidjson::Value obj(rapidjson::kObjectType);
        obj.AddMember("persona", rapidjson::Value(entry.persona.c_str(), a).Move(), a);
        obj.AddMember("parentId", rapidjson::Value(entry.parentId.c_str(), a).Move(), a);
        obj.AddMember("friendlyName", rapidjson::Value(entry.friendlyName.c_str(), a).Move(), a);
        obj.AddMember("title", rapidjson::Value(entry.title.c_str(), a).Move(), a);
        obj.AddMember("persistHistory", entry.persistHistory, a);
        d.AddMember(rapidjson::Value(agentId.c_str(), a).Move(), obj, a);
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);

    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot write agent manifest: " + path);
    }
    file << buffer.GetString();
}

ThreadPermissionRules ThreadManager::readPermissionRules(const std::string& threadId) const {
    ThreadPermissionRules rules;
    std::string path = permissionRulesPathFor(basePath_, threadId);

    if (!std::filesystem::exists(path)) {
        return rules;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open permission rules: " + path);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    rapidjson::Document d;
    d.Parse(buffer.str().c_str());
    if (d.HasParseError()) {
        throw std::runtime_error("Invalid JSON in permission rules: " + path);
    }

    if (!d.IsObject()) {
        return rules;
    }

    if (d.HasMember("command_allow_rules") &&
        d["command_allow_rules"].IsArray()) {
        for (const auto& ruleValue : d["command_allow_rules"].GetArray()) {
            if (!ruleValue.IsObject()) {
                continue;
            }

            CommandAllowRule rule;
            if (ruleValue.HasMember("exact_command") &&
                ruleValue["exact_command"].IsString()) {
                rule.exactCommand = ruleValue["exact_command"].GetString();
            }
            if (ruleValue.HasMember("normalized_command") &&
                ruleValue["normalized_command"].IsString()) {
                rule.normalizedCommand =
                    ruleValue["normalized_command"].GetString();
            }
            if (ruleValue.HasMember("primary_command") &&
                ruleValue["primary_command"].IsString()) {
                rule.primaryCommand = ruleValue["primary_command"].GetString();
            }
            if (ruleValue.HasMember("severity") &&
                ruleValue["severity"].IsString()) {
                rule.severity =
                    severityFromString(ruleValue["severity"].GetString());
            }

            if (!rule.exactCommand.empty()) {
                rules.commandAllowRules.push_back(std::move(rule));
            }
        }
    }

    if (d.HasMember("write_allow_paths") &&
        d["write_allow_paths"].IsArray()) {
        for (const auto& pathValue : d["write_allow_paths"].GetArray()) {
            if (pathValue.IsString()) {
                rules.writeAllowPaths.push_back(pathValue.GetString());
            }
        }
    }

    return rules;
}

void ThreadManager::writePermissionRules(const std::string& threadId,
                                         const ThreadPermissionRules& rules) {
    std::string dir = basePath_ + "/" + threadId;
    std::filesystem::create_directories(dir);
    std::string path = permissionRulesPathFor(basePath_, threadId);

    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();

    rapidjson::Value commandRules(rapidjson::kArrayType);
    for (const auto& rule : rules.commandAllowRules) {
        rapidjson::Value entry(rapidjson::kObjectType);
        entry.AddMember("exact_command",
                        rapidjson::Value(rule.exactCommand.c_str(), a).Move(), a);
        entry.AddMember(
            "normalized_command",
            rapidjson::Value(rule.normalizedCommand.c_str(), a).Move(), a);
        entry.AddMember(
            "primary_command",
            rapidjson::Value(rule.primaryCommand.c_str(), a).Move(), a);
        entry.AddMember("severity",
                        rapidjson::Value(severityToString(rule.severity), a).Move(),
                        a);
        commandRules.PushBack(entry, a);
    }
    d.AddMember("command_allow_rules", commandRules, a);

    rapidjson::Value writePaths(rapidjson::kArrayType);
    for (const auto& pathPrefix : rules.writeAllowPaths) {
        writePaths.PushBack(rapidjson::Value(pathPrefix.c_str(), a).Move(), a);
    }
    d.AddMember("write_allow_paths", writePaths, a);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);

    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot write permission rules: " + path);
    }
    file << buffer.GetString();
}

void ThreadManager::addCommandAllowRule(const std::string& threadId,
                                        const CommandAllowRule& rule) {
    auto rules = readPermissionRules(threadId);
    auto exists = std::any_of(
        rules.commandAllowRules.begin(), rules.commandAllowRules.end(),
        [&rule](const CommandAllowRule& existing) {
            return existing.exactCommand == rule.exactCommand &&
                   existing.normalizedCommand == rule.normalizedCommand;
        });
    if (!exists) {
        rules.commandAllowRules.push_back(rule);
        writePermissionRules(threadId, rules);
    }
}

void ThreadManager::addWriteAllowPath(const std::string& threadId,
                                      const std::string& pathPrefix) {
    auto rules = readPermissionRules(threadId);
    auto exists = std::any_of(
        rules.writeAllowPaths.begin(), rules.writeAllowPaths.end(),
        [&pathPrefix](const std::string& existing) {
            return existing == pathPrefix;
        });
    if (!exists) {
        rules.writeAllowPaths.push_back(pathPrefix);
        writePermissionRules(threadId, rules);
    }
}

}
