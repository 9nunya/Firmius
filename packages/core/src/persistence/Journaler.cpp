#include "persistence/Journaler.hpp"
#include "Serialization.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <filesystem>
#include <iostream>

namespace firmius::core {

Journaler::Journaler(const std::string& threadId, const std::string& agentId) {
    char* homeEnv = getenv("HOME");
    std::string home = homeEnv ? homeEnv : "/root";
    std::string dir = home + "/.firmius/threads/" + threadId;
    
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    filePath = dir + "/" + agentId + ".jsonl";
    file.open(filePath, std::ios::out | std::ios::app);
}

Journaler::~Journaler() {
    if (file.is_open()) {
        file.flush();
        file.close();
    }
}

void Journaler::appendTurn(const AgentTurn& turn) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!file.is_open()) return;

    rapidjson::Document d = toJson(turn);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);

    file << buffer.GetString() << "\n";
    file.flush();
}

void Journaler::rewriteJournal(const std::vector<AgentTurn>& turns) {
    std::lock_guard<std::mutex> lock(mutex);

    if (file.is_open()) {
        file.close();
    }

    file.open(filePath, std::ios::out | std::ios::trunc);
    if (!file.is_open()) return;

    for (const auto& turn : turns) {
        rapidjson::Document d = toJson(turn);
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        d.Accept(writer);
        file << buffer.GetString() << "\n";
    }
    file.flush();

    file.close();
    file.open(filePath, std::ios::out | std::ios::app);
}

}
