#ifndef FIRMIUS_TUI_UTILS_UI_TASK_SCHEDULER_HPP
#define FIRMIUS_TUI_UTILS_UI_TASK_SCHEDULER_HPP

#include "utils/BackgroundTaskPool.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace firmius::tui {

class UiTaskScheduler {
public:
  enum class Priority {
    Interactive,
    VisiblePrefetch,
    Background,
  };

  struct Telemetry {
    std::string key;
    std::uint64_t generation = 0;
    bool running = false;
    bool cancelled = false;
    double queued_ms = 0.0;
    double run_ms = 0.0;
    double apply_ms = 0.0;
  };

  using UiDispatcher = std::function<void(std::function<void()>)>;
  using ApplyCallback = std::function<void()>;
  using WorkCallback = std::function<ApplyCallback(
      const std::shared_ptr<std::atomic<bool>> &cancelled,
      std::uint64_t generation)>;

  UiTaskScheduler(BackgroundTaskPool &pool, UiDispatcher dispatcher)
      : pool_(pool), dispatcher_(std::move(dispatcher)) {}

  void schedule(const std::string &key, Priority priority, WorkCallback work) {
    if (key.empty() || !work) {
      return;
    }

    auto state = std::make_shared<TaskState>();
    state->key = key;
    state->generation = 0;
    state->cancelled = std::make_shared<std::atomic<bool>>(false);
    state->queued_at = std::chrono::steady_clock::now();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto &slot = tasks_[key];
      if (slot.cancelled) {
        slot.cancelled->store(true, std::memory_order_relaxed);
      }
      state->generation = slot.generation + 1;
      slot = *state;
      slot.running = true;
      slot.cancelled_flag = false;
    }

    pool_.post(
        [this, state, work = std::move(work)]() mutable {
          const auto started_at = std::chrono::steady_clock::now();
          ApplyCallback apply;
          try {
            if (!state->cancelled->load(std::memory_order_relaxed)) {
              apply = work(state->cancelled, state->generation);
            }
          } catch (...) {
            markFinished(state->key, state->generation, true,
                         millisSince(state->queued_at, started_at),
                         millisSince(started_at, std::chrono::steady_clock::now()),
                         0.0);
            return;
          }

          const double queued_ms = millisSince(state->queued_at, started_at);
          const auto finished_work_at = std::chrono::steady_clock::now();
          const double run_ms = millisSince(started_at, finished_work_at);
          if (!apply || state->cancelled->load(std::memory_order_relaxed)) {
            markFinished(state->key, state->generation, true, queued_ms, run_ms,
                         0.0);
            return;
          }

          dispatcher_([this, state, apply = std::move(apply), queued_ms,
                       run_ms]() mutable {
            const auto apply_started_at = std::chrono::steady_clock::now();
            bool cancelled = state->cancelled->load(std::memory_order_relaxed);
            {
              std::lock_guard<std::mutex> lock(mutex_);
              auto it = tasks_.find(state->key);
              cancelled = cancelled || it == tasks_.end() ||
                          it->second.generation != state->generation;
            }
            if (!cancelled) {
              apply();
            }
            const double apply_ms =
                millisSince(apply_started_at, std::chrono::steady_clock::now());
            markFinished(state->key, state->generation, cancelled, queued_ms,
                         run_ms, apply_ms);
          });
        },
        toPoolPriority(priority));
  }

  void cancel(const std::string &key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(key);
    if (it == tasks_.end()) {
      return;
    }
    if (it->second.cancelled) {
      it->second.cancelled->store(true, std::memory_order_relaxed);
    }
    it->second.running = false;
    it->second.cancelled_flag = true;
  }

  void cancelPrefix(const std::string &prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &[key, state] : tasks_) {
      if (key.rfind(prefix, 0) != 0) {
        continue;
      }
      if (state.cancelled) {
        state.cancelled->store(true, std::memory_order_relaxed);
      }
      state.running = false;
      state.cancelled_flag = true;
    }
  }

  std::unordered_map<std::string, Telemetry> telemetrySnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, Telemetry> out;
    out.reserve(tasks_.size());
    for (const auto &[key, state] : tasks_) {
      out.emplace(key, Telemetry{key, state.generation, state.running,
                                 state.cancelled_flag, state.queued_ms,
                                 state.run_ms, state.apply_ms});
    }
    return out;
  }

private:
  struct TaskState {
    std::string key;
    std::uint64_t generation = 0;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::chrono::steady_clock::time_point queued_at{};
    bool running = false;
    bool cancelled_flag = false;
    double queued_ms = 0.0;
    double run_ms = 0.0;
    double apply_ms = 0.0;
  };

  static double millisSince(std::chrono::steady_clock::time_point from,
                            std::chrono::steady_clock::time_point to) {
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::microseconds>(to - from)
                   .count()) /
           1000.0;
  }

  static BackgroundTaskPool::Priority toPoolPriority(Priority priority) {
    switch (priority) {
    case Priority::Interactive:
      return BackgroundTaskPool::Priority::Interactive;
    case Priority::VisiblePrefetch:
      return BackgroundTaskPool::Priority::VisiblePrefetch;
    case Priority::Background:
      return BackgroundTaskPool::Priority::Background;
    }
    return BackgroundTaskPool::Priority::Background;
  }

  void markFinished(const std::string &key, std::uint64_t generation,
                    bool cancelled, double queued_ms, double run_ms,
                    double apply_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(key);
    if (it == tasks_.end() || it->second.generation != generation) {
      return;
    }
    it->second.running = false;
    it->second.cancelled_flag = cancelled;
    it->second.queued_ms = queued_ms;
    it->second.run_ms = run_ms;
    it->second.apply_ms = apply_ms;
  }

  BackgroundTaskPool &pool_;
  UiDispatcher dispatcher_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, TaskState> tasks_;
};

} // namespace firmius::tui

#endif
