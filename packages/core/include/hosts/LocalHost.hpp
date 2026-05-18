#ifndef FIRMIUS_CORE_LOCAL_HOST_HPP
#define FIRMIUS_CORE_LOCAL_HOST_HPP

#include "IHost.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Handle for a process running on the local machine.
 * Provides real-time output capture and state inspection.
 */
class LocalHostProcess : public shared::IHostProcess {
public:
  struct Impl;

  explicit LocalHostProcess(std::unique_ptr<Impl> impl);
  ~LocalHostProcess() override;

  void onOutput(std::function<void(const std::string &, bool isError)> callback) override;
  shared::ProcessResult wait() override;
  shared::ProcessSnapshot inspect() const override;
  void kill() override;
  void write(const std::string &data) override;
  bool isRunning() override;
  std::string getSystemId() const override;

private:
  void captureLoop(bool isError);
  void joinCaptureThreads();
  bool reapIfNeeded(bool block);
  static int decodeExitStatus(int status);

  std::unique_ptr<Impl> impl;
  std::function<void(const std::string &, bool isError)> callback;
  mutable std::mutex callbackMutex;
  mutable std::string stdoutBuffer;
  mutable std::string stderrBuffer;
  std::thread stdoutThread;
  std::thread stderrThread;
  std::atomic<bool> finished{false};
  std::mutex stateMutex;
  bool reaped = false;
  std::atomic<int> exitCode{-1};
  std::chrono::steady_clock::time_point startTime;
};

/**
 * @brief Host implementation for local execution.
 * Uses platform-native process execution on the local machine.
 */
class LocalHost : public shared::IHost {
public:
  std::string init() override;
  void destroy() override;
  void cleanup() override;
  void setUser(const std::string &user) override;
  std::string getId() const override { return "localhost"; }

  std::vector<uint8_t> readFile(const std::string &path) override;
  void writeFile(const std::string &path,
                 const std::vector<uint8_t> &data) override;
  void deleteFile(const std::string &path) override;
  bool exists(const std::string &path) override;
  std::vector<shared::FileInfo> listDir(const std::string &path) override;
  shared::FileInfo stat(const std::string &path) override;

  shared::ProcessResult
  exec(const std::string &command, const std::string &cwd = "",
       const std::map<std::string, std::string> &env = {},
       std::optional<std::chrono::milliseconds> timeout = std::nullopt) override;
  std::unique_ptr<shared::IHostProcess>
  spawn(const std::string &command, const std::string &cwd = "",
        const std::map<std::string, std::string> &env = {}) override;

  void registerBackgroundProcess(const std::string &id,
                                 std::unique_ptr<shared::IHostProcess> proc) override;
  shared::ProcessSnapshot inspectBackgroundProcess(const std::string &id) override;
  void releaseBackgroundProcess(const std::string &id) override;
  void writeToBackgroundProcess(const std::string &id,
                                const std::string &data) override;
  void killBackgroundProcess(const std::string &id) override;

private:
  struct CompletedProcessSnapshot {
    shared::ProcessSnapshot snapshot;
    std::chrono::steady_clock::time_point completedAt;
  };

  static constexpr size_t kMaxCompletedBackgroundProcesses = 64;

  void promoteCompletedProcessLocked(const std::string &id,
                                     std::unique_ptr<shared::IHostProcess> proc,
                                     const shared::ProcessSnapshot &snapshot);
  std::map<std::string, CompletedProcessSnapshot>::iterator
  touchCompletedProcessLocked(const std::string &id);

  std::string currentUser;
  std::map<std::string, std::unique_ptr<shared::IHostProcess>> backgroundProcesses;
  std::map<std::string, CompletedProcessSnapshot> completedBackgroundProcesses;
  mutable std::mutex bgMutex;
};

} // namespace firmius::core

#endif