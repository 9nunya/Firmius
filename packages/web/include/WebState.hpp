#ifndef FIRMIUS_WEB_STATE_HPP
#define FIRMIUS_WEB_STATE_HPP

#include "Events.hpp"

#include <json/json.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace firmius::web {

class WebState {
public:
  struct LoggedEvent {
    std::uint64_t id = 0;
    std::string threadId;
    Json::Value payload;
  };

  static WebState &instance();

  WebState(const WebState &) = delete;
  WebState &operator=(const WebState &) = delete;

  void init();
  void shutdown();

  Json::Value buildStateSnapshot();
  Json::Value buildThreadsSnapshot();
  Json::Value buildFocusedHistorySnapshot();
  Json::Value buildThemesSnapshot();
  Json::Value buildConfigSnapshot();
  Json::Value buildProvidersSnapshot();
  Json::Value buildWorkflowsSnapshot();
  Json::Value buildWorkSnapshot();

  std::vector<LoggedEvent> waitForEventsAfter(std::uint64_t afterId,
                                              const std::string &threadId,
                                              int timeoutMs);
  std::uint64_t latestEventId() const;

private:
  WebState() = default;
  ~WebState() = default;

  void recordEvent(const firmius::shared::AppEvent &event);

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool initialized_ = false;
  int subscription_id_ = -1;
  std::uint64_t next_event_id_ = 1;
  std::vector<LoggedEvent> events_;
};

} // namespace firmius::web

#endif
