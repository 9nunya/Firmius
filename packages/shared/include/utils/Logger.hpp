#ifndef FIRMIUS_SHARED_LOGGER_HPP
#define FIRMIUS_SHARED_LOGGER_HPP

#include <fstream>
#include <memory>
#include <mutex>
#include <string>

namespace firmius::shared {

enum class LogLevel { Debug = 0, Info, Warning, Error };

class Logger {
public:
  static Logger &instance();

  void log(LogLevel level, const std::string &message);
  void logDebug(const std::string &message);
  void logInfo(const std::string &message);
  void logWarning(const std::string &message);
  void logError(const std::string &message);

  void enableFileLogging(const std::string &path);
  void disableFileLogging();
  void setMinimumLevel(LogLevel level);
  LogLevel minimumLevel() const;

private:
  Logger();
  ~Logger();

  Logger(const Logger &) = delete;
  Logger &operator=(const Logger &) = delete;

  static std::string levelName(LogLevel level);
  static std::string currentTimestamp();
  std::string formatMessage(LogLevel level,
                            const std::string &message) const;
  void writeToStreams(const std::string &line);

  mutable std::mutex mutex_;
  LogLevel minimumLevel_ = LogLevel::Debug;
  std::unique_ptr<std::ofstream> file_;
  bool fileEnabled_ = false;
  std::string filePath_;
};

}

#endif // FIRMIUS_SHARED_LOGGER_HPP
