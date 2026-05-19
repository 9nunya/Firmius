#ifndef FIRMIUS_SHARED_INTERRUPTIBLESLEEP_HPP
#define FIRMIUS_SHARED_INTERRUPTIBLESLEEP_HPP

#include "utils/AbortController.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace firmius::shared {

/**
 * @brief Sleep that can be interrupted by an abort signal.
 * 
 * Breaks the sleep into small intervals and checks the abort signal
 * between each interval. Returns early if abort is signaled.
 * 
 * @param totalDuration The total duration to sleep
 * @param abortSignal Pointer to atomic bool that signals cancellation
 * @param intervalMs Interval in milliseconds to check abort signal (default: 50ms)
 * @return true if sleep completed, false if interrupted
 */
inline bool interruptibleSleep(
    std::chrono::milliseconds totalDuration,
    const std::atomic<bool> *abortSignal,
    std::chrono::milliseconds intervalMs = std::chrono::milliseconds(50)) {
  
  auto elapsed = std::chrono::milliseconds(0);
  
  while (elapsed < totalDuration) {
    // Check abort signal before each interval
    if (abortSignal && abortSignal->load()) {
      return false; // Interrupted
    }
    
    // Calculate remaining time
    auto remaining = totalDuration - elapsed;
    auto sleepTime = (remaining < intervalMs) ? remaining : intervalMs;
    
    // Sleep for the interval
    std::this_thread::sleep_for(sleepTime);
    elapsed += sleepTime;
  }
  
  // Final check after sleep completes
  return !(abortSignal && abortSignal->load());
}

inline bool interruptibleSleep(
    std::chrono::milliseconds totalDuration,
    const std::shared_ptr<AbortController> &abortController,
    const std::atomic<bool> *abortSignal = nullptr,
    std::chrono::milliseconds intervalMs = std::chrono::milliseconds(50)) {
  if (!abortController) {
    return interruptibleSleep(totalDuration, abortSignal, intervalMs);
  }

  if (abortController->isCancelled() ||
      (abortSignal && abortSignal->load())) {
    return false;
  }

  const bool cancelled = abortController->waitForCancelFor(totalDuration);
  return !cancelled && !(abortSignal && abortSignal->load());
}

/**
 * @brief Convenience overload for seconds-based duration
 */
inline bool interruptibleSleep(
    std::chrono::seconds totalDuration,
    const std::atomic<bool> *abortSignal) {
  return interruptibleSleep(
      std::chrono::duration_cast<std::chrono::milliseconds>(totalDuration),
      abortSignal);
}

inline bool interruptibleSleep(
    std::chrono::seconds totalDuration,
    const std::shared_ptr<AbortController> &abortController,
    const std::atomic<bool> *abortSignal = nullptr) {
  return interruptibleSleep(
      std::chrono::duration_cast<std::chrono::milliseconds>(totalDuration),
      abortController, abortSignal);
}

} // namespace firmius::shared

#endif // FIRMIUS_SHARED_INTERRUPTIBLESLEEP_HPP
