#ifndef FIRMIUS_CORE_DOCKER_HOST_HPP
#define FIRMIUS_CORE_DOCKER_HOST_HPP

#include "IHost.hpp"
#include <curl/curl.h>
#include <mutex>
#include <map>
#include <memory>
#include <optional>
#include <chrono>
#include <vector>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Information about a Docker container.
 */
struct ContainerInfo {
    std::string id;
    std::map<std::string, std::string> labels;
};

/**
 * @brief Host implementation for sandboxed Docker execution.
 * Communicates with the Docker Engine API via the Unix socket.
 */
class DockerHost : public shared::IHost {
public:
    explicit DockerHost(const shared::HostCreationOptions& options);
    ~DockerHost() override;

    std::string init() override;
    void destroy() override;
    void cleanup() override;
    void setUser(const std::string& user) override;
    std::string getId() const override { return containerId; }

    std::vector<uint8_t> readFile(const std::string& path) override;
    void writeFile(const std::string& path, const std::vector<uint8_t>& data) override;
    void deleteFile(const std::string& path) override;
    bool exists(const std::string& path) override;
    std::vector<shared::FileInfo> listDir(const std::string& path) override;
    shared::FileInfo stat(const std::string& path) override;

    shared::ProcessResult exec(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}, std::optional<std::chrono::milliseconds> timeout = std::nullopt) override;
    std::unique_ptr<shared::IHostProcess> spawn(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}) override;

    void registerBackgroundProcess(const std::string& id, std::unique_ptr<shared::IHostProcess> proc) override;
    shared::ProcessSnapshot inspectBackgroundProcess(const std::string& id) override;
    void releaseBackgroundProcess(const std::string& id) override;
    void writeToBackgroundProcess(const std::string& id, const std::string& data) override;
    void killBackgroundProcess(const std::string& id) override;

    /**
     * @brief Lists all containers that have the specified label.
     * @param label The label key to search for.
     * @return Vector of ContainerInfo for matching containers.
     */
    static std::vector<ContainerInfo> listContainersWithLabel(const std::string& label);

private:
    struct CompletedProcessSnapshot {
        shared::ProcessSnapshot snapshot;
        std::chrono::steady_clock::time_point completedAt;
    };

    static constexpr size_t kMaxCompletedBackgroundProcesses = 64;

    void promoteCompletedProcessLocked(const std::string& id,
                                       std::unique_ptr<shared::IHostProcess> proc,
                                       const shared::ProcessSnapshot& snapshot);
    std::map<std::string, CompletedProcessSnapshot>::iterator
    touchCompletedProcessLocked(const std::string& id);

    /**
     * @brief Internal helper to send a request to the Docker Engine API.
     * @param method HTTP method.
     * @param url API endpoint path.
     * @param body Optional JSON request body.
     * @return Raw API response.
     */
    std::string request(const std::string& method, const std::string& url, const std::string& body = "");

    std::string containerId;
    shared::HostCreationOptions options;
    CURL* curl;
    std::string currentUser;
    std::vector<std::string> containerIds;
    std::mutex requestMutex;  ///< Guards the shared curl handle in request().
    std::map<std::string, std::unique_ptr<shared::IHostProcess>> backgroundProcesses;
    std::map<std::string, CompletedProcessSnapshot> completedBackgroundProcesses;
    mutable std::mutex bgMutex;
};

}

#endif