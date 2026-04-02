#include "persistence/Journaler.hpp"
#include "Serialization.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <filesystem>
#include <iostream>
#include <unordered_map>
#include <variant>
#include <fcntl.h>
#include <unistd.h>

namespace firmius::core {

namespace {

std::shared_ptr<std::mutex> acquireJournalFileMutex(
    const std::string &filePath) {
    static std::mutex registryMutex;
    static std::unordered_map<std::string, std::weak_ptr<std::mutex>> registry;

    std::lock_guard<std::mutex> guard(registryMutex);
    if (auto existing = registry[filePath].lock()) {
        return existing;
    }

    auto created = std::make_shared<std::mutex>();
    registry[filePath] = created;
    return created;
}

}

Journaler::Journaler(const std::string& threadId, const std::string& agentId) {
    char* homeEnv = getenv("HOME");
    std::string home = homeEnv ? homeEnv : "/root";
    std::string dir = home + "/.firmius/threads/" + threadId;
    
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    filePath = dir + "/" + agentId + ".jsonl";
    fileMutex = acquireJournalFileMutex(filePath);
    file.open(filePath, std::ios::out | std::ios::app);

    workerThread = std::jthread([this](std::stop_token) { processQueue(); });
}

Journaler::~Journaler() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopWorker = true;
    }
    queueCv.notify_all();

    if (workerThread.joinable()) {
        workerThread.join();
    }

    if (file.is_open()) {
        file.close();
    }
}

void Journaler::appendTurn(const AgentTurn& turn) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push(AppendOp{turn});
    }
    queueCv.notify_one();
}

void Journaler::rewriteJournal(const std::vector<AgentTurn>& turns) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push(RewriteOp{turns});
    }
    queueCv.notify_one();
}

void Journaler::processQueue() {
    while (true) {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCv.wait(lock, [this]() { return !queue.empty() || stopWorker; });

        if (stopWorker && queue.empty()) {
            break;
        }

        auto op = std::move(queue.front());
        queue.pop();
        lock.unlock();

        std::visit([this](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, AppendOp>) {
                writeTurn(arg.turn);
            } else if constexpr (std::is_same_v<T, RewriteOp>) {
                performRewrite(arg.turns);
            }
        }, op);
    }
}

void Journaler::writeTurn(const AgentTurn& turn) {
    std::lock_guard<std::mutex> fileLock(*fileMutex);
    if (!file.is_open()) return;

    rapidjson::Document d = toJson(turn);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);

    file << buffer.GetString() << "\n";
    file.flush();
    file.rdbuf()->pubsync();
}

void Journaler::performRewrite(const std::vector<AgentTurn>& turns) {
    std::lock_guard<std::mutex> fileLock(*fileMutex);
    if (file.is_open()) {
        file.close();
    }

    // Write to temp file for atomic rewrite
    std::string tempPath = filePath + ".tmp";

    // Serialize all turns to a string first
    std::string content;
    for (const auto& turn : turns) {
        rapidjson::Document d = toJson(turn);
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        d.Accept(writer);
        content += buffer.GetString();
        content += "\n";
    }

    // Write to temp file with explicit fsync
    int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        file.open(filePath, std::ios::out | std::ios::app);
        return;
    }

    const char* data = content.data();
    std::size_t remaining = content.size();
    bool writeOk = true;
    while (remaining > 0) {
        ssize_t written = ::write(fd, data, remaining);
        if (written < 0) {
            writeOk = false;
            break;
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }

    if (writeOk && ::fsync(fd) == 0) {
        ::close(fd);
        // Atomic rename: temp file becomes the journal
        std::error_code ec;
        std::filesystem::rename(tempPath, filePath, ec);
        if (ec) {
            std::filesystem::remove(tempPath, ec);
        }
    } else {
        ::close(fd);
        std::filesystem::remove(tempPath);
    }

    // Reopen in append mode
    file.open(filePath, std::ios::out | std::ios::app);
}

}
