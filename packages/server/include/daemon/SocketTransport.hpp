#pragma once

#include "daemon/Protocol.hpp"
#include "lsp/JsonRpcTransport.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace firmius::daemon {

class SocketTransport {
public:
  struct Channel {
    std::string label;
    firmius::core::JsonRpcTransport::Reader reader;
    firmius::core::JsonRpcTransport::Writer writer;
    firmius::core::JsonRpcTransport::WakeStop wakeStop;
  };

  explicit SocketTransport(DaemonConnectionInfo info = {});
  ~SocketTransport();

  const DaemonConnectionInfo &connectionInfo() const;
  const std::string &lastError() const;
  bool listen();
  void close();
  bool running() const;
  std::optional<Channel> accept();
  Channel connect(std::chrono::milliseconds timeout) const;

private:
#if defined(_WIN32)
  void *listenerHandle_ = nullptr;
#else
  int listenerFd_ = -1;
#endif
  DaemonConnectionInfo info_;
  bool running_ = false;
  std::string lastError_;
};

} // namespace firmius::daemon
