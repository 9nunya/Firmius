#include "environment/ProcessManager.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <chrono>
#include <thread>

namespace firmius::core {

ProcessManager::ProcessManager(std::shared_ptr<IHost> host,
                               std::function<void(const StreamEvent&)> eventCallback)
    : host_(std::move(host))
    , eventCallback_(std::move(eventCallback))
{
}

ProcessManager::~ProcessManager() {
    cleanup();
}

std::string ProcessManager::spawnProcess(const std::string& command,
                                        const std::string& toolCallId,
                                        const std::string& cwd,
                                        const std::map<std::string, std::string>& env,
                                        bool monitorCompletion) {
    if (!active_.load()) {
        throw std::runtime_error("ProcessManager is not active");
    }
    
    std::string id = StringUtil::generateUuid();
    auto proc = host_->spawn(command, cwd, env);
    
    proc->onOutput([this, id](const std::string& output, bool isError) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        emitOrBufferProcessOutputLocked(id, output, isError, false);
    });
    
    host_->registerBackgroundProcess(id, std::move(proc));
    
    std::lock_guard<std::mutex> lock(processMutex_);
    processIds_.insert(id);
    
    emitProcessSpawned(id, toolCallId, command);
    if (monitorCompletion) {
        std::lock_guard<std::mutex> lock(monitorMutex_);
        monitorThreads_.emplace_back(&ProcessManager::monitorProcessCompletion, this, id);
    }
    return id;
}

ProcessSnapshot ProcessManager::inspectProcess(const std::string& id) {
    return host_->inspectBackgroundProcess(id);
}

void ProcessManager::writeToProcess(const std::string& id, const std::string& data) {
    host_->writeToBackgroundProcess(id, data);
}

void ProcessManager::registerProcessId(const std::string& id) {
    std::lock_guard<std::mutex> lock(processMutex_);
    processIds_.insert(id);
}

void ProcessManager::emitProcessSpawned(const std::string& processId,
                                       const std::string& toolCallId,
                                       const std::string& command) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    spawnedEventEmitted_.insert(processId);
    if (eventCallback_) {
        AgentProcessSpawned event;
        event.processId = processId;
        event.toolCallId = toolCallId;
        event.command = command;
        eventCallback_(event);
    }
    flushBufferedProcessOutputLocked(processId);
}

void ProcessManager::addBlockingProcessId(const std::string& id) {
    std::lock_guard<std::mutex> lock(processMutex_);
    blockingProcessIds_.push_back(id);
}

void ProcessManager::removeBlockingProcessId(const std::string& id) {
    std::lock_guard<std::mutex> lock(processMutex_);
    auto it = std::find(blockingProcessIds_.begin(), blockingProcessIds_.end(), id);
    if (it != blockingProcessIds_.end()) {
        blockingProcessIds_.erase(it);
    }
}

std::vector<std::string> ProcessManager::getBlockingProcessIds() {
    std::lock_guard<std::mutex> lock(processMutex_);
    return blockingProcessIds_;
}

void ProcessManager::killProcess(const std::string& id) {
    try {
        host_->killBackgroundProcess(id);
    } catch (...) {
        // Ignore errors during kill
    }
    
    std::lock_guard<std::mutex> lock(processMutex_);
    processIds_.erase(id);
}

void ProcessManager::cleanup() {
    if (!active_.exchange(false)) {
        return; // Already cleaned up
    }

    std::vector<std::thread> monitorThreads;
    {
        std::lock_guard<std::mutex> lock(monitorMutex_);
        monitorThreads.swap(monitorThreads_);
    }
    for (auto& thread : monitorThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    std::vector<std::string> processIds;
    {
        std::lock_guard<std::mutex> lock(processMutex_);
        processIds.assign(processIds_.begin(), processIds_.end());
        processIds_.clear();
        blockingProcessIds_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        spawnedEventEmitted_.clear();
        pendingOutput_.clear();
    }

    for (const auto& id : processIds) {
        try {
            host_->killBackgroundProcess(id);
        } catch (...) {
            // Ignore errors during cleanup
        }
    }
}

size_t ProcessManager::getProcessCount() const {
    std::lock_guard<std::mutex> lock(processMutex_);
    return processIds_.size();
}

void ProcessManager::monitorProcessCompletion(const std::string& id) {
    using namespace std::chrono_literals;

    while (active_.load()) {
        try {
            auto snap = host_->inspectBackgroundProcess(id);
            if (!snap.running) {
                {
                    std::lock_guard<std::mutex> lock(callbackMutex_);
                    emitOrBufferProcessOutputLocked(
                        id, "", false, true, snap.exitCode, snap.elapsedMs);
                }
                finishTrackedProcess(id);
                return;
            }
        } catch (...) {
            finishTrackedProcess(id);
            return;
        }
        std::this_thread::sleep_for(50ms);
    }
}

void ProcessManager::finishTrackedProcess(const std::string& id) {
    std::lock_guard<std::mutex> lock(processMutex_);
    processIds_.erase(id);
    blockingProcessIds_.erase(
        std::remove(blockingProcessIds_.begin(), blockingProcessIds_.end(), id),
        blockingProcessIds_.end());
}

void ProcessManager::emitOrBufferProcessOutputLocked(const std::string& processId,
                                                    const std::string& output,
                                                    bool isStderr,
                                                    bool finished,
                                                    int exitCode,
                                                    double durationMs) {
    if (spawnedEventEmitted_.count(processId) == 0) {
        pendingOutput_[processId].push_back(
            PendingProcessOutput{output, isStderr, finished, exitCode, durationMs});
        return;
    }
    if (!eventCallback_) {
        return;
    }
    eventCallback_(ProcessOutputDelta{processId, output, isStderr, finished, exitCode,
                                      durationMs});
}

void ProcessManager::flushBufferedProcessOutputLocked(const std::string& processId) {
    auto it = pendingOutput_.find(processId);
    if (it == pendingOutput_.end()) {
        return;
    }
    if (eventCallback_) {
        for (const auto& pending : it->second) {
            eventCallback_(ProcessOutputDelta{processId, pending.output,
                                              pending.isStderr, pending.finished,
                                              pending.exitCode, pending.durationMs});
        }
    }
    pendingOutput_.erase(it);
}

} // namespace firmius::core
