#ifndef FIRMIUS_CORE_ENVIRONMENT_HPP
#define FIRMIUS_CORE_ENVIRONMENT_HPP

#include "IEnvironment.hpp"
#include "ProcessManager.hpp"
#include "Workspace.hpp"
#include <memory>
#include <atomic>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Implementation of IEnvironment that composes ProcessManager and Workspace.
 * 
 * Can be shared between multiple agents via shared_ptr.
 * Handles cleanup when the last reference is destroyed.
 */
class Environment : public IEnvironment,
                    public std::enable_shared_from_this<Environment> {
public:
    /**
     * @brief Constructs an Environment.
     * @param host The host to use for process execution.
     * @param cwd The current working directory.
     * @param eventCallback Callback for process events.
     */
    Environment(std::shared_ptr<IHost> host,
                const std::string& cwd,
                std::function<void(const StreamEvent&)> eventCallback);
    
    ~Environment() override;

    // IEnvironment implementation
    std::string getId() const override;
    IProcessManager& getProcessManager() override;
    IWorkspace& getWorkspace() override;
    std::shared_ptr<IHost> getHost() override;
    void cleanup() override;
    bool isActive() const override;

    /**
     * @brief Creates a shared Environment instance.
     * @param host The host to use.
     * @param cwd The current working directory.
     * @param eventCallback Callback for process events.
     * @return Shared pointer to the new Environment.
     */
    static std::shared_ptr<Environment> create(
        std::shared_ptr<IHost> host,
        const std::string& cwd,
        std::function<void(const StreamEvent&)> eventCallback);

private:
    std::shared_ptr<IHost> host_;
    ProcessManager processManager_;
    Workspace workspace_;
    std::atomic<bool> active_{true};
    mutable std::mutex cleanupMutex_;
    bool cleanedUp_ = false;
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_ENVIRONMENT_HPP