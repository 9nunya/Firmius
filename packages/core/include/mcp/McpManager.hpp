#ifndef FIRMIUS_CORE_MCPMANAGER_HPP
#define FIRMIUS_CORE_MCPMANAGER_HPP

#include "mcp/McpClient.hpp"
#include <functional>
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
  ~McpManager() { shutdown(); }

  /**
   * @brief Process-wide MCP manager. MCP servers are infrastructure, not
   * per-agent scratch processes; parallel agents share one client per server.
   */
  static McpManager &shared() {
    static McpManager instance;
    return instance;
  }

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
   * @brief Atomically gets or creates a client for the server.
   *
   * The factory runs under the manager lock intentionally: process spawn can be
   * slow, but it is cheaper than starting duplicate stdio MCP servers during a
   * parallel subagent wave.
   */
  std::shared_ptr<McpClient>
  getOrCreateClient(const std::string &serverName,
                    const std::function<std::shared_ptr<McpClient>()> &factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(serverName);
    if (it != clients_.end()) {
      return it->second;
    }

    auto client = factory();
    if (client) {
      clients_[serverName] = client;
    }
    return client;
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
    std::map<std::string, std::shared_ptr<McpClient>> clients;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      clients.swap(clients_);
    }
    for (auto &[name, client] : clients) {
      client->shutdown();
    }
  }

  size_t clientCountForTest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.size();
  }

private:
  std::map<std::string, std::shared_ptr<McpClient>> clients_;
  mutable std::mutex mutex_;
};

} // namespace firmius::core::mcp

#endif
