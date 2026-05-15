#pragma once

#include "daemon/Protocol.hpp"
#include "daemon/SocketTransport.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace firmius::daemon {

class DaemonService;

class DaemonServer {
public:
  explicit DaemonServer(DaemonConnectionInfo info = {});
  ~DaemonServer();

  bool start();
  void stop();
  bool running() const;

  const DaemonConnectionInfo &connectionInfo() const;
  const std::string &lastError() const;

private:
  struct ClientConnection;

  void acceptLoop();
  void stopClients();
  void reapFinishedClients();

  DaemonConnectionInfo info_;
  SocketTransport transport_;
  std::atomic<bool> running_{false};
  std::jthread acceptThread_;
  std::unique_ptr<DaemonService> service_;
  mutable std::mutex clientsMutex_;
  std::vector<std::shared_ptr<ClientConnection>> clients_;
};

} // namespace firmius::daemon
