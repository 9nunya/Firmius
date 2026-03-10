#include "utils/HistoryMetrics.hpp"

namespace firmius::shared {

AgentMetrics aggregateHistoryMetrics(const AgentHistory &history) {
  AgentMetrics result;
  for (const auto &turn : history.turns) {
    result += turn.metrics;
  }
  return result;
}

}
