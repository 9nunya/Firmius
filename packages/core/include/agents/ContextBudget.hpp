#ifndef FIRMIUS_CORE_CONTEXT_BUDGET_HPP
#define FIRMIUS_CORE_CONTEXT_BUDGET_HPP

#include "Context.hpp"
#include "IProvider.hpp"
#include "Metrics.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace firmius::core {

shared::ContextWindowMetrics estimateContextWindowMetrics(
    const shared::AgentHistory &requestHistory,
    const std::vector<firmius::provider::ToolDefinition> &tools);

void reconcileContextWindowMetrics(shared::AgentMetrics &metrics);

std::vector<shared::ContextBucketMetrics>
rankContextBuckets(const shared::ContextWindowMetrics &metrics);

std::string summarizeContextWindowMetrics(
    const shared::ContextWindowMetrics &metrics,
    std::size_t maxBuckets = 3);

} // namespace firmius::core

#endif
