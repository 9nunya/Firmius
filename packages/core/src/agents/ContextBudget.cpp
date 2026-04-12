#include "agents/ContextBudget.hpp"

#include "Message.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <unordered_map>

namespace firmius::core {

namespace {

constexpr double kApproxBytesPerToken = 4.0;

std::uint32_t estimateTokensForText(const std::string &text) {
  if (text.empty()) {
    return 0;
  }
  return static_cast<std::uint32_t>(
      std::max(1.0, std::ceil(static_cast<double>(text.size()) /
                              kApproxBytesPerToken)));
}

std::string bucketLabelForTurn(const shared::AgentTurn &turn) {
  if (turn.turnId == "bootstrap-system") {
    return "system_prompt";
  }
  if (turn.turnId == "runtime-overlay-work-state") {
    return "live_work_state";
  }
  if (turn.turnId == "runtime-overlay-watched-files") {
    return "watched_files";
  }
  if (turn.turnId == "runtime-overlay-loaded-skills") {
    return "loaded_skills";
  }
  if (turn.turnId == "runtime-overlay-user-memory") {
    return "user_memory";
  }
  if (turn.turnId == "runtime-overlay-rolling-status") {
    return "rolling_status";
  }
  if (turn.turnId == "runtime-overlay-rolling-memory") {
    return "rolling_observations";
  }
  return "system_overlay";
}

void addBucket(std::unordered_map<std::string, std::uint32_t> &buckets,
               const std::string &label, std::uint32_t tokens) {
  if (tokens == 0) {
    return;
  }
  buckets[label] += tokens;
}

void accountMessageContent(std::unordered_map<std::string, std::uint32_t> &buckets,
                           const shared::AgentTurn &turn,
                           const shared::Message &msg) {
  const bool isSystem = msg.role == shared::Role::System;
  for (const auto &part : msg.content) {
    if (const auto *txt = std::get_if<shared::TextContent>(&part)) {
      addBucket(buckets,
                isSystem ? bucketLabelForTurn(turn) : "conversation_history",
                estimateTokensForText(txt->text));
    } else if (const auto *thinking =
                   std::get_if<shared::ThinkingContent>(&part)) {
      addBucket(buckets, "conversation_history",
                estimateTokensForText(thinking->thinking));
    } else if (const auto *toolCall =
                   std::get_if<shared::ToolCallContent>(&part)) {
      addBucket(buckets, "tool_history",
                estimateTokensForText(toolCall->name) +
                    estimateTokensForText(toolCall->args));
    } else if (const auto *toolResult =
                   std::get_if<shared::ToolResultContent>(&part)) {
      addBucket(buckets, "tool_history",
                estimateTokensForText(toolResult->result));
    } else if (const auto *image = std::get_if<shared::ImageContent>(&part)) {
      addBucket(buckets, "images",
                estimateTokensForText(image->mediaType) +
                    estimateTokensForText(image->url) +
                    estimateTokensForText(image->detail));
    } else if (const auto *notice = std::get_if<shared::NoticeContent>(&part)) {
      addBucket(buckets, isSystem ? bucketLabelForTurn(turn)
                                  : "conversation_history",
                estimateTokensForText(notice->title) +
                    estimateTokensForText(notice->message) +
                    estimateTokensForText(notice->details));
    } else if (const auto *error = std::get_if<shared::ErrorContent>(&part)) {
      addBucket(buckets, isSystem ? bucketLabelForTurn(turn)
                                  : "conversation_history",
                estimateTokensForText(error->errorName) +
                    estimateTokensForText(error->description) +
                    estimateTokensForText(error->details));
    }
  }
}

std::string humanizeBucketLabel(const std::string &label) {
  std::string out = label;
  for (char &ch : out) {
    if (ch == '_') {
      ch = ' ';
    }
  }
  return out;
}

} // namespace

shared::ContextWindowMetrics estimateContextWindowMetrics(
    const shared::AgentHistory &requestHistory,
    const std::vector<firmius::provider::ToolDefinition> &tools) {
  std::unordered_map<std::string, std::uint32_t> bucketTotals;

  for (const auto &turn : requestHistory.turns) {
    for (const auto &msg : turn.messages) {
      accountMessageContent(bucketTotals, turn, msg);
    }
  }

  for (const auto &tool : tools) {
    addBucket(bucketTotals, "tool_schemas",
              estimateTokensForText(tool.name) +
                  estimateTokensForText(tool.description) +
                  estimateTokensForText(tool.inputSchema));
  }

  shared::ContextWindowMetrics metrics;
  metrics.buckets.reserve(bucketTotals.size());
  for (const auto &[label, estimated] : bucketTotals) {
    metrics.buckets.push_back(
        shared::ContextBucketMetrics{label, estimated, 0});
    metrics.sentTokens += estimated;
  }

  std::sort(metrics.buckets.begin(), metrics.buckets.end(),
            [](const auto &lhs, const auto &rhs) {
              if (lhs.estimatedTokens != rhs.estimatedTokens) {
                return lhs.estimatedTokens > rhs.estimatedTokens;
              }
              return lhs.label < rhs.label;
            });
  return metrics;
}

void reconcileContextWindowMetrics(shared::AgentMetrics &metrics) {
  auto &context = metrics.context;
  const std::uint32_t actualPromptTokens =
      metrics.tokens.contextSize > 0
          ? metrics.tokens.contextSize
          : (metrics.tokens.prompt + metrics.tokens.cacheRead);
  context.rawPromptTokens = actualPromptTokens;
  context.billedPromptTokens = metrics.tokens.prompt;

  if (actualPromptTokens == 0 || context.buckets.empty()) {
    context.reserveTokens = actualPromptTokens;
    return;
  }

  std::uint32_t estimatedTotal = 0;
  for (const auto &bucket : context.buckets) {
    estimatedTotal += bucket.estimatedTokens;
  }

  if (estimatedTotal == 0) {
    context.reserveTokens = actualPromptTokens;
    return;
  }

  std::uint32_t assigned = 0;
  for (std::size_t i = 0; i < context.buckets.size(); ++i) {
    auto &bucket = context.buckets[i];
    if (i + 1 == context.buckets.size()) {
      bucket.actualTokens = actualPromptTokens > assigned
                                ? actualPromptTokens - assigned
                                : 0;
      assigned += bucket.actualTokens;
      continue;
    }
    const double ratio =
        static_cast<double>(bucket.estimatedTokens) / estimatedTotal;
    bucket.actualTokens = static_cast<std::uint32_t>(
        std::llround(ratio * static_cast<double>(actualPromptTokens)));
    if (assigned + bucket.actualTokens > actualPromptTokens) {
      bucket.actualTokens = actualPromptTokens - assigned;
    }
    assigned += bucket.actualTokens;
  }

  context.reserveTokens =
      actualPromptTokens > assigned ? actualPromptTokens - assigned : 0;
}

std::vector<shared::ContextBucketMetrics>
rankContextBuckets(const shared::ContextWindowMetrics &metrics) {
  std::vector<shared::ContextBucketMetrics> ranked = metrics.buckets;
  std::sort(ranked.begin(), ranked.end(), [](const auto &lhs, const auto &rhs) {
    const auto left = lhs.actualTokens > 0 ? lhs.actualTokens : lhs.estimatedTokens;
    const auto right =
        rhs.actualTokens > 0 ? rhs.actualTokens : rhs.estimatedTokens;
    if (left != right) {
      return left > right;
    }
    return lhs.label < rhs.label;
  });
  return ranked;
}

std::string summarizeContextWindowMetrics(
    const shared::ContextWindowMetrics &metrics, std::size_t maxBuckets) {
  std::ostringstream out;
  out << "sent=" << metrics.sentTokens;
  if (metrics.rawPromptTokens > 0) {
    out << " raw=" << metrics.rawPromptTokens;
  }
  if (metrics.billedPromptTokens > 0) {
    out << " billed=" << metrics.billedPromptTokens;
  }

  const auto ranked = rankContextBuckets(metrics);
  if (!ranked.empty() && maxBuckets > 0) {
    out << " buckets:";
    std::size_t emitted = 0;
    for (const auto &bucket : ranked) {
      if (emitted >= maxBuckets) {
        break;
      }
      const auto tokens =
          bucket.actualTokens > 0 ? bucket.actualTokens : bucket.estimatedTokens;
      if (tokens == 0) {
        continue;
      }
      out << " " << humanizeBucketLabel(bucket.label) << "=" << tokens;
      ++emitted;
    }
  }

  if (metrics.reserveTokens > 0) {
    out << " reserve=" << metrics.reserveTokens;
  }
  return out.str();
}

} // namespace firmius::core
