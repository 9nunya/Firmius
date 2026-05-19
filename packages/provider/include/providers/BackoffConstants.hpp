#ifndef FIRMIUS_PROVIDER_BACKOFFCONSTANTS_HPP
#define FIRMIUS_PROVIDER_BACKOFFCONSTANTS_HPP

#include <array>
#include <cstddef>

namespace firmius::shared {

/**
 * Unified exponential backoff sequence for all providers.
 * Used for rate limit backoff and retry attempts.
 * 
 * Sequence: 1s -> 2s -> 4s -> 8s -> 9s -> 16s -> 20s -> 25s -> 30s -> 45s -> 60s
 * 
 * Usage: BackoffConstants::SEQUENCE[retryAttempt] gives the backoff in seconds
 * for the given retry attempt (0-indexed). If retryAttempt >= SIZE, the last
 * value (60s) is used.
 */
struct BackoffConstants {
  static constexpr std::array<int, 11> SEQUENCE{
      1,   // Attempt 0: 1s
      2,   // Attempt 1: 2s
      4,   // Attempt 2: 4s
      8,   // Attempt 3: 8s
      9,   // Attempt 4: 9s
      16,  // Attempt 5: 16s
      20,  // Attempt 6: 20s
      25,  // Attempt 7: 25s
      30,  // Attempt 8: 30s
      45,  // Attempt 9: 45s
      60   // Attempt 10+: 60s (capped)
  };

  static constexpr size_t SIZE = SEQUENCE.size();
  static constexpr int MAX_BACKOFF = 60;

  /**
   * Get backoff seconds for a given retry attempt.
   * Caps to MAX_BACKOFF if attempt >= SIZE.
   */
  static inline int getBackoffSeconds(int attempt) {
    if (attempt < 0) {
      return SEQUENCE[0];
    }
    if (static_cast<size_t>(attempt) >= SIZE) {
      return SEQUENCE.back();
    }
    return SEQUENCE[static_cast<size_t>(attempt)];
  }
};

} // namespace firmius::shared

#endif // FIRMIUS_SHARED_BACKOFFCONSTANTS_HPP
