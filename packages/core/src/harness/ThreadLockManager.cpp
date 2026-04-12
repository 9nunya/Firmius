#include "harness/ThreadLockManager.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const std::string FIRMIUS_DIR = ".firmius";
const std::string LOCK_FILE = ".lock";

bool ensureWritableDirectory(const std::filesystem::path &dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec || !std::filesystem::exists(dir) ||
      !std::filesystem::is_directory(dir)) {
    return false;
  }

  const auto probe = dir / ".write_probe";
  std::ofstream out(probe);
  if (!out.is_open()) {
    return false;
  }
  out << "ok";
  out.close();
  std::filesystem::remove(probe, ec);
  return true;
}

std::string getFirmiusHome() {
  const char *home = std::getenv("HOME");
  if (home) {
    const std::filesystem::path userHome =
        std::filesystem::path(home) / FIRMIUS_DIR;
    if (ensureWritableDirectory(userHome)) {
      return userHome.string();
    }
  }

  const std::filesystem::path localHome =
      std::filesystem::current_path() / FIRMIUS_DIR;
  if (ensureWritableDirectory(localHome)) {
    return localHome.string();
  }

  const std::filesystem::path tempHome =
      std::filesystem::temp_directory_path() / "firmius";
  ensureWritableDirectory(tempHome);
  return tempHome.string();
}

std::string getThreadDir(const std::string &threadId) {
  return getFirmiusHome() + "/threads/" + threadId;
}

std::string getLockFilePath(const std::string &threadId) {
  return getThreadDir(threadId) + "/" + LOCK_FILE;
}

} // namespace

namespace firmius::core {

int ThreadLockManager::acquire(const std::string &threadId) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string dir = getThreadDir(threadId);
  std::filesystem::create_directories(dir);
  std::string path = getLockFilePath(threadId);

  int fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0)
    return -1;

  if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
    if (errno == EWOULDBLOCK) {
      close(fd);
      return -2;
    }
    close(fd);
    return -1;
  }

  ftruncate(fd, 0);
  std::string pid = std::to_string(getpid());
  ::write(fd, pid.c_str(), pid.size());
  fsync(fd);

  locks_[threadId] = fd;
  return fd;
}

void ThreadLockManager::release(const std::string &threadId) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = locks_.find(threadId);
  if (it != locks_.end()) {
    flock(it->second, LOCK_UN);
    close(it->second);
    locks_.erase(it);
  }
}

void ThreadLockManager::releaseAll() {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto &[threadId, fd] : locks_) {
    if (fd >= 0) {
      flock(fd, LOCK_UN);
      close(fd);
    }
  }
  locks_.clear();
}

bool ThreadLockManager::isLocked(const std::string &threadId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return locks_.count(threadId) > 0;
}

int ThreadLockManager::getOwnerPid(const std::string &threadId) const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string lockPath = getLockFilePath(threadId);
  std::ifstream lf(lockPath);
  if (!lf.is_open())
    return -1;

  std::string pidStr;
  lf >> pidStr;
  try {
    return std::stoi(pidStr);
  } catch (...) {
    return -1;
  }
}

size_t ThreadLockManager::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return locks_.size();
}

} // namespace firmius::core
