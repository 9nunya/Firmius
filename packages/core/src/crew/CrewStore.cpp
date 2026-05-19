#include "crew/CrewStore.hpp"

#include "utils/PlatformPaths.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace firmius::core::crew {

namespace {

constexpr std::uint64_t kSchemaVersion = 1;

std::string toString(const rapidjson::Document &doc) {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
  doc.Accept(writer);
  return std::string(sb.GetString(), sb.GetSize());
}

[[maybe_unused]] bool atomicWriteFile(const fs::path &target,
                                      const std::string &payload) {
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (ec) return false;
  const fs::path tmp = target.string() + ".tmp";
#if defined(__unix__) || defined(__APPLE__)
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  const char *data = payload.data();
  std::size_t remaining = payload.size();
  while (remaining > 0) {
    const ssize_t n = ::write(fd, data, remaining);
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      ::unlink(tmp.c_str());
      return false;
    }
    data += n;
    remaining -= static_cast<std::size_t>(n);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    ::unlink(tmp.c_str());
    return false;
  }
  ::close(fd);
  if (::rename(tmp.c_str(), target.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  const int dfd = ::open(target.parent_path().c_str(), O_RDONLY);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
  return true;
#else
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (!out) return false;
  }
  fs::rename(tmp, target, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
#endif
}

[[maybe_unused]] bool appendLine(const fs::path &target,
                                 const std::string &line) {
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (ec) return false;
  std::ofstream out(target, std::ios::binary | std::ios::app);
  if (!out) return false;
  out.write(line.data(), static_cast<std::streamsize>(line.size()));
  if (line.empty() || line.back() != '\n') {
    out.put('\n');
  }
  return out.good();
}

std::optional<std::string> readEntireFile(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

} // namespace

std::uint64_t nowEpochMs() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::string mintShortId(const std::string &prefix) {
  // 16-bit random + 16-bit timestamp-tail → ~4 hex chars, low collision rate
  // for the per-crew/per-thread cardinality we expect.
  static thread_local std::mt19937_64 rng{
      static_cast<std::uint64_t>(nowEpochMs()) ^
      std::random_device{}()};
  const std::uint64_t r = rng();
  const std::uint64_t tail = nowEpochMs() & 0xFFFFu;
  std::ostringstream out;
  out << prefix << '-';
  out << std::hex << ((r ^ tail) & 0xFFFFu);
  return out.str();
}

CrewStore::CrewStore(std::string threadId, fs::path threadDirectory)
    : threadId_(std::move(threadId)) {
  fs::path base = threadDirectory;
  if (base.empty()) {
    base = firmius::shared::PlatformPaths::firmiusHomeDir() / "threads" /
           threadId_;
  }
  crewsRoot_ = base / "crews";
}

std::mutex &CrewStore::lockFor(const std::string &crewId) const {
  std::lock_guard<std::mutex> g(registryMutex_);
  auto it = crewMutexes_.find(crewId);
  if (it == crewMutexes_.end()) {
    it = crewMutexes_.emplace(crewId, std::make_unique<std::mutex>()).first;
  }
  return *it->second;
}

fs::path CrewStore::crewDir(const std::string &crewId) const {
  return crewsRoot_ / crewId;
}

fs::path CrewStore::manifestPath(const std::string &crewId) const {
  return crewDir(crewId) / "manifest.json";
}

fs::path CrewStore::membersPath(const std::string &crewId) const {
  return crewDir(crewId) / "members.json";
}

fs::path CrewStore::tasksPath(const std::string &crewId) const {
  return crewDir(crewId) / "tasks.json";
}

fs::path CrewStore::channelsPath(const std::string &crewId) const {
  return crewDir(crewId) / "channels.json";
}

fs::path CrewStore::mailPath(const std::string &crewId) const {
  return crewDir(crewId) / "mail.jsonl";
}

fs::path CrewStore::eventsPath(const std::string &crewId) const {
  return crewDir(crewId) / "events.jsonl";
}

fs::path CrewStore::flagsPath(const std::string &crewId) const {
  return crewDir(crewId) / "flags.jsonl";
}

std::vector<std::string> CrewStore::listCrewIds() const {
  std::vector<std::string> out;
  std::error_code ec;
  if (!fs::exists(crewsRoot_, ec)) return out;
  for (const auto &entry : fs::directory_iterator(crewsRoot_, ec)) {
    if (ec) break;
    if (entry.is_directory(ec)) {
      out.push_back(entry.path().filename().string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace firmius::core::crew
