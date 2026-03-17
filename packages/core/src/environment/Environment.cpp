#include "environment/Environment.hpp"
#include <algorithm>

namespace firmius::core {

Environment::Environment(std::shared_ptr<IHost> host,
                         const std::string& cwd,
                         std::function<void(const StreamEvent&)> eventCallback)
    : host_(std::move(host))
    , processManager_(host_, std::move(eventCallback))
    , workspace_(cwd)
{
}

Environment::~Environment() {
    cleanup();
}

std::string Environment::getId() const {
    if (host_) {
        return host_->getId();
    }
    return "unknown";
}

IProcessManager& Environment::getProcessManager() {
    return processManager_;
}

IWorkspace& Environment::getWorkspace() {
    return workspace_;
}

std::shared_ptr<IHost> Environment::getHost() {
    return host_;
}

void Environment::cleanup() {
    std::lock_guard<std::mutex> lock(cleanupMutex_);
    if (cleanedUp_) {
        return;
    }
    
    active_ = false;
    processManager_.cleanup();
    
    if (host_) {
        host_->destroy();
    }
    
    cleanedUp_ = true;
}

bool Environment::isActive() const {
    return active_.load();
}

std::shared_ptr<Environment> Environment::create(
    std::shared_ptr<IHost> host,
    const std::string& cwd,
    std::function<void(const StreamEvent&)> eventCallback) {
    
    return std::shared_ptr<Environment>(
        new Environment(std::move(host), cwd, std::move(eventCallback))
    );
}

} // namespace firmius::core