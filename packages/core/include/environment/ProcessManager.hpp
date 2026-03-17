#ifndef FIRMIUS_CORE_PROCESS_MANAGER_HPP
#define FIRMIUS_CORE_PROCESS_MANAGER_HPP

#include "IEnvironment.hpp"
#include "IHost.hpp"
#include <mutex>
#include <set>
#include <map>
#include <memory>
#include <functional>
#include <atomic>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Manages background processes for an environment.
 * 
 * Thread-safe implementation that tracks spawned processes,
 * handles blocking operations, and ensures cleanup.
 */
class ProcessManager : public IProcessManager {
public:
    /**
     * @brief Constructs a ProcessManager.
     * @param host The host to use for spawning processes.
     * @param eventCallback Callback for process events.
     */
    ProcessManager(std::shared_ptr<IHost> host,
                   std::function<void(const StreamEvent&)> eventCallback);
    
    ~ProcessManager() override;

    // IProcessManager implementation
    std::string spawnProcess(const std::string& command,
                            const std::string& toolCallId,
                            const std::string& cwd,
                            const std::map<std::string, std::string>& env) override;
    
    ProcessSnapshot inspectProcess(const std::string& id) override;
    void writeToProcess(const std::string& id, const std::string& data) override;
    void registerProcessId(const std::string& id) override;
    void emitProcessSpawned(const std::string& processId,
                           const std::string& toolCallId,
                           const std::string& command) override;
    void addBlockingProcessId(const std::string& id) override;
    void removeBlockingProcessId(const std::string& id) override;
    std::vector<std::string> getBlockingProcessIds() override;
    void killProcess(const std::string& id) override;

    /**
     * @brief Cleans up all managed processes.
     * Called during environment destruction.
     */
    void cleanup();

    /**
     * @brief Gets the count of active processes.
     * @return Number of processes.
     */
    size_t getProcessCount() const;

private:
    std::shared_ptr<IHost> host_;
    std::function<void(const StreamEvent&)> eventCallback_;
    
    mutable std::mutex processMutex_;
    mutable std::mutex callbackMutex_;
    std::set<std::string> processIds_;
    std::vector<std::string> blockingProcessIds_;
    
    std::atomic<bool> active_{true};
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_PROCESS_MANAGER_HPP