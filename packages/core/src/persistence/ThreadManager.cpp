#include "persistence/ThreadManager.hpp"
#include "Serialization.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>

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

std::string todosDirectoryPathFor(const std::string& basePath,
                                  const std::string& threadId) {
    return basePath + "/" + threadId + "/todos";
}

std::string todoPathFor(const std::string& basePath, const std::string& threadId,
                        const std::string& agentId) {
    return todosDirectoryPathFor(basePath, threadId) + "/" + agentId + ".json";
}

std::string artifactsDirectoryPathFor(const std::string& basePath,
                                      const std::string& threadId) {
    return basePath + "/" + threadId + "/artifacts";
}

std::string artifactsManifestPathFor(const std::string& basePath,
                                     const std::string& threadId) {
    return artifactsDirectoryPathFor(basePath, threadId) + "/manifest.json";
}

std::string artifactRelativeStoragePath(const std::string& ownerAgentId,
                                        const std::string& filename) {
    return "artifacts/" + ownerAgentId + "/" + filename;
}

std::string artifactAbsoluteStoragePath(const std::string& basePath,
                                        const std::string& threadId,
                                        const std::string& ownerAgentId,
                                        const std::string& filename) {
    return basePath + "/" + threadId + "/" +
           artifactRelativeStoragePath(ownerAgentId, filename);
}

uint64_t nowEpochMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void fsyncDirectoryBestEffort(const std::filesystem::path& directory) {
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return;
    }
    ::fsync(fd);
    ::close(fd);
}

void writeStringAtomically(const std::string& content,
                           const std::string& path) {
    const std::filesystem::path finalPath(path);
    std::filesystem::create_directories(finalPath.parent_path());

    const std::filesystem::path tempPath =
        finalPath.parent_path() /
        (finalPath.filename().string() + ".tmp." +
         shared::StringUtil::generateUuid());

    const int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error("Cannot write JSON file: " + path);
    }

    const char* data = content.data();
    std::size_t remaining = content.size();
    while (remaining > 0) {
        const ssize_t written = ::write(fd, data, remaining);
        if (written < 0) {
            const int error = errno;
            ::close(fd);
            std::filesystem::remove(tempPath);
            throw std::runtime_error("Cannot write JSON file: " + path +
                                     " (" + std::generic_category().message(error) +
                                     ")");
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }

    if (::fsync(fd) != 0) {
        const int error = errno;
        ::close(fd);
        std::filesystem::remove(tempPath);
        throw std::runtime_error("Cannot flush JSON file: " + path +
                                 " (" + std::generic_category().message(error) +
                                 ")");
    }
    ::close(fd);

    if (::rename(tempPath.c_str(), finalPath.c_str()) != 0) {
        const int error = errno;
        std::filesystem::remove(tempPath);
        throw std::runtime_error("Cannot replace JSON file: " + path +
                                 " (" + std::generic_category().message(error) +
                                 ")");
    }

    fsyncDirectoryBestEffort(finalPath.parent_path());
}

void writeJsonDocument(const rapidjson::Document& document,
                       const std::string& path) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
    writeStringAtomically(buffer.GetString(), path);
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

std::shared_ptr<std::mutex> acquirePlanMutex(const std::string& threadId,
                                             const std::string& planId) {
    static std::mutex registryMutex;
    static std::unordered_map<std::string, std::weak_ptr<std::mutex>> registry;

    const std::string key = threadId + ":" + planId;
    std::lock_guard<std::mutex> guard(registryMutex);
    if (auto existing = registry[key].lock()) {
        return existing;
    }

    auto created = std::make_shared<std::mutex>();
    registry[key] = created;
    return created;
}

std::shared_ptr<std::mutex> acquireTodoMutex(const std::string& threadId,
                                             const std::string& agentId) {
    static std::mutex registryMutex;
    static std::unordered_map<std::string, std::weak_ptr<std::mutex>> registry;

    const std::string key = threadId + ":" + agentId;
    std::lock_guard<std::mutex> guard(registryMutex);
    if (auto existing = registry[key].lock()) {
        return existing;
    }

    auto created = std::make_shared<std::mutex>();
    registry[key] = created;
    return created;
}

std::shared_ptr<std::mutex> acquireArtifactsMutex(const std::string& threadId) {
    static std::mutex registryMutex;
    static std::unordered_map<std::string, std::weak_ptr<std::mutex>> registry;

    std::lock_guard<std::mutex> guard(registryMutex);
    if (auto existing = registry[threadId].lock()) {
        return existing;
    }

    auto created = std::make_shared<std::mutex>();
    registry[threadId] = created;
    return created;
}

void validateArtifactFilename(const std::string& filename) {
    if (filename.empty()) {
        throw std::runtime_error("Artifact filename cannot be empty");
    }
    if (filename == "." || filename == "..") {
        throw std::runtime_error("Artifact filename is invalid: " + filename);
    }
    if (filename.find('\0') != std::string::npos) {
        throw std::runtime_error("Artifact filename contains NUL byte");
    }
    if (filename.find("..") != std::string::npos) {
        throw std::runtime_error("Artifact filename cannot contain '..'");
    }
    if (filename.front() == '/' || filename.front() == '\\') {
        throw std::runtime_error("Artifact filename must be relative");
    }
    if (filename.find('\\') != std::string::npos) {
        throw std::runtime_error("Artifact filename cannot contain '\\\\'");
    }
}

std::vector<shared::ThreadArtifactMetadata>
readArtifactsManifestFile(const std::string& path) {
    std::vector<shared::ThreadArtifactMetadata> artifacts;
    if (!std::filesystem::exists(path)) {
        return artifacts;
    }

    rapidjson::Document doc = readJsonDocument(path, "artifact manifest");
    if (!doc.IsObject()) {
        return artifacts;
    }
    if (!doc.HasMember("artifacts") || !doc["artifacts"].IsArray()) {
        return artifacts;
    }

    for (const auto& entry : doc["artifacts"].GetArray()) {
        if (!entry.IsObject()) {
            continue;
        }
        artifacts.push_back(shared::threadArtifactMetadataFromJson(entry));
    }
    return artifacts;
}

void writeArtifactsManifestFile(
    const std::string& path,
    const std::vector<shared::ThreadArtifactMetadata>& artifacts) {
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    rapidjson::Value items(rapidjson::kArrayType);
    for (const auto& artifact : artifacts) {
        rapidjson::Document artifactDoc = shared::toJson(artifact);
        rapidjson::Value artifactValue;
        artifactValue.CopyFrom(artifactDoc, alloc);
        items.PushBack(artifactValue, alloc);
    }
    doc.AddMember("artifacts", items, alloc);
    writeJsonDocument(doc, path);
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

bool ThreadManager::tryGetMetadata(const std::string& threadId,
                                   ThreadMetadata& metadata,
                                   std::string* error) const {
    try {
        metadata = getMetadata(threadId);
        return true;
    } catch (const std::exception& ex) {
        if (error) {
            *error = ex.what();
        }
    } catch (...) {
        if (error) {
            *error = "Unknown error loading thread metadata";
        }
    }
    return false;
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
            ThreadMetadata meta;
            if (tryGetMetadata(entry.path().filename().string(), meta)) {
                result.push_back(meta);
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

    auto planMutex = acquirePlanMutex(threadId, plan.id);
    std::lock_guard<std::mutex> lock(*planMutex);

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

    auto planMutex = acquirePlanMutex(threadId, plan.id);
    std::lock_guard<std::mutex> lock(*planMutex);

    Plan existing = getPlan(threadId, plan.id);
    if (persistedPlan.createdAt == 0) {
        persistedPlan.createdAt = existing.createdAt;
    }
    persistedPlan.updatedAt = nowEpochMs();
    writeJsonDocument(toJson(persistedPlan),
                      planPathFor(basePath_, threadId, persistedPlan.id));
}

Plan ThreadManager::mutatePlan(const std::string& threadId,
                               const std::string& planId,
                               const std::function<void(Plan&)>& mutator) {
    if (planId.empty()) {
        throw std::runtime_error("Cannot mutate plan with empty id");
    }

    auto planMutex = acquirePlanMutex(threadId, planId);
    std::lock_guard<std::mutex> lock(*planMutex);

    Plan plan = getPlan(threadId, planId);
    mutator(plan);
    if (plan.threadId.empty()) {
        plan.threadId = threadId;
    }
    if (plan.threadId != threadId) {
        throw std::runtime_error("Plan threadId does not match target thread");
    }
    if (plan.createdAt == 0) {
        plan.createdAt = nowEpochMs();
    }
    plan.updatedAt = nowEpochMs();
    writeJsonDocument(toJson(plan), planPathFor(basePath_, threadId, plan.id));
    return plan;
}

AgentTodoList ThreadManager::getAgentTodo(const std::string& threadId,
                                          const std::string& agentId) const {
    if (agentId.empty()) {
        throw std::runtime_error("Cannot load todo list with empty agentId");
    }

    std::string path = todoPathFor(basePath_, threadId, agentId);
    if (!std::filesystem::exists(path)) {
        AgentTodoList empty;
        empty.threadId = threadId;
        empty.agentId = agentId;
        empty.nextId = 1;
        return empty;
    }

    rapidjson::Document d = readJsonDocument(path, "agent todo list");
    AgentTodoList list = agentTodoListFromJson(d);
    if (list.threadId.empty()) {
        list.threadId = threadId;
    }
    if (list.agentId.empty()) {
        list.agentId = agentId;
    }
    if (list.nextId <= 0) {
        list.nextId = 1;
    }
    return list;
}

void ThreadManager::writeAgentTodo(const std::string& threadId,
                                   const std::string& agentId,
                                   const AgentTodoList& todoList) {
    if (agentId.empty()) {
        throw std::runtime_error("Cannot write todo list with empty agentId");
    }

    std::string threadDir = basePath_ + "/" + threadId;
    if (!std::filesystem::exists(threadDir)) {
        throw std::runtime_error("Thread not found: " + threadId);
    }

    auto todoMutex = acquireTodoMutex(threadId, agentId);
    std::lock_guard<std::mutex> lock(*todoMutex);

    AgentTodoList persisted = todoList;
    if (persisted.threadId.empty()) {
        persisted.threadId = threadId;
    }
    if (persisted.agentId.empty()) {
        persisted.agentId = agentId;
    }
    if (persisted.threadId != threadId || persisted.agentId != agentId) {
        throw std::runtime_error("Todo identity does not match target thread/agent");
    }
    if (persisted.nextId <= 0) {
        persisted.nextId = 1;
    }

    std::string todosDir = todosDirectoryPathFor(basePath_, threadId);
    std::filesystem::create_directories(todosDir);
    writeJsonDocument(toJson(persisted), todoPathFor(basePath_, threadId, agentId));
}

AgentTodoList ThreadManager::mutateAgentTodo(
    const std::string& threadId, const std::string& agentId,
    const std::function<void(AgentTodoList&)>& mutator) {
    if (agentId.empty()) {
        throw std::runtime_error("Cannot mutate todo list with empty agentId");
    }

    auto todoMutex = acquireTodoMutex(threadId, agentId);
    std::lock_guard<std::mutex> lock(*todoMutex);

    AgentTodoList todoList = getAgentTodo(threadId, agentId);
    mutator(todoList);
    if (todoList.threadId.empty()) {
        todoList.threadId = threadId;
    }
    if (todoList.agentId.empty()) {
        todoList.agentId = agentId;
    }
    if (todoList.nextId <= 0) {
        todoList.nextId = 1;
    }
    if (todoList.threadId != threadId || todoList.agentId != agentId) {
        throw std::runtime_error("Todo identity does not match target thread/agent");
    }

    writeJsonDocument(toJson(todoList), todoPathFor(basePath_, threadId, agentId));
    return todoList;
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

bool ThreadManager::tryReadAgentManifest(
    const std::string& threadId,
    std::map<std::string, AgentManifestEntry>& manifest,
    std::string* error) const {
    try {
        manifest = readAgentManifest(threadId);
        return true;
    } catch (const std::exception& ex) {
        manifest.clear();
        if (error) {
            *error = ex.what();
        }
    } catch (...) {
        manifest.clear();
        if (error) {
            *error = "Unknown error loading thread manifest";
        }
    }
    return false;
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

shared::ThreadArtifactMetadata
ThreadManager::writeArtifact(const std::string& threadId,
                             const std::string& ownerAgentId,
                             const std::string& ownerFriendlyName,
                             const std::string& filename,
                             const std::string& content, bool* created,
                             const std::optional<std::string>& kind,
                             const std::optional<std::string>& description) {
    if (threadId.empty()) {
        throw std::runtime_error("Cannot write artifact with empty threadId");
    }
    if (ownerAgentId.empty()) {
        throw std::runtime_error("Cannot write artifact with empty ownerAgentId");
    }
    validateArtifactFilename(filename);

    std::string threadDir = basePath_ + "/" + threadId;
    if (!std::filesystem::exists(threadDir)) {
        throw std::runtime_error("Thread not found: " + threadId);
    }

    auto artifactsMutex = acquireArtifactsMutex(threadId);
    std::lock_guard<std::mutex> lock(*artifactsMutex);

    const std::string storagePath =
        artifactRelativeStoragePath(ownerAgentId, filename);
    const std::string absoluteStoragePath =
        artifactAbsoluteStoragePath(basePath_, threadId, ownerAgentId, filename);
    const bool fileExists = std::filesystem::exists(absoluteStoragePath);
    writeStringAtomically(content, absoluteStoragePath);

    const std::string manifestPath = artifactsManifestPathFor(basePath_, threadId);
    auto artifacts = readArtifactsManifestFile(manifestPath);
    const uint64_t timestamp = nowEpochMs();

    auto it = std::find_if(
        artifacts.begin(), artifacts.end(),
        [&](const shared::ThreadArtifactMetadata& item) {
            return item.ownerAgentId == ownerAgentId && item.filename == filename;
        });

    bool wasCreated = false;
    if (it == artifacts.end()) {
        shared::ThreadArtifactMetadata metadata;
        metadata.threadId = threadId;
        metadata.ownerAgentId = ownerAgentId;
        metadata.ownerFriendlyName = ownerFriendlyName;
        metadata.filename = filename;
        metadata.storagePath = storagePath;
        metadata.createdAt = timestamp;
        metadata.updatedAt = timestamp;
        metadata.kind = kind;
        metadata.description = description;
        artifacts.push_back(std::move(metadata));
        it = std::prev(artifacts.end());
        wasCreated = !fileExists;
    } else {
        it->threadId = threadId;
        it->ownerAgentId = ownerAgentId;
        it->ownerFriendlyName = ownerFriendlyName;
        it->storagePath = storagePath;
        if (it->createdAt == 0) {
            it->createdAt = timestamp;
        }
        it->updatedAt = timestamp;
        if (kind.has_value()) {
            it->kind = kind;
        }
        if (description.has_value()) {
            it->description = description;
        }
        wasCreated = false;
    }

    std::sort(artifacts.begin(), artifacts.end(),
              [](const shared::ThreadArtifactMetadata& lhs,
                 const shared::ThreadArtifactMetadata& rhs) {
                  if (lhs.ownerAgentId == rhs.ownerAgentId) {
                      return lhs.filename < rhs.filename;
                  }
                  return lhs.ownerAgentId < rhs.ownerAgentId;
              });
    writeArtifactsManifestFile(manifestPath, artifacts);

    if (created) {
        *created = wasCreated;
    }
    return *std::find_if(
        artifacts.begin(), artifacts.end(),
        [&](const shared::ThreadArtifactMetadata& item) {
            return item.ownerAgentId == ownerAgentId && item.filename == filename;
        });
}

std::string ThreadManager::readArtifact(const std::string& threadId,
                                        const std::string& ownerAgentId,
                                        const std::string& filename) const {
    if (threadId.empty() || ownerAgentId.empty() || filename.empty()) {
        throw std::runtime_error("Artifact selector is incomplete");
    }
    validateArtifactFilename(filename);

    auto artifacts = listArtifacts(threadId);
    auto it = std::find_if(
        artifacts.begin(), artifacts.end(),
        [&](const shared::ThreadArtifactMetadata& item) {
            return item.ownerAgentId == ownerAgentId && item.filename == filename;
        });
    if (it == artifacts.end()) {
        throw std::runtime_error("Artifact not found: " + ownerAgentId + "/" +
                                 filename);
    }

    std::ifstream file(basePath_ + "/" + threadId + "/" + it->storagePath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot read artifact file: " + it->storagePath);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::vector<shared::ThreadArtifactMetadata>
ThreadManager::listArtifacts(const std::string& threadId) const {
    if (threadId.empty()) {
        return {};
    }
    auto artifacts = readArtifactsManifestFile(
        artifactsManifestPathFor(basePath_, threadId));
    std::sort(artifacts.begin(), artifacts.end(),
              [](const shared::ThreadArtifactMetadata& lhs,
                 const shared::ThreadArtifactMetadata& rhs) {
                  if (lhs.ownerFriendlyName == rhs.ownerFriendlyName) {
                      return lhs.filename < rhs.filename;
                  }
                  return lhs.ownerFriendlyName < rhs.ownerFriendlyName;
              });
    return artifacts;
}

std::vector<shared::ThreadArtifactMetadata>
ThreadManager::listArtifactsForAgent(const std::string& threadId,
                                     const std::string& ownerAgentId) const {
    if (ownerAgentId.empty()) {
        return {};
    }
    auto all = listArtifacts(threadId);
    std::vector<shared::ThreadArtifactMetadata> filtered;
    for (const auto& artifact : all) {
        if (artifact.ownerAgentId == ownerAgentId) {
            filtered.push_back(artifact);
        }
    }
    return filtered;
}

std::optional<std::string>
ThreadManager::findAgentIdByFriendlyName(const std::string& threadId,
                                         const std::string& friendlyName) const {
    if (threadId.empty() || friendlyName.empty()) {
        return std::nullopt;
    }
    const auto manifest = readAgentManifest(threadId);
    std::optional<std::string> match;
    for (const auto& [agentId, entry] : manifest) {
        if (entry.friendlyName != friendlyName) {
            continue;
        }
        if (match.has_value() && *match != agentId) {
            return std::nullopt;
        }
        match = agentId;
    }
    return match;
}

std::optional<std::string>
ThreadManager::findFriendlyNameByAgentId(const std::string& threadId,
                                         const std::string& agentId) const {
    if (threadId.empty() || agentId.empty()) {
        return std::nullopt;
    }
    const auto manifest = readAgentManifest(threadId);
    auto it = manifest.find(agentId);
    if (it == manifest.end()) {
        return std::nullopt;
    }
    return it->second.friendlyName;
}

}
