#include "persistence/ThreadManager.hpp"
#include "Serialization.hpp"
#include "utils/StringUtil.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <chrono>

namespace firmius::core {

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
    meta.createdAt = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    meta.lastActiveAt = meta.createdAt;

    rapidjson::Document d = toJson(meta);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);

    std::ofstream file(dir + "/metadata.json");
    file << buffer.GetString();
    return threadId;
}

ThreadMetadata ThreadManager::getMetadata(const std::string& threadId) const {
    std::string path = basePath_ + "/" + threadId + "/metadata.json";
    std::ifstream file(path);
    if (!file.is_open()) throw std::runtime_error("Cannot open thread metadata: " + path);
    std::stringstream buffer;
    buffer << file.rdbuf();

    rapidjson::Document d;
    d.Parse(buffer.str().c_str());
    if (d.HasParseError()) throw std::runtime_error("Invalid JSON in thread metadata: " + path);
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
    rapidjson::Document d = toJson(metadata);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
    
    std::ofstream file(dir + "/metadata.json");
    file << buffer.GetString();
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
    rapidjson::Document d = toJson(metadata);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
    std::ofstream file(dir + "/metadata.json");
    file << buffer.GetString();
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

}
