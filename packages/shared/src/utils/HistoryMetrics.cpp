#include "utils/HistoryMetrics.hpp"

namespace firmius::shared {

AgentMetrics aggregateHistoryMetrics(const AgentHistory &history) {
  AgentMetrics result;
  for (const auto &turn : history.turns) {
    result += turn.metrics;
  }

  if (result.tokens.contextSize == 0) {
    for (auto it = history.turns.rbegin(); it != history.turns.rend(); ++it) {
      if (it->metrics.tokens.contextSize > 0) {
        result.tokens.contextSize = it->metrics.tokens.contextSize;
        break;
      }
      if (result.tokens.contextSize == 0 && it->metrics.tokens.prompt > 0) {
        result.tokens.contextSize = it->metrics.tokens.prompt;
      }
    }
  }

  return result;
}

}
