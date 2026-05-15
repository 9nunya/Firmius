#ifndef FIRMIUS_TUI_UTILS_BACKGROUND_TASK_POOL_HPP
#define FIRMIUS_TUI_UTILS_BACKGROUND_TASK_POOL_HPP

#include <condition_variable>
#include <array>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace firmius::tui {

/// Tiny shared worker pool for non-UI background work scheduled from
/// TuiState. Tasks run on dedicated worker threads; UI mutations should
/// be marshaled back via TuiState::deferUiMutation().
class BackgroundTaskPool {
public:
  enum class Priority : std::size_t {
    Interactive = 0,
    VisiblePrefetch = 1,
    Background = 2,
    Count = 3,
  };

  explicit BackgroundTaskPool(std::size_t threads = 4) {
    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
      workers_.emplace_back([this](std::stop_token st) { workerLoop(st); });
    }
  }

  ~BackgroundTaskPool() { stop(); }

  BackgroundTaskPool(const BackgroundTaskPool &) = delete;
  BackgroundTaskPool &operator=(const BackgroundTaskPool &) = delete;

  void post(std::function<void()> task,
            Priority priority = Priority::Background) {
    if (!task) {
      return;
    }
    {
      std::lock_guard<std::mutex> lk(mtx_);
      if (stopped_) {
        return;
      }
      queues_[static_cast<std::size_t>(priority)].push_back(std::move(task));
    }
    cv_.notify_one();
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      stopped_ = true;
    }
    cv_.notify_all();
    for (auto &w : workers_) {
      w.request_stop();
    }
    cv_.notify_all();
    workers_.clear();
  }

private:
  void workerLoop(std::stop_token st) {
    while (!st.stop_requested()) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [&] {
          return stopped_ || st.stop_requested() || hasQueuedTasks();
        });
        if ((stopped_ || st.stop_requested()) && !hasQueuedTasks()) {
          return;
        }
        for (auto &queue : queues_) {
          if (queue.empty()) {
            continue;
          }
          task = std::move(queue.front());
          queue.pop_front();
          break;
        }
      }
      if (task) {
        try {
          task();
        } catch (...) {
          // swallow; UI must not crash on background failures
        }
      }
    }
  }

  std::vector<std::jthread> workers_;
  std::mutex mtx_;
  std::condition_variable cv_;
  std::array<std::deque<std::function<void()>>, 3> queues_;
  bool stopped_ = false;

  bool hasQueuedTasks() const {
    for (const auto &queue : queues_) {
      if (!queue.empty()) {
        return true;
      }
    }
    return false;
  }
};

} // namespace firmius::tui

#endif
