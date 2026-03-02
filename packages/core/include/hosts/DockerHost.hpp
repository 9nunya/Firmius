#ifndef FIRMIUS_CORE_DOCKER_HOST_HPP
#define FIRMIUS_CORE_DOCKER_HOST_HPP

#include "IHost.hpp"
#include <curl/curl.h>
#include <mutex>
#include <map>
#include <memory>
#include <optional>
#include <chrono>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Host implementation for sandboxed Docker execution.
 * Communicates with the Docker Engine API via the Unix socket.
 */
class DockerHost : public shared::IHost {
public:
    /**
     * @brief Constructs a DockerHost.
     * @param containerId The ID of the container to use.
     */
    DockerHost(const std::string& containerId);
    DockerHost();
    ~DockerHost() override;

    void init() override;
    void destroy() override;
    void cleanup() override;
    void setUser(const std::string& user) override;

    std::vector<uint8_t> readFile(const std::string& path) override;
    void writeFile(const std::string& path, const std::vector<uint8_t>& data) override;
    bool exists(const std::string& path) override;
    std::vector<shared::FileInfo> listDir(const std::string& path) override;
    shared::FileInfo stat(const std::string& path) override;

    shared::ProcessResult exec(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}, std::optional<std::chrono::milliseconds> timeout = std::nullopt) override;
    std::unique_ptr<shared::IHostProcess> spawn(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}) override;

    std::string registerBackgroundProcess(std::unique_ptr<shared::IHostProcess> proc) override;
    shared::ProcessSnapshot inspectBackgroundProcess(const std::string& id) override;
    void writeToBackgroundProcess(const std::string& id, const std::string& data) override;
    void killBackgroundProcess(const std::string& id) override;

private:
    /**
     * @brief Internal helper to send a request to the Docker Engine API.
     * @param method HTTP method.
     * @param url API endpoint path.
     * @param body Optional JSON request body.
     * @return Raw API response.
     */
    std::string request(const std::string& method, const std::string& url, const std::string& body = "");

    std::string containerId;
    CURL* curl;
    std::string currentUser;
    std::vector<std::string> containerIds;
    std::mutex requestMutex;  ///< Guards the shared curl handle in request().
    std::map<std::string, std::unique_ptr<shared::IHostProcess>> backgroundProcesses;
    mutable std::mutex bgMutex;
};

}

#endif
