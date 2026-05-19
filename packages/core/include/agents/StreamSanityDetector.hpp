#ifndef FIRMIUS_CORE_STREAMSANITYDETECTOR_HPP
#define FIRMIUS_CORE_STREAMSANITYDETECTOR_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <deque>

namespace firmius::core {

/**
 * @brief Result of an insanity check.
 */
struct InsanityCheckResult {
  bool isInsane = false;       ///< Whether the stream appears to be insane
  std::string reason;          ///< Human-readable reason for detection
  enum class Type {
    None,
    RepetitiveLoop,            ///< Same pattern repeated many times
    ExcessiveTokenCount,       ///< Too many tokens without termination
    HighEntropyJunk,           ///< Random/gibberish character distribution
  } type = Type::None;

  InsanityCheckResult() = default;
  InsanityCheckResult(bool insane, std::string r, Type t)
      : isInsane(insane), reason(std::move(r)), type(t) {}
};

/**
 * @brief Detects when an LLM stream goes "insane" — repeating token sequences
 * endlessly or streaming random/gibberish content forever.
 *
 * The detector works incrementally, analyzing text deltas as they arrive.
 * It's designed to be lightweight (O(n) per delta) and configurable.
 */
class StreamSanityDetector {
public:
  struct Config {
    // Repetition detection: same pattern repeated consecutively
    int minPatternLength;          ///< Minimum pattern length to check (bytes)
    int maxPatternLength;          ///< Maximum pattern length to check
    int minConsecutiveRepeats;     ///< Minimum consecutive repeats to flag

    // Token count threshold
    std::uint64_t maxTokenThreshold;  ///< Max tokens before flagging

    // High-entropy/gibberish detection
    bool entropyDetectionEnabled;
    int entropyWindowSize;         ///< Window for entropy calculation
    double entropyThreshold;       ///< Shannon entropy threshold
    int gibberishNonWordRatio;     ///< % non-alphanumeric chars to flag

    // Global enable/disable
    bool enabled;

    Config()
        : minPatternLength(8), maxPatternLength(256),
          minConsecutiveRepeats(3), maxTokenThreshold(50000),
          entropyDetectionEnabled(true), entropyWindowSize(1000),
          entropyThreshold(4.5), gibberishNonWordRatio(35), enabled(true) {}
  };

  /**
   * @brief Construct a detector with given configuration.
   */
  explicit StreamSanityDetector(const Config &cfg = Config());

  /**
   * @brief Add a text delta to the detector's analysis.
   * @param delta New text chunk from the stream.
   */
  void addDelta(const std::string &delta);

  /**
   * @brief Check if the stream has gone insane.
   * @return InsanityCheckResult with detection status and reason.
   *
   * This should be called periodically during streaming (e.g., every N tokens
   * or after each delta). It analyzes accumulated data and returns true if
   * insanity patterns are detected.
   */
  InsanityCheckResult check() const;

  /**
   * @brief Reset the detector state (clear all buffers).
   * Call this between turns to start fresh.
   */
  void clear();

  /**
   * @brief Get the configuration.
   */
  const Config &getConfig() const { return config_; }

  /**
   * @brief Get total tokens accumulated.
   */
  std::uint64_t getTotalTokens() const { return totalTokens_; }

private:
  Config config_;

  // Full accumulated text (for pattern matching)
  std::string accumulatedText_;

  // Circular buffer for entropy analysis (last N characters)
  std::deque<char> entropyWindow_;

  // Token count approximation (very rough: count non-space sequences)
  std::uint64_t totalTokens_ = 0;

  // Last detected reason (cached)
  mutable InsanityCheckResult lastResult_;

  // Helper: count approximate tokens in text
  static std::uint64_t approximateTokenCount(const std::string &text);

  // Helper: calculate Shannon entropy of a string
  static double calculateEntropy(const std::string &text);

  // Helper: calculate ratio of non-alphanumeric chars
  static double calculateNonWordRatio(const std::string &text);

  // Helper: check for repetitive patterns in accumulated text
  InsanityCheckResult checkRepetition() const;

  // Helper: check for excessive token count
  InsanityCheckResult checkTokenLimit() const;

  // Helper: check for high-entropy gibberish
  InsanityCheckResult checkEntropy() const;

  // Helper: trim to max pattern length from the end
  static std::string tail(const std::string &s, std::size_t maxLen);
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_STREAMSANITYDETECTOR_HPP
