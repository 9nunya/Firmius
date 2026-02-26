#ifndef FIRMIUS_CORE_DOCKER_HOST_HPP
#define FIRMIUS_CORE_DOCKER_HOST_HPP

#include "IHost.hpp"
#include <curl/curl.h>

namespace firmius::core {
using namespace firmius::shared;

using namespace firmius::shared;

/**
 * @brief Host implementation for sandboxed Docker execution.
 */
class DockerHost : public shared::IHost {
public:
    /**
     * @brief Constructs a DockerHost.
     * @param containerId The ID of the container to use.
     */
    DockerHost(const std::string& containerId);
    ~DockerHost() override;

    void init() override;
    void destroy() override;

    std::vector<uint8_t> readFile(const std::string& path) override;
    void writeFile(const std::string& path, const std::vector<uint8_t>& data) override;
    bool exists(const std::string& path) override;

    shared::ProcessResult exec(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}) override;
    std::unique_ptr<shared::IHostProcess> spawn(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}) override;

private:
    /**
     * @brief Internal helper to send a request to the Docker Engine API.
     */
    std::string request(const std::string& method, const std::string& url, const std::string& body = "");
    
    std::string containerId;
    CURL* curl;
};

}

#endif
