#include "utils/Logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace firmius::shared {

Logger &Logger::instance() {
  static Logger inst;
  return inst;
}

Logger::Logger() = default;

Logger::~Logger() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (file_) {
    file_->flush();
  }
}

void Logger::log(LogLevel level, const std::string &message) {
  if (static_cast<int>(level) < static_cast<int>(minimumLevel_)) {
    return;
  }
  auto formatted = formatMessage(level, message);

  std::lock_guard<std::mutex> guard(mutex_);
  writeToStreams(formatted);
}

void Logger::logDebug(const std::string &message) { log(LogLevel::Debug, message); }

void Logger::logInfo(const std::string &message) { log(LogLevel::Info, message); }

void Logger::logWarning(const std::string &message) {
  log(LogLevel::Warning, message);
}

void Logger::logError(const std::string &message) { log(LogLevel::Error, message); }

void Logger::enableFileLogging(const std::string &path) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto next = std::make_unique<std::ofstream>(path, std::ios::app | std::ios::out);
  if (next->is_open()) {
    file_ = std::move(next);
    filePath_ = path;
    fileEnabled_ = true;
  } else {
    fileEnabled_ = false;
    file_.reset();
  }
}

void Logger::disableFileLogging() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (file_) {
    file_->close();
  }
  file_.reset();
  fileEnabled_ = false;
  filePath_.clear();
}

void Logger::setMinimumLevel(LogLevel level) {
  std::lock_guard<std::mutex> guard(mutex_);
  minimumLevel_ = level;
}

LogLevel Logger::minimumLevel() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return minimumLevel_;
}

std::string Logger::formatMessage(LogLevel level,
                                  const std::string &message) const {
  std::ostringstream oss;
  oss << "[" << currentTimestamp() << "] [" << levelName(level) << "] "
      << message;
  return oss.str();
}

void Logger::writeToStreams(const std::string &line) {
  try {
    std::cerr << line;
    if (line.empty() || line.back() != '\n') {
      std::cerr << '\n';
    }
    std::cerr.flush();
  } catch (...) {
  }

  if (fileEnabled_ && file_) {
    try {
      *file_ << line;
      if (line.empty() || line.back() != '\n') {
        *file_ << '\n';
      }
      file_->flush();
      if (!file_->good()) {
        fileEnabled_ = false;
        file_.reset();
      }
    } catch (...) {
      fileEnabled_ = false;
      file_.reset();
    }
  }
}

std::string Logger::levelName(LogLevel level) {
  switch (level) {
  case LogLevel::Debug:
    return "DEBUG";
  case LogLevel::Info:
    return "INFO";
  case LogLevel::Warning:
    return "WARN";
  case LogLevel::Error:
    return "ERROR";
  }
  return "UNKNOWN";
}

std::string Logger::currentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch());
  std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm;
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.'
      << std::setfill('0') << std::setw(3) << (ms.count() % 1000);
  return oss.str();
}

}
