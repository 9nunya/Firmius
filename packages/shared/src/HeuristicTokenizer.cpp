#include "HeuristicTokenizer.hpp"

#include <algorithm>
#include <cmath>

namespace firmius::shared {

namespace {
constexpr double kApproxBytesPerToken = 4.0;
}

uint32_t HeuristicTokenizer::count(std::string_view text) const {
  if (text.empty()) {
    return 0;
  }
  return static_cast<uint32_t>(
      std::max(1.0, std::ceil(static_cast<double>(text.size()) /
                              kApproxBytesPerToken)));
}

std::string HeuristicTokenizer::id() const { return "heuristic"; }

} // namespace firmius::shared
