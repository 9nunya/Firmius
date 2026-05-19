#ifndef FIRMIUS_SHARED_ABORTCONTROLLER_HPP
#define FIRMIUS_SHARED_ABORTCONTROLLER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace firmius::shared {

class AbortController {
public:
  using CallbackId = std::uint64_t;

  std::atomic<bool> *signal() { return &cancelled_; }
  const std::atomic<bool> *signal() const { return &cancelled_; }

  bool isCancelled() const { return cancelled_.load(); }

  void cancel() {
    std::unordered_map<CallbackId, std::function<void()>> callbacks;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (cancelled_.exchange(true)) {
        return;
      }
      callbacks.swap(callbacks_);
    }

    cv_.notify_all();
    for (const auto &[_, callback] : callbacks) {
      if (callback) {
        callback();
      }
    }
  }

  CallbackId subscribe(std::function<void()> callback) {
    if (!callback) {
      return 0;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!cancelled_.load()) {
        const CallbackId id = nextCallbackId_++;
        callbacks_.emplace(id, std::move(callback));
        return id;
      }
    }

    callback();
    return 0;
  }

  void unsubscribe(CallbackId id) {
    if (id == 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.erase(id);
  }

  bool waitForCancelFor(std::chrono::milliseconds timeout) const {
    if (cancelled_.load()) {
      return true;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [this]() { return cancelled_.load(); });
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::atomic<bool> cancelled_{false};
  CallbackId nextCallbackId_ = 1;
  std::unordered_map<CallbackId, std::function<void()>> callbacks_;
};

} // namespace firmius::shared

#endif // FIRMIUS_SHARED_ABORTCONTROLLER_HPP
