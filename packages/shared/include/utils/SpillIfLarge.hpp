#ifndef FIRMIUS_SHARED_UTILS_SPILL_IF_LARGE_HPP
#define FIRMIUS_SHARED_UTILS_SPILL_IF_LARGE_HPP

#include <cstdint>
#include <optional>
#include <string>

/**
 * @file SpillIfLarge.hpp
 * @brief Token-waste pass 2/4: shared "spill to /tmp on overflow" helper.
 *
 * Several tools accept arbitrarily large foreign output (process stdout,
 * Python stdout, file reads, web fetch HTML). When that output is too big
 * to feed an LLM cheaply, we spill it to a file under /tmp and return a
 * tiny reference + a tail peek instead. This module unifies that pattern
 * across tools so thresholds, ref shape, and tail size are consistent.
 *
 * This helper does NOT decide what is "large" — callers pass a threshold
 * appropriate for their tool (e.g. 64 KB for process stdout, 32 KB for web
 * fetch). It only performs the spill when the threshold is exceeded.
 */
namespace firmius::shared::utils {

struct SpillResult {
  /// True when content was written to disk and `tail` reflects only the
  /// last `tailBytes` of the original. False means content was small
  /// enough to keep inline; `tail` contains the full content.
  bool spilled = false;
  /// Filesystem path of the spilled file (empty when not spilled).
  std::string refPath;
  /// Total byte size of the original content (always populated).
  std::uint64_t totalBytes = 0;
  /// Total newline-delimited line count of the original (always populated).
  std::uint64_t totalLines = 0;
  /// Tail of the content. When not spilled, this is the entire content.
  /// When spilled, the last `tailBytes` bytes of the original (truncated
  /// at a line boundary to avoid splitting a line mid-byte).
  std::string tail;
};

/**
 * @brief Conditionally spill `content` to a /tmp file if it exceeds
 *        `thresholdBytes`. Returns a SpillResult describing the outcome.
 *
 * @param content        Bytes to consider for spilling.
 * @param thresholdBytes Spill iff `content.size() > thresholdBytes`.
 * @param filenamePrefix Prefix for the on-disk filename (e.g. "firmius_proc").
 *                       Final path is `/tmp/<prefix>_<uuid>.log`.
 * @param tailBytes      When spilled, return at most this many trailing
 *                       bytes inline. Defaults to 4 KB.
 * @return SpillResult. On disk-write failure, `spilled` is false, `refPath`
 *         is empty, and `tail` falls back to the original content (so the
 *         caller is never silently empty).
 */
SpillResult spillIfLarge(const std::string &content,
                         std::size_t thresholdBytes,
                         const std::string &filenamePrefix,
                         std::size_t tailBytes = 4096);

/**
 * @brief Build a one-line prose suffix describing a spill, suitable for
 *        appending to a tool result's prose `result` string. Returns an
 *        empty string when nothing was spilled.
 *
 * Example output: " (full output 312 KB / 4823 lines spilled to
 *                  /tmp/firmius_proc_<uuid>.log; showing last 4 KB)"
 */
std::string formatSpillNote(const SpillResult &spill);

}  // namespace firmius::shared::utils

#endif  // FIRMIUS_SHARED_UTILS_SPILL_IF_LARGE_HPP
