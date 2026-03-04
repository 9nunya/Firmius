#include "harness/Harness.hpp"
#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "IHost.hpp"
#include "persistence/ThreadManager.hpp"
#include "agents/PurposeLoader.hpp"
#include "hosts/DockerHost.hpp"
#include "hosts/LocalHost.hpp"
#include "utils/StringUtil.hpp"
#include "providers/ProviderRegistry.hpp"
#include <Panic.hpp>
#include <EnvLoader.hpp>
#include <Context.hpp>
#include <Events.hpp>
#include <Serialization.hpp>
#include <HarnessEvents.hpp>

#include <memory>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <signal.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <curl/curl.h>

namespace firmius::core {

using namespace firmius::shared;

namespace {
const std::string FIRMIUS_DIR = ".firmius";
const std::string SESSION_FILE = "last_session.json";
const std::string LOCK_FILE = ".lock";
const std::string OWNER_PID_LABEL = "com.firmius.owner_pid";

std::string getFirmiusHome() {
    const char* home = std::getenv("HOME");
    if (!home) {
        home = "/tmp";
    }
    return std::string(home) + "/" + FIRMIUS_DIR;
}

std::string getSessionPath() {
    return getFirmiusHome() + "/" + SESSION_FILE;
}

std::string getThreadDir(const std::string& threadId) {
    return getFirmiusHome() + "/threads/" + threadId;
}

std::string getLockFilePath(const std::string& threadId) {
    return getThreadDir(threadId) + "/" + LOCK_FILE;
}

bool isPidAlive(pid_t pid) {
    return kill(pid, 0) == 0 || errno == EPERM;
}

size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

void killDockerContainer(const std::string& containerId) {
    CURL* curl = curl_easy_init();
    if (curl) {
        std::string response;
        curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, "/var/run/docker.sock");
        curl_easy_setopt(curl, CURLOPT_URL, ("http://localhost/v1.44/containers/" + containerId + "?force=true").c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
}
} // namespace

Harness& Harness::instance() {
    static Harness instance;
    return instance;
}

Harness::Harness() : nextSubscriptionId_(0) {}

void Harness::init() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Create firmius directory if it doesn't exist
    std::filesystem::create_directories(getFirmiusHome());

    PurposeLoader::bootstrapDefaults("prompts/");
    shared::ConfigLoader::instance().load();

    // Cleanup orphaned Docker containers
    auto containers = DockerHost::listContainersWithLabel(OWNER_PID_LABEL);
    for (const auto& container : containers) {
        auto it = container.labels.find(OWNER_PID_LABEL);
        if (it != container.labels.end()) {
            try {
                pid_t ownerPid = std::stoi(it->second);
                if (!isPidAlive(ownerPid)) {
                    killDockerContainer(container.id);
                }
            } catch (...) {}
        }
    }

    // Subscribe to Engine events
    Engine::instance().addEventListener([this](const EngineEvent& event) {
        this->routeEngineEvent(event);
    });

    // Load last session
    std::ifstream sessionFile(getSessionPath());
    if (sessionFile.is_open()) {
        std::string content((std::istreambuf_iterator<char>(sessionFile)),
                            std::istreambuf_iterator<char>());
        sessionFile.close();

        rapidjson::Document doc;
        doc.Parse(content.c_str());
        if (!doc.HasParseError()) {
            if (doc.HasMember("threadId") && doc["threadId"].IsString()) {
                currentThreadId_ = doc["threadId"].GetString();
            }
            if (doc.HasMember("focusedAgentId") && doc["focusedAgentId"].IsString()) {
                focusedAgentId_ = doc["focusedAgentId"].GetString();
            }
        }

        if (!currentThreadId_.empty() && !focusedAgentId_.empty()) {
            threadAgentMap_[currentThreadId_] = focusedAgentId_;
        }
    }
}

void Harness::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Release all thread locks
    for (auto& [threadId, fd] : threadLocks_) {
        if (fd >= 0) {
            flock(fd, LOCK_UN);
            close(fd);
        }
    }
    threadLocks_.clear();

    // Write current session
    rapidjson::Document doc;
    doc.SetObject();
    auto& a = doc.GetAllocator();

    if (!currentThreadId_.empty()) {
        doc.AddMember("threadId", rapidjson::Value(currentThreadId_.c_str(), a), a);
    }
    if (!focusedAgentId_.empty()) {
        doc.AddMember("focusedAgentId", rapidjson::Value(focusedAgentId_.c_str(), a), a);
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    std::ofstream sessionFile(getSessionPath());
    if (sessionFile.is_open()) {
        sessionFile << buffer.GetString();
        sessionFile.close();
    }
}

std::string Harness::newThread(HostType hostType, const std::string& cwd, const std::string& leadPersona) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    ThreadMetadata newMeta;
    newMeta.title = "Thread-" + StringUtil::generateUuid().substr(0, 8);
    newMeta.hostType = hostType;
    newMeta.hostIdentifier = (hostType == HostType::Docker) ? "" : "localhost";
    newMeta.cwd = cwd;
    newMeta.leadPersona = leadPersona;

    std::string threadId = ThreadManager::createThread(newMeta);

    int fd = acquireThreadLock(threadId);
    if (fd < 0) {
        int ownerPid = -1;
        if (fd == -2) {
            std::string lockPath = getLockFilePath(threadId);
            std::ifstream lf(lockPath);
            if (lf.is_open()) {
                std::string pidStr;
                lf >> pidStr;
                try { ownerPid = std::stoi(pidStr); } catch (...) {}
            }
        }
        emitEvent(harness::ThreadLocked{threadId, ownerPid});
        return "";
    }

    if (!currentThreadId_.empty() && !focusedAgentId_.empty()) {
        threadAgentMap_[currentThreadId_] = focusedAgentId_;
    }

    threadLocks_[threadId] = fd;
    currentThreadId_ = threadId;
    focusedAgentId_.clear();

    auto metadata = ThreadManager::getMetadata(threadId);
    emitEvent(harness::ThreadChanged{threadId, metadata});

    return threadId;
}

bool Harness::switchThread(const std::string& threadId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (threadLocks_.count(threadId)) {
        if (!currentThreadId_.empty() && !focusedAgentId_.empty()) {
            threadAgentMap_[currentThreadId_] = focusedAgentId_;
        }

        currentThreadId_ = threadId;

        auto it = threadAgentMap_.find(threadId);
        if (it != threadAgentMap_.end()) {
            focusedAgentId_ = it->second;
        } else {
            focusedAgentId_.clear();
        }
        
        auto metadata = ThreadManager::getMetadata(threadId);
        emitEvent(harness::ThreadChanged{threadId, metadata});
        return true;
    }

    int newFd = acquireThreadLock(threadId);
    if (newFd < 0) {
        int ownerPid = -1;
        if (newFd == -2) {
            std::string lockPath = getLockFilePath(threadId);
            std::ifstream lf(lockPath);
            if (lf.is_open()) {
                std::string pidStr;
                lf >> pidStr;
                try { ownerPid = std::stoi(pidStr); } catch (...) {}
            }
        }
        emitEvent(harness::ThreadLocked{threadId, ownerPid});
        return false;
    }

    if (!currentThreadId_.empty() && !focusedAgentId_.empty()) {
        threadAgentMap_[currentThreadId_] = focusedAgentId_;
    }

    if (!currentThreadId_.empty()) {
        releaseThreadLock(currentThreadId_);
        threadLocks_.erase(currentThreadId_);
    }

    threadLocks_[threadId] = newFd;
    currentThreadId_ = threadId;

    // Re-hydrate agents from manifest for this thread
    try {
        auto manifest = ThreadManager::readAgentManifest(threadId);
        for (const auto& [agentId, entry] : manifest) {
            // Skip if agent already exists in registry (e.g., from previous session still alive)
            if (!AgentRegistry::instance().getAgent(agentId)) {
                Engine::instance().resumeAgent(
                    threadId, agentId, entry.persona, entry.parentId,
                    entry.friendlyName, entry.title, entry.persistHistory
                );
            }
        }
    } catch (...) {
        // Ignore manifest errors; proceed with thread switch
    }

    auto it = threadAgentMap_.find(threadId);
    if (it != threadAgentMap_.end()) {
        focusedAgentId_ = it->second;
    } else {
        focusedAgentId_.clear();
    }

    auto metadata = ThreadManager::getMetadata(threadId);
    emitEvent(harness::ThreadChanged{threadId, metadata});

    return true;
}

bool Harness::resumeLast() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (currentThreadId_.empty()) return false;
    return switchThread(currentThreadId_);
}

void Harness::send(const std::string& text) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (currentThreadId_.empty()) {
        emitEvent(harness::HarnessError{"No current thread active"});
        return;
    }

    auto metadata = ThreadManager::getMetadata(currentThreadId_);
    metadata.lastActiveAt = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    ThreadManager::updateMetadata(currentThreadId_, metadata);

    if (focusedAgentId_.empty()) {
        focusedAgentId_ = Engine::instance().summonAgent(currentThreadId_, metadata.leadPersona, text, true);
        if (focusedAgentId_.empty()) {
            emitEvent(harness::HarnessError{"Failed to summon lead agent"});
            return;
        }
        threadAgentMap_[currentThreadId_] = focusedAgentId_;
        return;
    }

    auto agent = AgentRegistry::instance().getAgent(focusedAgentId_);
    if (agent && agent->isRunning()) {
        std::string messageId = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        messageQueue_.push({messageId, text});
        emitEvent(harness::MessageQueued{messageId, text});
        return;
    }

    Engine::instance().executeTask(focusedAgentId_, text);
}

void Harness::abort() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (focusedAgentId_.empty()) return;

    auto agent = AgentRegistry::instance().getAgent(focusedAgentId_);
    if (!agent) return;

    auto procIds = agent->getBlockingProcessIds();
    for (const auto& procId : procIds) {
        try {
            // Surgical Interrupt in OS if numeric PID
            if (std::all_of(procId.begin(), procId.end(), ::isdigit)) {
                kill(std::stoi(procId), SIGKILL);
            }
            // Also kill via host handle
            agent->getHost()->killBackgroundProcess(procId);
        } catch (...) {}
    }

    agent->interrupt();
}

int Harness::subscribe(std::function<void(const harness::HarnessEvent&)> callback) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int id = nextSubscriptionId_++;
    subscribers_[id] = std::move(callback);
    return id;
}

void Harness::unsubscribe(const int& subscriptionId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    subscribers_.erase(subscriptionId);
}

std::string Harness::currentThreadId() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return currentThreadId_;
}

std::string Harness::focusedAgentId() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return focusedAgentId_;
}

int Harness::acquireThreadLock(const std::string& threadId) {
    std::string dir = getThreadDir(threadId);
    std::filesystem::create_directories(dir);
    std::string path = getLockFilePath(threadId);

    int fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            close(fd);
            return -2;
        }
        close(fd);
        return -1;
    }

    ftruncate(fd, 0);
    std::string pid = std::to_string(getpid());
    write(fd, pid.c_str(), pid.size());
    fsync(fd);

    return fd;
}

void Harness::releaseThreadLock(const std::string& threadId) {
    auto it = threadLocks_.find(threadId);
    if (it != threadLocks_.end()) {
        flock(it->second, LOCK_UN);
        close(it->second);
    }
}

bool Harness::isDescendant(const std::string& agentId, const std::string& ancestorId, int depth) {
    if (depth > 100) return false; // cycle protection
    if (agentId == ancestorId) return true;
    auto it = agentParentMap_.find(agentId);
    if (it != agentParentMap_.end()) {
        return isDescendant(it->second, ancestorId, depth + 1);
    }
    return false;
}

void Harness::emitEvent(const harness::HarnessEvent& event) {
    for (auto& [id, cb] : subscribers_) {
        cb(event);
    }
}

void Harness::routeEngineEvent(const EngineEvent& event) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    std::string agentId;
    std::string parentId;

    std::visit([&](auto&& ev) {
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T, AgentSpawned>) {
            agentId = ev.agentId;
            parentId = ev.parentId;
            if (!parentId.empty()) agentParentMap_[agentId] = parentId;
        } else if constexpr (requires { ev.agentId; }) {
            agentId = ev.agentId;
        }
        
        if constexpr (requires { ev.parentId; }) {
            parentId = ev.parentId;
        }
    }, event);

    if (focusedAgentId_.empty() || !isDescendant(agentId, focusedAgentId_)) return;

    if (auto* e = std::get_if<AgentText>(&event)) {
        emitEvent(harness::MessageChunk{e->agentId, e->delta, false});
    } else if (auto* e = std::get_if<AgentThinking>(&event)) {
        emitEvent(harness::MessageChunk{e->agentId, e->delta, true});
    } else if (auto* e = std::get_if<AgentToolCall>(&event)) {
        emitEvent(harness::ToolCallStarted{e->agentId, e->toolCallId, e->toolName, e->toolArgs});
    } else if (auto* e = std::get_if<AgentTurnCompleted>(&event)) {
        if (!e->turn.messages.empty()) {
            const auto& msg = e->turn.messages.back();
            emitEvent(harness::MessageCompleted{e->agentId, msg, e->turn.metrics});

            // Also emit ToolCallResult if the turn contains tool results
            for (const auto& part : msg.content) {
                if (auto* tr = std::get_if<ToolResultContent>(&part)) {
                    emitEvent(harness::ToolCallResult{e->agentId, tr->toolCallId, tr->result, tr->success});
                }
            }

            // Trigger title generation on first turn if not already done
            if (!currentThreadId_.empty() &&
                titleGeneratedThreads_.find(currentThreadId_) == titleGeneratedThreads_.end()) {
                std::string firstMessage;
                for (const auto& turnMsg : e->turn.messages) {
                    for (const auto& part : turnMsg.content) {
                        if (auto* txt = std::get_if<TextContent>(&part)) {
                            firstMessage += txt->text;
                        }
                    }
                }
                if (!firstMessage.empty()) {
                    titleGeneratedThreads_.insert(currentThreadId_);
                    maybeGenerateTitle(currentThreadId_, firstMessage);
                }
            }
        }
        // Drain one message from queue if this is the focused agent
        if (e->agentId == focusedAgentId_) {
            drainQueue();
        }
    } else if (auto* e = std::get_if<AgentSpawned>(&event)) {
        emitEvent(harness::SubagentSpawned{e->parentId, e->agentId, e->personaName, e->friendlyName, e->title});

        // Write agent manifest entry for current thread
        if (!currentThreadId_.empty()) {
            try {
                auto manifest = ThreadManager::readAgentManifest(currentThreadId_);
                AgentManifestEntry entry;
                entry.persona = e->personaName;
                entry.parentId = e->parentId;
                entry.friendlyName = e->friendlyName;
                entry.title = e->title;
                entry.persistHistory = e->persistHistory;
                manifest[e->agentId] = entry;
                ThreadManager::writeAgentManifest(currentThreadId_, manifest);
            } catch (...) {
                // Ignore manifest errors
            }
        }
    } else if (auto* e = std::get_if<AgentCompleted>(&event)) {
        // Drain one message from queue if this is the focused agent
        if (e->agentId == focusedAgentId_) {
            drainQueue();
        }
    } else if (auto* e = std::get_if<AgentProcessOutput>(&event)) {
        emitEvent(harness::ProcessOutputChunk{e->agentId, e->processId, e->output, e->isStderr});
    } else if (auto* e = std::get_if<AgentError>(&event)) {
        emitEvent(harness::HarnessError{e->message});
    } else if (auto* e = std::get_if<AgentCompacting>(&event)) {
        emitEvent(harness::AgentCompactingEvent{e->agentId});
    } else if (auto* e = std::get_if<ContextCompacted>(&event)) {
        emitEvent(harness::ContextCompactedEvent{e->agentId, e->tokensSaved});
    } else if (auto* e = std::get_if<ModelSwitched>(&event)) {
        emitEvent(harness::ModelSwitchedEvent{e->agentId, e->oldProviderId, e->oldModelId, e->newProviderId, e->newModelId});
    } else if (auto* e = std::get_if<AgentRetrying>(&event)) {
        emitEvent(harness::AgentRetrying{e->agentId, e->attempt, e->maxAttempts, e->httpStatus, e->delayMs, e->reason});
    } else if (auto* e = std::get_if<AgentRetryFailed>(&event)) {
        emitEvent(harness::AgentRetryFailed{e->agentId, e->httpStatus, e->reason});
    } else if (auto* e = std::get_if<HistoryUndone>(&event)) {
        emitEvent(harness::HistoryUndoneEvent{e->threadId, e->agentId, e->turnsRemoved, e->compactionReversed});
    }
}

void Harness::deleteThread(const std::string& threadId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (threadId == currentThreadId_) {
        emitEvent(harness::HarnessError{"Cannot delete the currently active thread"});
        return;
    }

    if (threadLocks_.count(threadId)) {
        releaseThreadLock(threadId);
        threadLocks_.erase(threadId);
    }

    threadAgentMap_.erase(threadId);
    ThreadManager::deleteThread(threadId);
    emitEvent(harness::ThreadDeleted{threadId});
}

std::vector<ThreadMetadata> Harness::listThreads() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ThreadManager::listThreadsWithMetadata();
}

const UserConfig& Harness::getConfig() {
    return shared::ConfigLoader::instance().getConfig();
}

void Harness::updateConfig(const UserConfig& config) {
    shared::ConfigLoader::instance().updateConfig(config);
    shared::ConfigLoader::instance().save();
    emitEvent(harness::ConfigUpdated{});
}

void Harness::switchModel(const std::string& providerId, const std::string& modelId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (focusedAgentId_.empty()) {
        emitEvent(harness::HarnessError{"No focused agent to switch model on"});
        return;
    }
    Engine::instance().switchAgentModel(focusedAgentId_, providerId, modelId);
}

void Harness::interruptAndSwitchModel(const std::string& providerId, const std::string& modelId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (focusedAgentId_.empty()) {
        emitEvent(harness::HarnessError{"No focused agent to switch model on"});
        return;
    }
    abort();
    // Small delay to allow abort to propagate
    Engine::instance().switchAgentModel(focusedAgentId_, providerId, modelId);
}

UndoResult Harness::undoTurns(int count) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (focusedAgentId_.empty()) {
        emitEvent(harness::HarnessError{"No focused agent for undo"});
        return {};
    }
    auto result = Engine::instance().undoAgentTurns(focusedAgentId_, count);
    return result;
}

UndoResult Harness::undoMessages(int count) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (focusedAgentId_.empty()) {
        emitEvent(harness::HarnessError{"No focused agent for undo"});
        return {};
    }
    auto result = Engine::instance().undoAgentMessages(focusedAgentId_, count);
    return result;
}

UndoResult Harness::undoAfterTimestamp(uint64_t timestamp) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (focusedAgentId_.empty()) {
        emitEvent(harness::HarnessError{"No focused agent for undo"});
        return {};
    }
    auto result = Engine::instance().undoAgentAfterTimestamp(focusedAgentId_, timestamp);
    return result;
}

void Harness::maybeGenerateTitle(const std::string& threadId, const std::string& firstMessage) {
    std::thread([this, threadId, firstMessage]() {
        try {
            auto metadata = ThreadManager::getMetadata(threadId);
            auto agent = AgentRegistry::instance().getAgent(threadAgentMap_[threadId]);
            if (!agent) return;

            const auto& config = agent->getContext().config;
            auto provider = firmius::provider::ProviderRegistry::instance().getProvider(config.providerId);
            if (!provider) return;

            std::string titlerPrompt = PurposeLoader::load("titler").identityPrompt;
            std::string fullPrompt = titlerPrompt + "\n\nUser's first message: " + firstMessage;

            std::string generatedTitle;
            shared::AgentHistory history;
            history.threadId = threadId;
            shared::AgentTurn turn;
            turn.turnId = "titler-turn";
            shared::Message msg;
            msg.role = shared::Role::User;
            msg.content.push_back(shared::TextContent{fullPrompt});
            msg.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            turn.messages.push_back(msg);
            history.turns.push_back(turn);
            
            firmius::provider::ProviderOptions opts;
            opts.modelId = config.modelId;
            provider->stream(history, opts,
                [&](const shared::StreamEvent& ev) {
                    if (auto* txt = std::get_if<shared::TextChunk>(&ev)) {
                        generatedTitle += txt->delta;
                    }
                });

            generatedTitle = StringUtil::trim(generatedTitle);
            if (!generatedTitle.empty() && generatedTitle.length() <= 100) {
                metadata.title = generatedTitle;
                metadata.lastActiveAt = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count()
                );
                ThreadManager::updateMetadata(threadId, metadata);
                emitEvent(harness::ThreadTitleUpdated{threadId, generatedTitle});
            }
        } catch (...) {
        }
    }).detach();
}

void Harness::drainQueue() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (messageQueue_.empty()) {
        return;
    }

    auto [id, text] = messageQueue_.front();
    messageQueue_.pop();

    // Unlock mutex before calling executeTask to avoid deadlock
    // (executeTask may trigger events that route back to Harness)
    mutex_.unlock();
    emitEvent(harness::MessageDequeued{id});
    Engine::instance().executeTask(focusedAgentId_, text);
    mutex_.lock();
}

void Harness::writeInterruptionRecord() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (currentThreadId_.empty()) return;

    ThreadMetadata metadata;
    try {
        metadata = ThreadManager::getMetadata(currentThreadId_);
    } catch (...) {
        return;
    }

    std::string journalDir = getThreadDir(currentThreadId_) + "/journal";
    std::filesystem::create_directories(journalDir);

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    rapidjson::Document doc;
    doc.SetObject();
    auto& a = doc.GetAllocator();

    doc.AddMember("type", "interrupted", a);
    doc.AddMember("timestamp", static_cast<uint64_t>(now), a);

    rapidjson::Value toolsArray(rapidjson::kArrayType);
    auto activeAgents = AgentRegistry::instance().listAll();
    for (const auto& agentId : activeAgents) {
        auto agent = AgentRegistry::instance().getAgent(agentId);
        if (!agent) continue;
        const auto& state = agent->getContext().state;
        for (const auto& tcId : state.pendingToolCalls) {
            toolsArray.PushBack(rapidjson::Value(tcId.c_str(), a), a);
        }
    }
    doc.AddMember("inFlightTools", toolsArray, a);

    rapidjson::Value subagentsArray(rapidjson::kArrayType);
    for (const auto& agentId : activeAgents) {
        auto agent = AgentRegistry::instance().getAgent(agentId);
        if (!agent) continue;
        if (agent->isRunning()) {
            subagentsArray.PushBack(rapidjson::Value(agentId.c_str(), a), a);
        }
    }
    doc.AddMember("activeSubagents", subagentsArray, a);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    std::string path = journalDir + "/interruption_" + std::to_string(now) + ".json";
    std::ofstream file(path);
    if (file.is_open()) {
        file << buffer.GetString();
    }
}

} // namespace firmius::core
