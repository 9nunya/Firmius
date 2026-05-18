#include "utils/SpillIfLarge.hpp"

#include "utils/StringUtil.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace firmius::shared::utils {

namespace {

// Count newline-delimited lines in `s`. Trailing partial line counts as a
// line iff `s` is non-empty and does not end with a newline.
std::uint64_t countLines(const std::string &s) {
  if (s.empty()) return 0;
  std::uint64_t lines = 0;
  for (char c : s) {
    if (c == '\n') ++lines;
  }
  if (s.back() != '\n') ++lines;
  return lines;
}

// Return the last `tailBytes` of `s` truncated at a line boundary so we
// never split a line in the middle. Returns the entire string if it is
// already shorter than `tailBytes`.
std::string lastNBytesAtLineBoundary(const std::string &s,
                                     std::size_t tailBytes) {
  if (s.size() <= tailBytes) return s;
  std::size_t start = s.size() - tailBytes;
  // Walk forward to the next newline so we don't return a half line.
  while (start < s.size() && s[start] != '\n') ++start;
  if (start < s.size()) ++start;  // consume the newline itself
  // If we walked past everything (entire tail was one giant line), fall
  // back to the raw tail — better than returning an empty string.
  if (start >= s.size()) {
    start = s.size() - tailBytes;
  }
  return s.substr(start);
}

}  // namespace

SpillResult spillIfLarge(const std::string &content,
                         std::size_t thresholdBytes,
                         const std::string &filenamePrefix,
                         std::size_t tailBytes) {
  SpillResult out;
  out.totalBytes = static_cast<std::uint64_t>(content.size());
  out.totalLines = countLines(content);

  if (content.size() <= thresholdBytes) {
    out.spilled = false;
    out.tail = content;
    return out;
  }

  // Build the spill path.
  std::filesystem::path spillDir = std::filesystem::temp_directory_path();
  const std::string filename =
      filenamePrefix + "_" + firmius::shared::StringUtil::generateUuid() +
      ".log";
  const std::filesystem::path fullPath = spillDir / filename;

  try {
    std::ofstream ofs(fullPath, std::ios::binary | std::ios::trunc);
    if (!ofs) {
      // Disk-write failed before we wrote anything; degrade gracefully by
      // returning the original content as the tail. The caller can still
      // emit a reasonable response, just without the spill ref.
      out.spilled = false;
      out.tail = content;
      return out;
    }
    ofs.write(content.data(),
              static_cast<std::streamsize>(content.size()));
    ofs.flush();
    if (!ofs) {
      out.spilled = false;
      out.tail = content;
      return out;
    }
  } catch (const std::exception &) {
    out.spilled = false;
    out.tail = content;
    return out;
  }

  out.spilled = true;
  out.refPath = fullPath.string();
  out.tail = lastNBytesAtLineBoundary(content, tailBytes);
  return out;
}

std::string formatSpillNote(const SpillResult &spill) {
  if (!spill.spilled) return "";
  std::ostringstream s;
  // Pretty byte size.
  auto fmtBytes = [](std::uint64_t n) {
    std::ostringstream o;
    if (n < 1024) {
      o << n << " B";
    } else if (n < 1024ull * 1024) {
      o.precision(1);
      o << std::fixed << (static_cast<double>(n) / 1024.0) << " KB";
    } else {
      o.precision(2);
      o << std::fixed << (static_cast<double>(n) / (1024.0 * 1024.0)) << " MB";
    }
    return o.str();
  };
  s << " (full output " << fmtBytes(spill.totalBytes) << " / "
    << spill.totalLines << " line" << (spill.totalLines == 1 ? "" : "s")
    << " spilled to " << spill.refPath << "; showing last "
    << fmtBytes(static_cast<std::uint64_t>(spill.tail.size())) << ")";
  return s.str();
}

}  // namespace firmius::shared::utils
