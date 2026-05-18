#include "agents/working_memory/WorkingMemory.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <unordered_set>

namespace firmius::core::working_memory {

namespace {

constexpr std::uint32_t kDefaultContextWindow = 128'000;

std::uint64_t nowMicros() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool turnIsUser(const shared::AgentTurn& turn) {
  for (const auto& msg : turn.messages) {
    if (msg.role == shared::Role::User) {
      return true;
    }
  }
  return false;
}

std::uint32_t imagePartsInTurn(const shared::AgentTurn& turn) {
  std::uint32_t count = 0;
  for (const auto& msg : turn.messages) {
    for (const auto& part : msg.content) {
      if (std::holds_alternative<shared::ImageContent>(part)) {
        ++count;
      }
    }
  }
  return count;
}

std::string buildLatestQueryText(const shared::AgentHistory& history,
                                 std::size_t maxBytes = 4096) {
  // Concatenate the last user message and last assistant message to form
  // a query. Bias toward user intent.
  std::string userBlob;
  std::string assistantBlob;
  for (auto it = history.turns.rbegin(); it != history.turns.rend(); ++it) {
    bool gotUser = false;
    bool gotAssistant = false;
    for (const auto& msg : it->messages) {
      const std::string* target =
          msg.role == shared::Role::User ? &userBlob :
          msg.role == shared::Role::Assistant ? &assistantBlob : nullptr;
      if (!target) {
        continue;
      }
      for (const auto& part : msg.content) {
        if (const auto* txt = std::get_if<shared::TextContent>(&part)) {
          if (target->size() + txt->text.size() < maxBytes) {
            *const_cast<std::string*>(target) += txt->text;
            *const_cast<std::string*>(target) += '\n';
          }
        }
      }
      if (msg.role == shared::Role::User) gotUser = true;
      if (msg.role == shared::Role::Assistant) gotAssistant = true;
    }
    if (!userBlob.empty() && !assistantBlob.empty()) {
      break;
    }
    (void)gotUser;
    (void)gotAssistant;
  }
  std::string out = userBlob;
  if (!out.empty() && !assistantBlob.empty()) {
    out += "\n";
  }
  out += assistantBlob;
  if (out.size() > maxBytes) {
    out.resize(maxBytes);
  }
  return out;
}

} // namespace

ResolvedThresholds resolveThresholds(const shared::WorkingMemoryConfig& config,
                                     std::uint32_t actorContextWindow) {
  ResolvedThresholds t;
  t.contextWindow =
      actorContextWindow == 0 ? kDefaultContextWindow : actorContextWindow;

  auto fromRatio = [&](float ratio) -> std::uint32_t {
    if (ratio <= 0.0f) return 0;
    return static_cast<std::uint32_t>(
        std::floor(static_cast<double>(t.contextWindow) *
                   static_cast<double>(ratio)));
  };

  t.bufferTokens = fromRatio(config.bufferOccupancyRatio);
  t.targetTokens = fromRatio(config.targetOccupancyRatio);
  t.emergencyTokens = fromRatio(config.emergencyOccupancyRatio);
  t.recencyTailTokens = std::max<std::uint32_t>(
      config.minimumRecencyTailTokens, fromRatio(config.recencyTailRatio));

  // Sanity ordering: ensure buffer < target < emergency. Defends against
  // user-supplied configs that violate the invariant.
  if (t.targetTokens <= t.bufferTokens) {
    t.targetTokens = t.bufferTokens + std::max<std::uint32_t>(1, t.bufferTokens / 8);
  }
  if (t.emergencyTokens <= t.targetTokens) {
    t.emergencyTokens = t.targetTokens + std::max<std::uint32_t>(1, t.targetTokens / 8);
  }
  return t;
}

FocusCounts countFocusElements(const shared::AgentHistory& history) {
  FocusCounts c;
  for (const auto& turn : history.turns) {
    if (turnIsUser(turn)) {
      c.userPromptsTotal += 1;
    }
    c.imagePartsTotal += imagePartsInTurn(turn);
  }
  return c;
}

std::vector<bool> buildHardPinMask(const PinClassification& classification) {
  std::vector<bool> mask(classification.decisions.size(), false);
  for (std::size_t i = 0; i < classification.decisions.size(); ++i) {
    if (classification.decisions[i].kind == PinKind::HardPin) {
      mask[i] = true;
    }
  }
  return mask;
}

shared::AgentHistory assembleWorkingSet(const shared::AgentContext& context,
                                        const shared::AgentHistory& history,
                                        const WorkingMemoryInputs& inputs,
                                        WorkingMemoryReport& report) {
  const std::uint64_t startMicros = nowMicros();

  if (!inputs.tokenizer) {
    // Without a tokenizer we cannot reason about tokens. Fall back to
    // pass-through, but populate the report so the caller can detect this.
    report = {};
    report.thresholds.contextWindow = inputs.actorContextWindow;
    report.rawHistoryTokens = 0;
    report.workingSetTokens = 0;
    report.hotPathLatencyMicros = nowMicros() - startMicros;
    return history;
  }

  const auto thresholds =
      resolveThresholds(context.config.workingMemory, inputs.actorContextWindow);

  // Below buffer threshold OR feature disabled: return history as-is.
  // Still produce focus telemetry so /metrics is honest about retention.
  const FocusCounts focus = countFocusElements(history);

  // Compute raw history tokens once.
  PinPolicyInputs pinInputs;
  pinInputs.recencyTailTokens = thresholds.recencyTailTokens;
  for (const auto& f : context.state.editedFiles) {
    pinInputs.editedFiles.insert(f);
  }
  for (const auto& id : context.state.pinnedTurnIds) {
    pinInputs.agentPinnedTurnIds.insert(id);
  }
  if (inputs.activeStateReferences) {
    for (auto& id : inputs.activeStateReferences()) {
      pinInputs.activeStateReferences.insert(id);
    }
  }

  const PinClassification classification =
      classifyPins(history, context, pinInputs, *inputs.tokenizer);

  report = {};
  report.thresholds = thresholds;
  report.rawHistoryTokens = classification.totalTokens;
  report.hardPinTokens = classification.hardPinTokens;
  report.softPinTokens = classification.softPinTokens;
  report.evictableTokens = classification.evictableTokens;
  report.userPromptsTotal = focus.userPromptsTotal;
  report.imagePartsTotal = focus.imagePartsTotal;
  report.aboveBufferThreshold = classification.totalTokens >= thresholds.bufferTokens;
  report.aboveTargetThreshold = classification.totalTokens >= thresholds.targetTokens;
  report.aboveEmergencyThreshold =
      classification.totalTokens >= thresholds.emergencyTokens;

  if (!context.config.workingMemory.enabled || !report.aboveBufferThreshold) {
    // Pass-through. Count user prompts retained = total (since we keep
    // everything), images retained = total.
    report.workingSetTokens = report.rawHistoryTokens;
    report.userPromptsRetained = focus.userPromptsTotal;
    report.imagePartsRetained = focus.imagePartsTotal;
    report.pinnedTurnCount = classification.hardPinCount + classification.softPinCount;
    report.evictedTurnCount = 0;
    report.hotPathLatencyMicros = nowMicros() - startMicros;
    return history;
  }

  // Above buffer: actively assemble.

  // Step 1: include hard pins + soft pins. Track which evictable turns we've
  // accepted and which are eligible for relevance recall.
  std::vector<std::size_t> includedIndexes;
  includedIndexes.reserve(history.turns.size());
  std::vector<bool> isIncluded(history.turns.size(), false);
  std::vector<std::size_t> evictableEligible;

  std::uint32_t runningTokens = 0;
  for (std::size_t i = 0; i < classification.decisions.size(); ++i) {
    const auto kind = classification.decisions[i].kind;
    if (kind == PinKind::HardPin || kind == PinKind::SoftPin) {
      includedIndexes.push_back(i);
      isIncluded[i] = true;
      runningTokens += classification.decisions[i].estimatedTokens;
    } else {
      evictableEligible.push_back(i);
    }
  }

  // Step 2: relevance fill above target threshold.
  std::vector<std::string> recalledTurnIds;
  if (report.aboveTargetThreshold && context.config.workingMemory.embeddingsEnabled &&
      inputs.relevanceQuery && !evictableEligible.empty()) {
    const std::string query = buildLatestQueryText(history);
    if (!query.empty()) {
      // The embedding cost is the query text token count.
      report.tokensSpentOnEmbeddings += inputs.tokenizer->count(query);
      auto recalled = inputs.relevanceQuery(
          query,
          static_cast<std::size_t>(context.config.workingMemory.embeddingTopK));

      // Map turn IDs back to indexes.
      std::unordered_set<std::string> recallSet(recalled.begin(), recalled.end());
      for (std::size_t idx : evictableEligible) {
        if (recallSet.count(history.turns[idx].turnId) > 0 && !isIncluded[idx]) {
          includedIndexes.push_back(idx);
          isIncluded[idx] = true;
          runningTokens += classification.decisions[idx].estimatedTokens;
          recalledTurnIds.push_back(history.turns[idx].turnId);
        }
      }
    }
  }

  // Step 3: between buffer and target, fill remaining evictable turns until
  // we'd exceed the target threshold. Above target, only the recalled set is
  // kept from the evictable region.
  if (!report.aboveTargetThreshold) {
    for (std::size_t idx : evictableEligible) {
      if (isIncluded[idx]) continue;
      const std::uint32_t addCost = classification.decisions[idx].estimatedTokens;
      if (runningTokens + addCost > thresholds.targetTokens) {
        // Note: we still try to include older Evictable turns by stopping
        // here rather than skipping ahead; pin classification already
        // ensured the freshest turns are HardPin via the recency tail.
        break;
      }
      includedIndexes.push_back(idx);
      isIncluded[idx] = true;
      runningTokens += addCost;
    }
  }

  // Sort included indexes back into history order so the request reads
  // chronologically.
  std::sort(includedIndexes.begin(), includedIndexes.end());

  // Step 4: build the assembled history.
  shared::AgentHistory assembled;
  assembled.threadId = history.threadId;
  assembled.turns.reserve(includedIndexes.size());
  for (std::size_t idx : includedIndexes) {
    assembled.turns.push_back(history.turns[idx]);
  }

  // Step 5: deflation against SoftPin turns within the assembled history,
  // when above target. Hard pins are protected by the mask.
  if (report.aboveTargetThreshold && inputs.archive) {
    DeflationSelectorInputs selInputs;
    selInputs.minPartTokens = context.config.workingMemory.deflationMinPartTokens;
    selInputs.defaultHorizon =
        context.config.workingMemory.defaultDeflationTurnHorizon;
    selInputs.horizonsByTool = context.config.workingMemory.deflationHorizonsByTool;

    // Note: we deliberately do NOT pass a hard-pin mask here. The pin
    // distinction governs which turns travel in the working set — it does
    // not protect oversize tool-result bodies inside hard-pinned turns
    // from in-place body replacement. Deflation preserves toolCallId and
    // message envelope, so the turn's hard-pin status (and any tool-call
    // pairing) is unaffected. This is critical: the fattest tool result
    // bodies live in the recency tail, which is hard-pinned by policy.
    // Excluding hard pins from deflation would mean those bodies are
    // never compressible — defeating the layer's purpose at high
    // occupancy.
    selInputs.hardPinMask = nullptr;

    auto candidates =
        selectDeflationCandidates(assembled, *inputs.tokenizer, selInputs);

    // Above emergency: deflate everything we can synchronously to reduce
    // token pressure. Below emergency but above target: deflate only the
    // top-N oldest candidates to avoid spending too much hot-path budget.
    const std::size_t cap = report.aboveEmergencyThreshold
                                ? candidates.size()
                                : std::min<std::size_t>(candidates.size(), 4);
    candidates.resize(cap);

    if (!candidates.empty()) {
      // When an async upgrade hook is provided, capture the ORIGINAL bodies
      // before deflation mutates them in place. Then run deflation with a
      // null summarizer so we emit deterministic stubs synchronously
      // (cheap), and use the captured bodies + archive ids to fire async
      // upgrade jobs that replace the stubs with LLM summaries on a
      // background thread.
      const bool runAsync =
          static_cast<bool>(inputs.asyncUpgrade) &&
          static_cast<bool>(inputs.synchronousSummarizer);

      std::vector<std::string> capturedBodies;
      if (runAsync) {
        capturedBodies.reserve(candidates.size());
        for (const auto& cand : candidates) {
          if (cand.turnIndex < assembled.turns.size()) {
            const auto& turn = assembled.turns[cand.turnIndex];
            if (cand.messageIndex < turn.messages.size()) {
              const auto& msg = turn.messages[cand.messageIndex];
              if (cand.partIndex < msg.content.size()) {
                if (const auto* tr = std::get_if<shared::ToolResultContent>(
                        &msg.content[cand.partIndex])) {
                  capturedBodies.push_back(tr->result);
                  continue;
                }
              }
            }
          }
          capturedBodies.emplace_back(); // placeholder for misaligned candidate
        }
      }

      const auto defResult = deflateCandidates(
          assembled, *inputs.archive, candidates, *inputs.tokenizer,
          runAsync ? SummarizerFn(nullptr) : inputs.synchronousSummarizer);
      report.deflatedPartCount += defResult.deflatedPartCount;
      report.tokensSavedByDeflation += defResult.tokensSaved;
      report.tokensSpentOnSummaries += defResult.tokensSpentOnSummaries;

      // Fire async upgrade jobs. archiveIds is aligned with the ordered
      // subset of candidates that successfully deflated. We walk both the
      // candidates and archiveIds in lockstep, advancing archiveIds only
      // when a candidate had a usable body captured.
      if (runAsync && !defResult.archiveIds.empty()) {
        std::size_t archiveCursor = 0;
        for (std::size_t i = 0;
             i < candidates.size() && archiveCursor < defResult.archiveIds.size();
             ++i) {
          if (i < capturedBodies.size() && !capturedBodies[i].empty()) {
            const auto& cand = candidates[i];
            const auto& archiveId = defResult.archiveIds[archiveCursor++];
            try {
              inputs.asyncUpgrade(
                  // Use the assembled history's turnId at cand.turnIndex
                  // — the assembler did not reorder relative to candidate
                  // selection.
                  assembled.turns[cand.turnIndex].turnId,
                  cand.messageIndex, cand.partIndex, cand.toolName,
                  cand.toolArgs, capturedBodies[i], archiveId);
            } catch (...) {
              // Best-effort.
            }
          }
        }
      }
      // Recompute running token count after deflation.
      runningTokens = 0;
      for (const auto& turn : assembled.turns) {
        for (const auto& msg : turn.messages) {
          for (const auto& part : msg.content) {
            if (const auto* txt = std::get_if<shared::TextContent>(&part)) {
              runningTokens += inputs.tokenizer->count(txt->text);
            } else if (const auto* th =
                           std::get_if<shared::ThinkingContent>(&part)) {
              runningTokens += inputs.tokenizer->count(th->thinking);
            } else if (const auto* tc =
                           std::get_if<shared::ToolCallContent>(&part)) {
              runningTokens += inputs.tokenizer->count(tc->name);
              runningTokens += inputs.tokenizer->count(tc->args);
            } else if (const auto* tr =
                           std::get_if<shared::ToolResultContent>(&part)) {
              runningTokens += inputs.tokenizer->count(tr->result);
            } else if (const auto* nt =
                           std::get_if<shared::NoticeContent>(&part)) {
              runningTokens += inputs.tokenizer->count(nt->title);
              runningTokens += inputs.tokenizer->count(nt->message);
              runningTokens += inputs.tokenizer->count(nt->details);
            } else if (const auto* er =
                           std::get_if<shared::ErrorContent>(&part)) {
              runningTokens += inputs.tokenizer->count(er->errorName);
              runningTokens += inputs.tokenizer->count(er->description);
              runningTokens += inputs.tokenizer->count(er->details);
            } else if (std::holds_alternative<shared::ImageContent>(part)) {
              runningTokens += 1024;
            }
          }
        }
      }
    }
  }

  // Compute focus-retention metrics over assembled.
  std::uint32_t userPromptsRetained = 0;
  std::uint32_t imagePartsRetained = 0;
  for (const auto& turn : assembled.turns) {
    if (turnIsUser(turn)) {
      userPromptsRetained += 1;
    }
    imagePartsRetained += imagePartsInTurn(turn);
  }

  // Tokens saved by eviction = sum of estimated tokens for evictable turns
  // that did NOT make it into the working set.
  std::uint32_t tokensSavedByEviction = 0;
  std::uint32_t evictedTurnCount = 0;
  std::vector<std::string> evictedIds;
  for (std::size_t i = 0; i < classification.decisions.size(); ++i) {
    if (!isIncluded[i] && classification.decisions[i].kind == PinKind::Evictable) {
      tokensSavedByEviction += classification.decisions[i].estimatedTokens;
      evictedTurnCount += 1;
      evictedIds.push_back(history.turns[i].turnId);
    }
  }

  report.workingSetTokens = runningTokens;
  report.userPromptsRetained = userPromptsRetained;
  report.imagePartsRetained = imagePartsRetained;
  report.pinnedTurnCount = classification.hardPinCount + classification.softPinCount;
  report.evictedTurnCount = evictedTurnCount;
  report.recalledTurnCount = static_cast<std::uint32_t>(recalledTurnIds.size());
  report.tokensSavedByEviction = tokensSavedByEviction;
  report.evictedTurnIds = std::move(evictedIds);
  report.recalledTurnIds = std::move(recalledTurnIds);
  report.hotPathLatencyMicros = nowMicros() - startMicros;
  return assembled;
}

} // namespace firmius::core::working_memory
