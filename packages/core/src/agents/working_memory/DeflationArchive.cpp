#include "agents/working_memory/DeflationArchive.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

namespace firmius::core::working_memory {

namespace fs = std::filesystem;

namespace {

std::string sanitizeId(const std::string& threadId) {
  std::string out;
  out.reserve(threadId.size());
  for (char c : threadId) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
      out.push_back(c);
    } else {
      out.push_back('_');
    }
    if (out.size() >= 16) {
      break;
    }
  }
  if (out.empty()) {
    out = "thread";
  }
  return out;
}

std::uint64_t nowMicros() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

} // namespace

DeflationArchive::DeflationArchive(std::string threadDirPath)
    : rootDir_(std::move(threadDirPath)) {
  if (!rootDir_.empty() && rootDir_.back() != '/') {
    rootDir_.push_back('/');
  }
  rootDir_ += "working_memory/archive";
}

void DeflationArchive::ensureRootExists() const {
  std::error_code ec;
  fs::create_directories(rootDir_, ec);
  // Errors are non-fatal; put()/get() will surface them via empty results.
}

std::string DeflationArchive::mintId(const std::string& threadId) {
  std::lock_guard lk(mu_);
  ++seq_;
  std::ostringstream out;
  out << "ar-" << sanitizeId(threadId) << '-' << nowMicros() << '-' << seq_;
  return out.str();
}

void DeflationArchive::put(const std::string& archiveId,
                           const std::string& body) {
  if (archiveId.empty()) {
    return;
  }
  ensureRootExists();
  const fs::path target = fs::path(rootDir_) / (archiveId + ".txt");
  const fs::path temp = fs::path(rootDir_) /
                        (archiveId + ".txt.tmp." + std::to_string(nowMicros()));
  {
    std::ofstream out(temp, std::ios::binary);
    if (!out) {
      return;
    }
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!out) {
      std::error_code ec;
      fs::remove(temp, ec);
      return;
    }
  }
  std::error_code ec;
  fs::rename(temp, target, ec);
  if (ec) {
    fs::remove(temp, ec);
  }
}

std::optional<std::string> DeflationArchive::get(
    const std::string& archiveId) const {
  if (archiveId.empty()) {
    return std::nullopt;
  }
  const fs::path target = fs::path(rootDir_) / (archiveId + ".txt");
  std::ifstream in(target, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  if (!in && !in.eof()) {
    return std::nullopt;
  }
  return buf.str();
}

bool DeflationArchive::has(const std::string& archiveId) const {
  if (archiveId.empty()) {
    return false;
  }
  std::error_code ec;
  return fs::exists(fs::path(rootDir_) / (archiveId + ".txt"), ec);
}

void DeflationArchive::remove(const std::string& archiveId) {
  if (archiveId.empty()) {
    return;
  }
  std::error_code ec;
  fs::remove(fs::path(rootDir_) / (archiveId + ".txt"), ec);
}

std::uint64_t DeflationArchive::totalBytesOnDisk() const {
  std::error_code ec;
  std::uint64_t total = 0;
  if (!fs::exists(rootDir_, ec)) {
    return 0;
  }
  for (const auto& entry : fs::directory_iterator(rootDir_, ec)) {
    if (ec) {
      break;
    }
    if (entry.is_regular_file(ec)) {
      total += entry.file_size(ec);
    }
  }
  return total;
}

} // namespace firmius::core::working_memory
