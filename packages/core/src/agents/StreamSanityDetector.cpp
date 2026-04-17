#include "agents/StreamSanityDetector.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <unordered_map>

namespace firmius::core {

StreamSanityDetector::StreamSanityDetector(const Config &cfg)
    : config_(cfg), accumulatedText_(), entropyWindow_(), totalTokens_(0) {}

void StreamSanityDetector::addDelta(const std::string &delta) {
  if (delta.empty() || !config_.enabled) {
    return;
  }

  // Accumulate full text
  accumulatedText_ += delta;

  // Update entropy window (circular buffer)
  for (char c : delta) {
    entropyWindow_.push_back(c);
    if (static_cast<int>(entropyWindow_.size()) > config_.entropyWindowSize) {
      entropyWindow_.pop_front();
    }
  }

  // Approximate token count (rough: count sequences of non-space chars)
  totalTokens_ += approximateTokenCount(delta);
}

InsanityCheckResult StreamSanityDetector::check() const {
  if (!config_.enabled || accumulatedText_.empty()) {
    return InsanityCheckResult{};
  }

  // Check in order of computational cost (cheapest first)

  // 1. Token count limit (cheapest)
  if (auto result = checkTokenLimit(); result.isInsane) {
    return result;
  }

  // 2. Repetition detection
  if (auto result = checkRepetition(); result.isInsane) {
    return result;
  }

  // 3. Entropy-based gibberish detection (most expensive)
  if (config_.entropyDetectionEnabled) {
    if (auto result = checkEntropy(); result.isInsane) {
      return result;
    }
  }

  return InsanityCheckResult{};
}

void StreamSanityDetector::clear() {
  accumulatedText_.clear();
  entropyWindow_.clear();
  totalTokens_ = 0;
  lastResult_ = InsanityCheckResult{};
}

std::uint64_t StreamSanityDetector::approximateTokenCount(const std::string &text) {
  if (text.empty()) {
    return 0;
  }

  std::uint64_t count = 0;
  bool inToken = false;

  for (unsigned char c : text) {
    if (std::isspace(c)) {
      inToken = false;
    } else {
      if (!inToken) {
        count++;
        inToken = true;
      }
    }
  }

  return count;
}

double StreamSanityDetector::calculateEntropy(const std::string &text) {
  if (text.empty()) {
    return 0.0;
  }

  std::unordered_map<char, int> freq;
  for (char c : text) {
    freq[c]++;
  }

  double entropy = 0.0;
  const double len = static_cast<double>(text.size());

  for (const auto &[_, count] : freq) {
    double p = static_cast<double>(count) / len;
    if (p > 0) {
      entropy -= p * std::log2(p);
    }
  }

  return entropy;
}

double StreamSanityDetector::calculateNonWordRatio(const std::string &text) {
  if (text.empty()) {
    return 0.0;
  }

  int nonWordCount = 0;
  for (char c : text) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && !std::isspace(static_cast<unsigned char>(c))) {
      nonWordCount++;
    }
  }

  return (static_cast<double>(nonWordCount) / text.size()) * 100.0;
}

std::string StreamSanityDetector::tail(const std::string &s, std::size_t maxLen) {
  if (s.size() <= maxLen) {
    return s;
  }
  return s.substr(s.size() - maxLen);
}

InsanityCheckResult StreamSanityDetector::checkRepetition() const {
  const std::string &text = accumulatedText_;

  // Need at least some text to check
  if (text.size() < static_cast<std::size_t>(config_.minPatternLength * 2)) {
    return InsanityCheckResult{};
  }

  // Only check the tail portion for performance
  const std::size_t checkWindow = config_.maxPatternLength * config_.minConsecutiveRepeats * 2;
  std::string window = tail(text, checkWindow);

  // Try different pattern lengths
  for (int patLen = config_.minPatternLength;
       patLen <= config_.maxPatternLength && patLen * 2 <= static_cast<int>(window.size());
       ++patLen) {

    // Take candidate pattern from the very end
    std::string pattern = window.substr(window.size() - patLen);

    // Count consecutive repeats backwards
    int repeatCount = 1;  // We already have one occurrence
    std::size_t pos = window.size() - patLen;

    while (pos >= static_cast<std::size_t>(patLen)) {
      pos -= patLen;
      std::string prevChunk = window.substr(pos, patLen);
      if (prevChunk == pattern) {
        repeatCount++;
      } else {
        break;
      }
    }

    if (repeatCount >= config_.minConsecutiveRepeats) {
      std::ostringstream reason;
      reason << "Repetitive content detected: the same "
             << patLen << "-character pattern repeated "
             << repeatCount << " consecutive times";
      return InsanityCheckResult{true, reason.str(), InsanityCheckResult::Type::RepetitiveLoop};
    }
  }

  return InsanityCheckResult{};
}

InsanityCheckResult StreamSanityDetector::checkTokenLimit() const {
  if (totalTokens_ > config_.maxTokenThreshold) {
    std::ostringstream reason;
    reason << "Excessive token count: " << totalTokens_
           << " tokens exceeded limit of " << config_.maxTokenThreshold;
    return InsanityCheckResult{true, reason.str(), InsanityCheckResult::Type::ExcessiveTokenCount};
  }
  return InsanityCheckResult{};
}

InsanityCheckResult StreamSanityDetector::checkEntropy() const {
  // Build string from entropy window
  std::string text;
  text.reserve(entropyWindow_.size());
  for (char c : entropyWindow_) {
    text.push_back(c);
  }

  if (static_cast<int>(text.size()) < config_.entropyWindowSize / 2) {
    // Not enough data yet
    return InsanityCheckResult{};
  }

  double entropy = calculateEntropy(text);

  if (entropy > config_.entropyThreshold) {
    double nonWordRatio = calculateNonWordRatio(text);
    if (nonWordRatio > config_.gibberishNonWordRatio) {
      std::ostringstream reason;
      reason << "High-entropy random content detected: entropy="
             << entropy << " (threshold " << config_.entropyThreshold
             << "), non-word ratio=" << nonWordRatio << "%";
      return InsanityCheckResult{true, reason.str(), InsanityCheckResult::Type::HighEntropyJunk};
    }
  }

  return InsanityCheckResult{};
}

} // namespace firmius::core
