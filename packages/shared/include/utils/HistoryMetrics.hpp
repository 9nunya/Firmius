#ifndef FIRMIUS_SHARED_HISTORYMETRICS_HPP
#define FIRMIUS_SHARED_HISTORYMETRICS_HPP

#include "Context.hpp"

namespace firmius::shared {

AgentMetrics aggregateHistoryMetrics(const AgentHistory &history);

}

#endif
