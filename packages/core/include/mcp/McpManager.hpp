#ifndef FIRMIUS_CORE_MCP_MANAGER_HPP
#define FIRMIUS_CORE_MCP_MANAGER_HPP

#include "mcp/McpClient.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace firmius::core::mcp {

/**
 * @brief Manages active MCP client sessions.
 */
class McpManager {
public:
  McpManager() = default;
  ~McpManager() = default;

  /**
   * @brief Gets or creates an MCP client for the given server.
   */
  std::shared_ptr<McpClient> getClient(const std::string &serverName) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(serverName);
    if (it != clients_.end()) {
      return it->second;
    }
    return nullptr;
  }

  /**
   * @brief Registers an active MCP client.
   */
  void registerClient(const std::string &serverName, std::shared_ptr<McpClient> client) {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_[serverName] = client;
  }

  /**
   * @brief Removes an MCP client.
   */
  void removeClient(const std::string &serverName) {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.erase(serverName);
  }

  /**
   * @brief Shuts down all managed clients.
   */
  void shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &[name, client] : clients_) {
      client->shutdown();
    }
    clients_.clear();
  }

private:
  std::map<std::string, std::shared_ptr<McpClient>> clients_;
  std::mutex mutex_;
};

} // namespace firmius::core::mcp

#endif
