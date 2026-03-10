#ifndef FIRMIUS_SHARED_HISTORY_METRICS_HPP
#define FIRMIUS_SHARED_HISTORY_METRICS_HPP

#include "Context.hpp"

namespace firmius::shared {

AgentMetrics aggregateHistoryMetrics(const AgentHistory &history);

}

#endif
