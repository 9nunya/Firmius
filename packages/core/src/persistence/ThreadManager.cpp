#include "persistence/ThreadManager.hpp"
#include "Serialization.hpp"
#include "utils/StringUtil.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::core {

namespace {
std::string getThreadsDir() {
    std::string home = getenv("HOME") ? getenv("HOME") : "/root";
    return home + "/.firmius/threads";
}
}

std::vector<std::string> ThreadManager::listThreads() {
    std::vector<std::string> threads;
    std::string dir = getThreadsDir();
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
    std::string dir = getThreadsDir() + "/" + threadId;
    std::filesystem::create_directories(dir);

    rapidjson::Document d = toJson(metadata);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);

    std::ofstream file(dir + "/metadata.json");
    file << buffer.GetString();
    return threadId;
}

ThreadMetadata ThreadManager::getMetadata(const std::string& threadId) {
    std::string path = getThreadsDir() + "/" + threadId + "/metadata.json";
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();

    rapidjson::Document d;
    d.Parse(buffer.str().c_str());
    return threadMetadataFromJson(d);
}

AgentHistory ThreadManager::loadAgentHistory(const std::string& threadId, const std::string& agentId) {
    std::string path = getThreadsDir() + "/" + threadId + "/" + agentId + ".jsonl";
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

std::vector<std::string> ThreadManager::listAgents(const std::string& threadId) {
    std::vector<std::string> agents;
    std::string dir = getThreadsDir() + "/" + threadId;
    if (!std::filesystem::exists(dir)) return {};

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
            agents.push_back(entry.path().stem().string());
        }
    }
    return agents;
}

}
