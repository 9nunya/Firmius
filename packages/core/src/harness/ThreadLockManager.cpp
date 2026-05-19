#include "harness/ThreadLockManager.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>

#if defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

const char *getHomeEnv() {
  const char *home = std::getenv("HOME");
#if defined(_WIN32)
  if (!home) {
    home = std::getenv("USERPROFILE");
  }
#endif
  return home;
}

std::string getFirmiusHome() {
  const char *home = getHomeEnv();
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

int openLockFile(const std::string &path) {
#if defined(_WIN32)
  // _S_IREAD/_S_IWRITE are MSVC-style flags; fall back to numeric values
  // for MinGW which doesn't always define them.
  #ifndef _S_IREAD
  #define _S_IREAD 0x0100
  #endif
  #ifndef _S_IWRITE
  #define _S_IWRITE 0x0080
  #endif
  return _open(path.c_str(), _O_RDWR | _O_CREAT, _S_IREAD | _S_IWRITE);
#else
  return open(path.c_str(), O_RDWR | O_CREAT, 0644);
#endif
}

void closeLockFile(int fd) {
#if defined(_WIN32)
  _close(fd);
#else
  close(fd);
#endif
}

bool tryAcquireExclusiveLock(int fd, bool &wouldBlock) {
  wouldBlock = false;

#if defined(_WIN32)
  HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }

  OVERLAPPED overlapped{};
  if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                 MAXDWORD, MAXDWORD, &overlapped)) {
    return true;
  }

  const DWORD err = GetLastError();
  if (err == ERROR_LOCK_VIOLATION) {
    wouldBlock = true;
  }
  return false;
#else
  if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
    return true;
  }

  if (errno == EWOULDBLOCK) {
    wouldBlock = true;
  }
  return false;
#endif
}

void releaseExclusiveLock(int fd) {
#if defined(_WIN32)
  HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE) {
    return;
  }

  OVERLAPPED overlapped{};
  UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
#else
  flock(fd, LOCK_UN);
#endif
}

bool truncateLockFile(int fd) {
#if defined(_WIN32)
  return _chsize_s(fd, 0) == 0;
#else
  return ftruncate(fd, 0) == 0;
#endif
}

int getCurrentPid() {
#if defined(_WIN32)
  return static_cast<int>(_getpid());
#else
  return static_cast<int>(getpid());
#endif
}

bool writeAll(int fd, const std::string &data) {
#if defined(_WIN32)
  if (_lseek(fd, 0, SEEK_SET) < 0) {
    return false;
  }

  const int written =
      _write(fd, data.c_str(), static_cast<unsigned int>(data.size()));
  return written == static_cast<int>(data.size());
#else
  if (lseek(fd, 0, SEEK_SET) < 0) {
    return false;
  }

  const ssize_t written = ::write(fd, data.c_str(), data.size());
  return written == static_cast<ssize_t>(data.size());
#endif
}

void syncLockFile(int fd) {
#if defined(_WIN32)
  _commit(fd);
#else
  fsync(fd);
#endif
}

int parsePidString(const std::string &pidStr) {
  try {
    size_t consumed = 0;
    const long long parsed = std::stoll(pidStr, &consumed);
    if (consumed != pidStr.size() || parsed <= 0 ||
        parsed > std::numeric_limits<int>::max()) {
      return -1;
    }
    return static_cast<int>(parsed);
  } catch (...) {
    return -1;
  }
}

int readOwnerPidAtPath(const std::string &lockPath) {
  std::ifstream lf(lockPath);
  if (!lf.is_open()) {
    return -1;
  }

  std::string pidStr;
  lf >> pidStr;
  return parsePidString(pidStr);
}

bool isPidAlive(int pidValue) {
  if (pidValue <= 0) {
    return false;
  }
#if defined(_WIN32)
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pidValue));
  if (process == nullptr) {
    return false;
  }
  const DWORD waitResult = WaitForSingleObject(process, 0);
  CloseHandle(process);
  return waitResult == WAIT_TIMEOUT;
#else
  return kill(static_cast<pid_t>(pidValue), 0) == 0 || errno == EPERM;
#endif
}

bool removeStaleLockFile(const std::string &lockPath) {
  std::error_code ec;
  return std::filesystem::remove(lockPath, ec) && !ec;
}

} // namespace

namespace firmius::core {

int ThreadLockManager::acquire(const std::string &threadId) {
  std::lock_guard<std::mutex> lock(mutex_);

  const std::string dir = getThreadDir(threadId);
  std::filesystem::create_directories(dir);
  std::string path = getLockFilePath(threadId);

  int fd = openLockFile(path);
  if (fd < 0) {
    return -1;
  }

  bool wouldBlock = false;
  if (!tryAcquireExclusiveLock(fd, wouldBlock)) {
    closeLockFile(fd);
    if (!wouldBlock) {
      return -1;
    }

    const int ownerPid = readOwnerPidAtPath(path);
    if (ownerPid > 0 && !isPidAlive(ownerPid) && removeStaleLockFile(path)) {
      fd = openLockFile(path);
      if (fd < 0) {
        return -1;
      }
      wouldBlock = false;
      if (!tryAcquireExclusiveLock(fd, wouldBlock)) {
        closeLockFile(fd);
        return wouldBlock ? -2 : -1;
      }
    } else {
      return -2;
    }
  }

  if (!truncateLockFile(fd)) {
    releaseExclusiveLock(fd);
    closeLockFile(fd);
    return -1;
  }

  const std::string pid = std::to_string(getCurrentPid());
  if (!writeAll(fd, pid)) {
    releaseExclusiveLock(fd);
    closeLockFile(fd);
    return -1;
  }

  syncLockFile(fd);
  locks_[threadId] = fd;
  return fd;
}

void ThreadLockManager::release(const std::string &threadId) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = locks_.find(threadId);
  if (it != locks_.end()) {
    releaseExclusiveLock(it->second);
    closeLockFile(it->second);
    locks_.erase(it);
  }
}

void ThreadLockManager::releaseAll() {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto &[threadId, fd] : locks_) {
    (void)threadId;
    if (fd >= 0) {
      releaseExclusiveLock(fd);
      closeLockFile(fd);
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
  return readOwnerPidAtPath(getLockFilePath(threadId));
}

size_t ThreadLockManager::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return locks_.size();
}

} // namespace firmius::core
