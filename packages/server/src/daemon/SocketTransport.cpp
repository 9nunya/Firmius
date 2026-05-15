#include "daemon/SocketTransport.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace firmius::daemon {

namespace {
#if defined(_WIN32)
std::wstring utf8ToWide(const std::string &value) {
  if (value.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  std::wstring wide(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), size);
  if (!wide.empty() && wide.back() == L'\0') {
    wide.pop_back();
  }
  return wide;
}

class PipeHandleOwner {
public:
  PipeHandleOwner(HANDLE handle, bool disconnectBeforeClose)
      : handle_(handle), disconnectBeforeClose_(disconnectBeforeClose) {}

  ~PipeHandleOwner() { close(); }

  HANDLE handle() const { return handle_; }

  void close() {
    if (closed_) {
      return;
    }
    closed_ = true;
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
      return;
    }
    if (disconnectBeforeClose_) {
      FlushFileBuffers(handle_);
      DisconnectNamedPipe(handle_);
    }
    CloseHandle(handle_);
    handle_ = nullptr;
  }

private:
  HANDLE handle_ = nullptr;
  bool disconnectBeforeClose_ = false;
  bool closed_ = false;
};
#else
std::string makePosixError(const std::string &context) {
  return context + ": " + std::strerror(errno);
}

void setSocketTimeouts(int fd) {
  timeval timeout{};
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
#endif
} // namespace

SocketTransport::SocketTransport(DaemonConnectionInfo info) : info_(std::move(info)) {}

SocketTransport::~SocketTransport() { close(); }

const DaemonConnectionInfo &SocketTransport::connectionInfo() const { return info_; }
const std::string &SocketTransport::lastError() const { return lastError_; }

bool SocketTransport::listen() {
  if (running_) {
    return true;
  }
  lastError_.clear();
#if defined(_WIN32)
  const std::wstring endpoint = utf8ToWide(info_.endpoint);
  listenerHandle_ = CreateNamedPipeW(
      endpoint.c_str(), PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
  if (listenerHandle_ == INVALID_HANDLE_VALUE) {
    lastError_ = "CreateNamedPipeW failed";
    listenerHandle_ = nullptr;
    return false;
  }
#else
  std::error_code ec;
  const auto endpointPath = std::filesystem::path(info_.endpoint);
  if (endpointPath.has_parent_path()) {
    std::filesystem::create_directories(endpointPath.parent_path(), ec);
    if (ec) {
      lastError_ = "failed to create daemon socket directory: " + ec.message();
      return false;
    }
  }
  if (info_.endpoint.size() >= sizeof(sockaddr_un::sun_path)) {
    lastError_ = "daemon socket path is too long";
    return false;
  }

  listenerFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listenerFd_ < 0) {
    lastError_ = makePosixError("socket");
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", info_.endpoint.c_str());

  auto tryBind = [&]() -> bool {
    return ::bind(listenerFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
  };
  if (!tryBind()) {
    if (errno == EADDRINUSE) {
      const int probeFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (probeFd >= 0) {
        const bool live = ::connect(probeFd, reinterpret_cast<sockaddr *>(&addr),
                                    sizeof(addr)) == 0;
        ::close(probeFd);
        if (!live) {
          ::unlink(info_.endpoint.c_str());
          if (!tryBind()) {
            lastError_ = makePosixError("bind");
            ::close(listenerFd_);
            listenerFd_ = -1;
            return false;
          }
        } else {
          lastError_ = "daemon endpoint already in use by a live server";
          ::close(listenerFd_);
          listenerFd_ = -1;
          return false;
        }
      } else {
        lastError_ = makePosixError("probe socket");
        ::close(listenerFd_);
        listenerFd_ = -1;
        return false;
      }
    } else {
      lastError_ = makePosixError("bind");
      ::close(listenerFd_);
      listenerFd_ = -1;
      return false;
    }
  }
  if (::listen(listenerFd_, 16) != 0) {
    lastError_ = makePosixError("listen");
    ::close(listenerFd_);
    listenerFd_ = -1;
    return false;
  }
#endif
  running_ = true;
  return true;
}

void SocketTransport::close() {
  if (!running_) {
    return;
  }
  running_ = false;
#if defined(_WIN32)
  if (listenerHandle_) {
    CloseHandle(static_cast<HANDLE>(listenerHandle_));
    listenerHandle_ = nullptr;
  }
#else
  if (listenerFd_ >= 0) {
    ::shutdown(listenerFd_, SHUT_RDWR);
    ::close(listenerFd_);
    listenerFd_ = -1;
  }
  ::unlink(info_.endpoint.c_str());
#endif
}

bool SocketTransport::running() const { return running_; }

std::optional<SocketTransport::Channel> SocketTransport::accept() {
  if (!running_) {
    return std::nullopt;
  }
#if defined(_WIN32)
  HANDLE pipe = static_cast<HANDLE>(listenerHandle_);
  if (!ConnectNamedPipe(pipe, nullptr)) {
    const DWORD err = GetLastError();
    if (err != ERROR_PIPE_CONNECTED) {
      lastError_ = "ConnectNamedPipe failed";
      return std::nullopt;
    }
  }
  // The accepted pipe instance is distinct from the next listener instance.
  // The channel owns the accepted instance and must disconnect+close it exactly
  // once; listenerHandle_ is immediately replaced with a fresh listening pipe.
  auto acceptedPipe = std::make_shared<PipeHandleOwner>(pipe, true);
  Channel channel;
  channel.label = info_.endpoint;
  channel.reader = [acceptedPipe](std::chrono::milliseconds) -> std::string {
    char buffer[8192];
    DWORD read = 0;
    if (!ReadFile(acceptedPipe->handle(), buffer, sizeof(buffer), &read, nullptr) ||
        read == 0) {
      return {};
    }
    return std::string(buffer, static_cast<size_t>(read));
  };
  channel.writer = [acceptedPipe](const std::string &data) -> bool {
    DWORD written = 0;
    return WriteFile(acceptedPipe->handle(), data.data(),
                     static_cast<DWORD>(data.size()), &written, nullptr) &&
           written == data.size();
  };
  channel.wakeStop = [acceptedPipe]() { acceptedPipe->close(); };
  listenerHandle_ = CreateNamedPipeW(
      utf8ToWide(info_.endpoint).c_str(), PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
  return channel;
#else
  pollfd pfd{};
  pfd.fd = listenerFd_;
  pfd.events = POLLIN;
  const int pollResult = ::poll(&pfd, 1, 200);
  if (pollResult < 0) {
    if (errno == EINTR) {
      return std::nullopt;
    }
    lastError_ = makePosixError("accept poll");
    return std::nullopt;
  }
  if (pollResult == 0) {
    return std::nullopt;
  }
  if (!(pfd.revents & POLLIN)) {
    return std::nullopt;
  }
  const int fd = ::accept(listenerFd_, nullptr, nullptr);
  if (fd < 0) {
    if (errno != EINTR) {
      lastError_ = makePosixError("accept");
    }
    return std::nullopt;
  }
  setSocketTimeouts(fd);
  Channel channel;
  channel.label = info_.endpoint;
  channel.reader = firmius::core::JsonRpcTransport::makeFdReader(fd);
  channel.writer = firmius::core::JsonRpcTransport::makeFdWriter(fd);
  channel.wakeStop = [fd]() { ::shutdown(fd, SHUT_RDWR); ::close(fd); };
  return channel;
#endif
}

SocketTransport::Channel SocketTransport::connect(std::chrono::milliseconds timeout) const {
#if defined(_WIN32)
  const std::wstring endpoint = utf8ToWide(info_.endpoint);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    HANDLE pipe = CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      auto clientPipe = std::make_shared<PipeHandleOwner>(pipe, false);
      Channel channel;
      channel.label = info_.endpoint;
      channel.reader = [clientPipe](std::chrono::milliseconds) -> std::string {
        char buffer[8192];
        DWORD read = 0;
        if (!ReadFile(clientPipe->handle(), buffer, sizeof(buffer), &read, nullptr) ||
            read == 0) {
          return {};
        }
        return std::string(buffer, static_cast<size_t>(read));
      };
      channel.writer = [clientPipe](const std::string &data) -> bool {
        DWORD written = 0;
        return WriteFile(clientPipe->handle(), data.data(),
                         static_cast<DWORD>(data.size()), &written, nullptr) &&
               written == data.size();
      };
      channel.wakeStop = [clientPipe]() { clientPipe->close(); };
      return channel;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw std::runtime_error("timed out connecting to named pipe");
#else
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string lastConnectError = "daemon socket unavailable";
  while (std::chrono::steady_clock::now() < deadline) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      lastConnectError = makePosixError("socket");
      break;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", info_.endpoint.c_str());
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
      setSocketTimeouts(fd);
      Channel channel;
      channel.label = info_.endpoint;
      channel.reader = firmius::core::JsonRpcTransport::makeFdReader(fd);
      channel.writer = firmius::core::JsonRpcTransport::makeFdWriter(fd);
      channel.wakeStop = [fd]() { ::shutdown(fd, SHUT_RDWR); ::close(fd); };
      return channel;
    }
    lastConnectError = makePosixError("connect");
    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw std::runtime_error("timed out connecting to daemon socket at " + info_.endpoint +
                           " (" + lastConnectError + ")");
#endif
}

} // namespace firmius::daemon
