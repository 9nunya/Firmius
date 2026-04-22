#include "agents/RollingContextManager.hpp"

#include "ConfigLoader.hpp"
#include "Message.hpp"
#include "agents/ContextBudget.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/ProviderRegistry.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace firmius::core {

namespace {

constexpr double kApproxBytesPerToken = 4.0;

std::uint64_t nowEpochMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::uint32_t estimateTokensForText(const std::string &text) {
  if (text.empty()) {
    return 0;
  }
  return static_cast<std::uint32_t>(
      std::max(1.0, std::ceil(static_cast<double>(text.size()) /
                              kApproxBytesPerToken)));
}

std::uint32_t estimateTurnTokens(const shared::AgentTurn &turn) {
  std::uint32_t total = 0;
  for (const auto &msg : turn.messages) {
    for (const auto &part : msg.content) {
      if (const auto *txt = std::get_if<shared::TextContent>(&part)) {
        total += estimateTokensForText(txt->text);
      } else if (const auto *thinking =
                     std::get_if<shared::ThinkingContent>(&part)) {
        total += estimateTokensForText(thinking->thinking);
      } else if (const auto *toolCall =
                     std::get_if<shared::ToolCallContent>(&part)) {
        total += estimateTokensForText(toolCall->name);
        total += estimateTokensForText(toolCall->args);
      } else if (const auto *toolResult =
                     std::get_if<shared::ToolResultContent>(&part)) {
        total += estimateTokensForText(toolResult->result);
      } else if (const auto *notice =
                     std::get_if<shared::NoticeContent>(&part)) {
        total += estimateTokensForText(notice->title);
        total += estimateTokensForText(notice->message);
        total += estimateTokensForText(notice->details);
      } else if (const auto *error = std::get_if<shared::ErrorContent>(&part)) {
        total += estimateTokensForText(error->errorName);
        total += estimateTokensForText(error->description);
        total += estimateTokensForText(error->details);
      }
    }
  }
  return total;
}

std::string normalizePreset(const std::string &preset) {
  if (preset.empty()) {
    return "balanced";
  }
  std::string lowered = preset;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return lowered;
}

struct PresetProfile {
  float target;
  float buffer;
  float emergency;
  float reflection;
  float retainTail;
};

PresetProfile presetProfileFor(const shared::AgentConfig::RollingMemoryConfig &cfg) {
  const std::string preset = normalizePreset(cfg.preset);
  if (preset == "aggressive") {
    return {0.48f, 0.38f, 0.57f, 0.24f, 0.14f};
  }
  if (preset == "extended") {
    return {0.68f, 0.58f, 0.77f, 0.40f, 0.22f};
  }
  if (preset == "custom") {
    return {cfg.targetOccupancyRatio, cfg.bufferOccupancyRatio,
            cfg.emergencyOccupancyRatio, cfg.reflectionOccupancyRatio,
            cfg.retainTailRatio};
  }
  return {0.57f, 0.47f, 0.66f, 0.32f, 0.18f};
}

shared::AgentConfig::RollingModelConfig
resolveMaintenanceModel(const shared::AgentConfig &config,
                        const shared::AgentConfig::RollingModelConfig &candidate) {
  if (!candidate.enabled || candidate.providerId.empty() ||
      candidate.modelId.empty()) {
    shared::AgentConfig::RollingModelConfig fallback;
    fallback.enabled = true;
    fallback.providerId = config.providerId;
    fallback.modelId = config.modelId;
    fallback.variantName = config.modelVariant;
    return fallback;
  }
  return candidate;
}

std::shared_ptr<firmius::provider::IProvider>
resolveProvider(const shared::AgentConfig::RollingModelConfig &choice) {
  return firmius::provider::ProviderRegistry::instance().getProvider(
      choice.providerId);
}

std::string extractXmlTag(const std::string &text, const std::string &tag) {
  const std::string open = "<" + tag + ">";
  const std::string close = "</" + tag + ">";
  const auto start = text.find(open);
  if (start == std::string::npos) {
    return "";
  }
  const auto end = text.find(close, start + open.size());
  if (end == std::string::npos) {
    return "";
  }
  return shared::StringUtil::trim(
      text.substr(start + open.size(), end - (start + open.size())));
}

struct SummaryPayload {
  std::string summary;
  std::string currentTask;
  std::string suggestedResponse;
  std::string activeGoal;
  std::vector<std::string> keyActions;
  std::vector<std::string> keyToolResults;
  std::vector<std::string> openLoops;
  std::vector<std::string> filesSurfaces;
  std::vector<std::string> retrievalTags;
  std::vector<RollingMemoryAnchorRecord> anchors;
};

std::vector<std::string> extractXmlList(const std::string &text,
                                        const std::string &containerTag,
                                        const std::string &itemTag) {
  std::vector<std::string> items;
  const std::string container = extractXmlTag(text, containerTag);
  if (container.empty()) {
    return items;
  }
  const std::string open = "<" + itemTag + ">";
  const std::string close = "</" + itemTag + ">";
  std::size_t pos = 0;
  while (true) {
    const auto start = container.find(open, pos);
    if (start == std::string::npos) {
      break;
    }
    const auto end = container.find(close, start + open.size());
    if (end == std::string::npos) {
      break;
    }
    const std::string value = shared::StringUtil::trim(
        container.substr(start + open.size(), end - (start + open.size())));
    if (!value.empty()) {
      items.push_back(value);
    }
    pos = end + close.size();
  }
  return items;
}

std::vector<RollingMemoryAnchorRecord>
extractAnchors(const std::string &text) {
  std::vector<RollingMemoryAnchorRecord> anchors;
  const std::string container = extractXmlTag(text, "canonical_anchors");
  if (container.empty()) {
    return anchors;
  }
  const std::string open = "<anchor>";
  const std::string close = "</anchor>";
  std::size_t pos = 0;
  while (true) {
    const auto start = container.find(open, pos);
    if (start == std::string::npos) {
      break;
    }
    const auto end = container.find(close, start + open.size());
    if (end == std::string::npos) {
      break;
    }
    const std::string body =
        container.substr(start + open.size(), end - (start + open.size()));
    RollingMemoryAnchorRecord anchor;
    anchor.anchorType = extractXmlTag(body, "type");
    anchor.canonicalText = extractXmlTag(body, "text");
    anchor.exactQuote = extractXmlTag(body, "exact_quote");
    anchor.importance = extractXmlTag(body, "importance");
    anchor.volatility = extractXmlTag(body, "volatility");
    anchor.retrievalTags = extractXmlList(body, "retrieval_tags", "item");
    if (!anchor.canonicalText.empty()) {
      anchors.push_back(std::move(anchor));
    }
    pos = end + close.size();
  }
  return anchors;
}

SummaryPayload parseSummaryPayload(const std::string &raw) {
  SummaryPayload payload;
  payload.summary = extractXmlTag(raw, "summary");
  payload.currentTask = extractXmlTag(raw, "current_task");
  payload.suggestedResponse = extractXmlTag(raw, "suggested_response");
  payload.activeGoal = extractXmlTag(raw, "active_goal");
  payload.keyActions = extractXmlList(raw, "key_actions", "item");
  payload.keyToolResults = extractXmlList(raw, "key_tool_results", "item");
  payload.openLoops = extractXmlList(raw, "open_loops", "item");
  payload.filesSurfaces = extractXmlList(raw, "files_surfaces", "item");
  payload.retrievalTags = extractXmlList(raw, "retrieval_tags", "item");
  if (payload.summary.empty()) {
    payload.summary = shared::StringUtil::trim(raw);
  payload.anchors = extractAnchors(raw);
  }
  return payload;
}

std::string roleLabel(shared::Role role) {
  switch (role) {
  case shared::Role::System:
    return "system";
  case shared::Role::User:
    return "user";
  case shared::Role::Assistant:
    return "assistant";
  case shared::Role::ToolResult:
    return "tool_result";
  case shared::Role::Error:
    return "error";
  }
  return "unknown";
}

std::string renderTurnForPrompt(const shared::AgentTurn &turn) {
  std::ostringstream out;
  out << "## Turn " << turn.turnId << "\n";
  for (const auto &msg : turn.messages) {
    out << "[" << roleLabel(msg.role) << "] ";
    bool firstPart = true;
    for (const auto &part : msg.content) {
      if (!firstPart) {
        out << " ";
      }
      firstPart = false;
      if (const auto *txt = std::get_if<shared::TextContent>(&part)) {
        out << txt->text;
      } else if (const auto *thinking = std::get_if<shared::ThinkingContent>(&part)) {
        out << "<thinking>" << thinking->thinking << "</thinking>";
      } else if (const auto *toolCall =
                     std::get_if<shared::ToolCallContent>(&part)) {
        out << "<tool_call name=\"" << toolCall->name << "\">"
            << toolCall->args << "</tool_call>";
      } else if (const auto *toolResult =
                     std::get_if<shared::ToolResultContent>(&part)) {
        out << "<tool_result success=\"" << (toolResult->success ? "true" : "false")
            << "\">" << toolResult->result << "</tool_result>";
      } else if (const auto *notice =
                     std::get_if<shared::NoticeContent>(&part)) {
        out << notice->title << ": " << notice->message;
        if (!notice->details.empty()) {
          out << " (" << notice->details << ")";
        }
      } else if (const auto *error = std::get_if<shared::ErrorContent>(&part)) {
        out << error->errorName << ": " << error->description;
        if (!error->details.empty()) {
          out << " (" << error->details << ")";
        }
      }
    }
    out << "\n";
  }
  return out.str();
}

std::string buildWorkingMemoryPrompt(
    const std::vector<const RollingMemoryChunk *> &activeReflections,
    const std::vector<const RollingMemoryChunk *> &activeObservations,
    const std::vector<RollingMemoryAnchorRecord> &anchors) {
  std::ostringstream out;
  out << "You are Firmius working-memory bridge composer.\n"
         "Assemble a task-scoped memory packet from canonical anchors, active reflections, and active observations.\n"
         "Return XML exactly in this shape:\n"
         "<summary>...</summary>\n"
         "<active_goal>...</active_goal>\n"
         "<current_task>...</current_task>\n"
         "<suggested_response>...</suggested_response>\n"
         "<retrieval_tags><item>...</item></retrieval_tags>\n"
         "<open_loops><item>...</item></open_loops>\n"
         "<files_surfaces><item>...</item></files_surfaces>\n"
         "Prefer canonical anchors first, then reflections, then live episodes.\n"
         "Do not hallucinate missing facts; only compose from the supplied memory surfaces.\n\n";
  out << "Canonical anchors:\n";
  for (const auto &anchor : anchors) {
    out << "- [" << anchor.anchorType << "] " << anchor.canonicalText << "\n";
  }
  out << "\nActive reflections:\n";
  for (const auto *chunk : activeReflections) {
    out << "- " << chunk->summary << "\n";
  }
  out << "\nActive observations:\n";
  for (const auto *chunk : activeObservations) {
    out << "- " << chunk->summary << "\n";
  }
  return out.str();
}

std::string buildObservationPrompt(const std::vector<shared::AgentTurn> &turns) {
  std::ostringstream out;
  out << "You are Firmius rolling memory observer.\n"
         "Summarize the following older conversation segment into dense durable "
         "memory for later prompt injection.\n"
         "Preserve concrete facts, file paths, decisions, unresolved work, tool "
         "results, user preferences, and failure modes.\n"
         "Return XML exactly in this shape:\n"
         "<summary>...</summary>\n"
         "<active_goal>...</active_goal>\n"
         "<current_task>...</current_task>\n"
         "<suggested_response>...</suggested_response>\n\n"
         "<key_actions><item>...</item></key_actions>\n"
         "<key_tool_results><item>...</item></key_tool_results>\n"
         "<open_loops><item>...</item></open_loops>\n"
         "<files_surfaces><item>...</item></files_surfaces>\n"
         "<retrieval_tags><item>...</item></retrieval_tags>\n"
         "<canonical_anchors>\n"
         "  <anchor>\n"
         "    <type>objective|constraint|acceptance|decision|tool_result|blocker|preference</type>\n"
         "    <text>...</text>\n"
         "    <exact_quote>...</exact_quote>\n"
         "    <importance>critical|high|medium</importance>\n"
         "    <volatility>stable|situational</volatility>\n"
         "    <retrieval_tags><item>...</item></retrieval_tags>\n"
         "  </anchor>\n"
         "</canonical_anchors>\n"
         "Use anchors for facts that should survive aggressive compaction.\n"
         "Do not omit exact old constraints if they still govern current work.\n"
         "Prefer concise, structured extraction over elegant prose.\n"
         "Conversation segment:\n";
  for (const auto &turn : turns) {
    out << renderTurnForPrompt(turn) << "\n";
  }
  return out.str();
}

std::string buildReflectionPrompt(const std::vector<RollingMemoryChunk> &chunks) {
  std::ostringstream out;
  out << "You are Firmius rolling memory reflector.\n"
         "Condense these older rolling-memory observation chunks into one more "
         "compact summary while preserving chronology, unresolved goals, and "
         "important facts.\n"
         "Return XML exactly in this shape:\n"
         "<active_goal>...</active_goal>\n"
         "<summary>...</summary>\n"
         "<current_task>...</current_task>\n"
         "<key_actions><item>...</item></key_actions>\n"
         "<key_tool_results><item>...</item></key_tool_results>\n"
         "<open_loops><item>...</item></open_loops>\n"
         "<files_surfaces><item>...</item></files_surfaces>\n"
         "<retrieval_tags><item>...</item></retrieval_tags>\n"
         "Focus on durable invariants, repeated failures, strategic cautions, and what the actor should still believe after context shrinks.\n"
         "Do not merely paraphrase the observation summaries.\n"
         "<suggested_response>...</suggested_response>\n\n"
         "Observation chunks:\n";
  for (const auto &chunk : chunks) {
    out << "## Chunk " << chunk.chunkId << " [" << chunk.sourceStartTurnId
        << " .. " << chunk.sourceEndTurnId << "]\n";
    out << chunk.summary << "\n\n";
  }
  return out.str();
}

SummaryPayload generateSummaryForPrompt(
    const shared::AgentConfig::RollingModelConfig &choice,
    const std::string &prompt, std::atomic<bool> *abortSignal) {
  SummaryPayload payload;
  auto provider = resolveProvider(choice);
  if (!provider) {
    payload.summary = shared::StringUtil::trim(prompt);
    return payload;
  }

  shared::AgentHistory emptyHistory;
  std::string fullText;
  provider->generateSummary(
      choice.modelId, emptyHistory, prompt,
      [&](const shared::StreamEvent &ev) {
        if (const auto *txt = std::get_if<shared::TextChunk>(&ev)) {
          fullText += txt->delta;
        } else if (const auto *thinking = std::get_if<shared::ThinkingChunk>(&ev)) {
          fullText += thinking->delta;
        } else if (const auto *compText =
                       std::get_if<shared::AgentCompactionText>(&ev)) {
          fullText += compText->delta;
        } else if (const auto *compThinking =
                       std::get_if<shared::AgentCompactionThinking>(&ev)) {
          fullText += compThinking->delta;
        }
      },
      abortSignal);

  if (!fullText.empty()) {
    payload = parseSummaryPayload(fullText);
  }
  return payload;
}

bool isSummarizableTurn(const shared::AgentTurn &turn) {
  if (turn.turnId == "bootstrap-system") {
    return false;
  }
  if (turn.turnId.rfind("compaction-", 0) == 0) {
    return false;
  }
  if (turn.turnId.rfind("system-note-", 0) == 0) {
    return false;
  }

  bool hasNonSystem = false;
  for (const auto &msg : turn.messages) {
    if (msg.role != shared::Role::System) {
      hasNonSystem = true;
      break;
    }
  }
  return hasNonSystem;
}

std::unordered_set<std::string>
coveredTurnIds(const RollingMemoryState &state, bool includeBuffered) {
  std::unordered_set<std::string> covered;
  auto add = [&covered](const std::vector<RollingMemoryChunk> &chunks,
                        bool includeBufferedChunks) {
    for (const auto &chunk : chunks) {
      if (chunk.buffered && !includeBufferedChunks) {
        continue;
      }
      for (const auto &turnId : chunk.sourceTurnIds) {
        covered.insert(turnId);
      }
    }
  };
  add(state.observationChunks, includeBuffered);
  return covered;
}

std::unordered_set<std::string>
tailTurnIds(const shared::AgentHistory &history, std::uint32_t retainTailTokens) {
  std::unordered_set<std::string> keep;
  if (history.turns.empty()) {
    return keep;
  }

  std::uint32_t total = 0;
  for (auto it = history.turns.rbegin(); it != history.turns.rend(); ++it) {
    keep.insert(it->turnId);
    total += estimateTurnTokens(*it);
    if (total >= retainTailTokens && keep.size() >= 2) {
      break;
    }
  }
  keep.insert(history.turns.front().turnId);
  return keep;
}

shared::NoticeContent makeRollingNotice(
    const std::string &message, const std::string &details,
    const std::string &eventKind, const std::string &lifecycle,
    const std::optional<std::string> &modelLabel = std::nullopt,
    const std::optional<std::string> &sourceStartTurnId = std::nullopt,
    const std::optional<std::string> &sourceEndTurnId = std::nullopt,
    const std::optional<std::uint32_t> &sourceTurnCount = std::nullopt,
    const std::optional<std::uint32_t> &sourceChunkCount = std::nullopt,
    const std::optional<std::uint32_t> &sourceTokens = std::nullopt,
    const std::optional<std::uint32_t> &summaryTokens = std::nullopt,
    const std::optional<std::uint32_t> &savedTokens = std::nullopt) {
  shared::RollingNoticeMetadata metadata;
  metadata.eventKind = eventKind;
  metadata.lifecycle = lifecycle;
  metadata.modelLabel = modelLabel;
  metadata.sourceStartTurnId = sourceStartTurnId;
  metadata.sourceEndTurnId = sourceEndTurnId;
  metadata.sourceTurnCount = sourceTurnCount;
  metadata.sourceChunkCount = sourceChunkCount;
  metadata.sourceTokens = sourceTokens;
  metadata.summaryTokens = summaryTokens;
  metadata.savedTokens = savedTokens;

  return shared::NoticeContent{"Rolling memory", message, details,
                               shared::NoticeSeverity::Info, std::move(metadata)};
}

shared::AgentTurn makeRollingEventTurn(const std::string &eventId,
                                       shared::NoticeContent notice) {
  shared::AgentTurn turn;
  turn.turnId = eventId + "-" + std::to_string(nowEpochMs());
  shared::Message msg;
  msg.role = shared::Role::System;
  msg.visibility = shared::MessageVisibility::Visible;
  msg.timestamp = nowEpochMs();
  msg.content.push_back(std::move(notice));
  turn.messages.push_back(std::move(msg));
  return turn;
}

RollingMemoryBridgeRecord buildBridgeRecord(
    const RollingMemoryState &state,
    const std::vector<const RollingMemoryChunk *> &activeReflections,
    const std::vector<const RollingMemoryChunk *> &activeObservations) {
  RollingMemoryBridgeRecord bridge;
  bridge.bridgeId = "bridge-" + std::to_string(nowEpochMs());
  if (!activeReflections.empty() &&
      !activeReflections.front()->activeGoal.empty()) {
    bridge.targetTaskSignature = activeReflections.front()->activeGoal;
  } else if (!activeObservations.empty() &&
             !activeObservations.front()->activeGoal.empty()) {
    bridge.targetTaskSignature = activeObservations.front()->activeGoal;
  }
  for (const auto &anchor : state.anchors) {
    if (anchor.importance == "critical" || anchor.importance == "high") {
      bridge.relevantAnchorIds.push_back(anchor.anchorId);
    }
  }
  for (const auto *chunk : activeReflections) {
    bridge.relevantReflectionIds.push_back(chunk->chunkId);
  }
  for (const auto *chunk : activeObservations) {
    bridge.relevantEpisodeIds.push_back(chunk->chunkId);
  }
  bridge.rationale =
      "Bridge active rolling memory into a task-scoped packet using durable "
      "anchors first, then reflections, then live episodes.";
  if (!activeReflections.empty() &&
      !activeReflections.front()->suggestedResponse.empty()) {
    bridge.executionHint = activeReflections.front()->suggestedResponse;
  } else if (!activeObservations.empty() &&
             !activeObservations.front()->suggestedResponse.empty()) {
    bridge.executionHint = activeObservations.front()->suggestedResponse;
  }
  bridge.createdAt = nowEpochMs();
  return bridge;
}

std::string formatMaintenanceModelLabel(
    const shared::AgentConfig::RollingModelConfig &choice) {
  if (choice.providerId.empty() || choice.modelId.empty()) {
    return "(actor default)";
  }
  return choice.providerId + "/" + choice.modelId +
         (choice.variantName.empty() ? "" : (" (" + choice.variantName + ")"));
}

RollingMemoryState loadState(const shared::AgentContext &context) {
  RollingMemoryState state;
  if (!context.history || context.history->threadId.empty() ||
      context.identity.id.empty()) {
    return state;
  }
  try {
    ThreadManager tm(ThreadManager::defaultBasePath());
    state = tm.loadRollingMemoryState(context.history->threadId,
                                      context.identity.id);
  } catch (...) {
  }
  return state;
}

void saveState(const shared::AgentContext &context, const RollingMemoryState &state) {
  if (!context.history || context.history->threadId.empty() ||
      context.identity.id.empty()) {
    return;
  }
  try {
    ThreadManager tm(ThreadManager::defaultBasePath());
    tm.writeRollingMemoryState(context.history->threadId, context.identity.id,
                               state);
  } catch (...) {
  }
}

std::vector<shared::AgentTurn>
selectObservationTurns(const shared::AgentHistory &history,
                       const std::unordered_set<std::string> &coveredIds,
                       const std::unordered_set<std::string> &tailIds,
                       std::uint32_t desiredTokens) {
  std::vector<shared::AgentTurn> selected;
  std::uint32_t total = 0;
  for (const auto &turn : history.turns) {
    if (!isSummarizableTurn(turn)) {
      continue;
    }
    if (coveredIds.count(turn.turnId) > 0) {
      continue;
    }
    if (tailIds.count(turn.turnId) > 0) {
      continue;
    }
    selected.push_back(turn);
    total += estimateTurnTokens(turn);
    if (total >= desiredTokens) {
      break;
    }
  }
  return selected;
}

std::uint32_t totalSummaryTokens(const std::vector<RollingMemoryChunk> &chunks,
                                 bool requireActive) {
  std::uint32_t total = 0;
  for (const auto &chunk : chunks) {
    if (requireActive && !chunk.active) {
      continue;
    }
    if (chunk.superseded) {
      continue;
    }
    total += chunk.summaryTokens;
  }
  return total;
}

} // namespace

bool RollingContextManager::isEnabled(const shared::AgentContext &context) {
  if (!context.config.rollingMemory.enabled) {
    return false;
  }
  if (context.config.rollingMemory.mode == "legacy_compaction" ||
      context.config.rollingMemory.mode == "disabled") {
    return false;
  }
  return true;
}

ResolvedRollingThresholds
RollingContextManager::resolveThresholds(const shared::AgentContext &context) {
  ResolvedRollingThresholds resolved;
  resolved.enabled = isEnabled(context);
  resolved.preset = normalizePreset(context.config.rollingMemory.preset);
  if (!resolved.enabled) {
    return resolved;
  }

  std::uint32_t contextWindow = 0;
  auto provider = firmius::provider::ProviderRegistry::instance().getProvider(
      context.config.providerId);
  if (provider) {
    try {
      contextWindow = provider->getModelInfo(context.config.modelId).contextWindow;
    } catch (...) {
    }
  }
  if (contextWindow == 0) {
    contextWindow = 128000;
  }

  const auto profile = presetProfileFor(context.config.rollingMemory);
  resolved.contextWindow = contextWindow;
  resolved.targetOccupancyRatio = profile.target;
  resolved.bufferOccupancyRatio = std::min(profile.buffer, profile.target - 0.02f);
  resolved.emergencyOccupancyRatio = std::max(profile.emergency, profile.target + 0.05f);
  resolved.reflectionOccupancyRatio = profile.reflection;
  resolved.retainTailRatio = profile.retainTail;
  resolved.bufferThresholdTokens = static_cast<std::uint32_t>(
      std::floor(static_cast<double>(contextWindow) *
                 resolved.bufferOccupancyRatio));
  resolved.targetThresholdTokens = static_cast<std::uint32_t>(
      std::floor(static_cast<double>(contextWindow) *
                 resolved.targetOccupancyRatio));
  resolved.emergencyThresholdTokens = static_cast<std::uint32_t>(
      std::floor(static_cast<double>(contextWindow) *
                 resolved.emergencyOccupancyRatio));
  resolved.reflectionThresholdTokens = std::max<std::uint32_t>(
      context.config.rollingMemory.minimumChunkTokens,
      static_cast<std::uint32_t>(
          std::floor(static_cast<double>(resolved.targetThresholdTokens) *
                     resolved.reflectionOccupancyRatio)));
  resolved.retainedTailTokens = std::max<std::uint32_t>(
      context.config.rollingMemory.minimumRetainedTailTokens,
      static_cast<std::uint32_t>(
          std::floor(static_cast<double>(contextWindow) *
                     resolved.retainTailRatio)));
  resolved.minimumChunkTokens = std::max<std::uint32_t>(
      context.config.rollingMemory.minimumChunkTokens,
      resolved.targetThresholdTokens / 8);
  return resolved;
}

void RollingContextManager::maintain(shared::AgentContext &context,
                                     firmius::provider::IProvider &actorProvider,
                                     PersistTurnFn persistTurn,
                                     std::atomic<bool> *abortSignal) {
  (void)actorProvider;
  if (!isEnabled(context) || !context.history) {
    return;
  }

  const auto thresholds = resolveThresholds(context);
  if (!thresholds.enabled || thresholds.contextWindow == 0) {
    return;
  }

  auto state = loadState(context);
  state.threadId = context.history->threadId;
  state.agentId = context.identity.id;
  state.lastContextWindow = thresholds.contextWindow;
  state.lastBufferThresholdTokens = thresholds.bufferThresholdTokens;
  state.lastTargetThresholdTokens = thresholds.targetThresholdTokens;
  state.lastEmergencyThresholdTokens = thresholds.emergencyThresholdTokens;
  state.lastRetainedTailTokens = thresholds.retainedTailTokens;

  const std::uint32_t currentContext = context.aggregateMetrics.tokens.contextSize;
  const auto coveredIds = coveredTurnIds(state, false);
  const auto tailIds =
      tailTurnIds(*context.history, thresholds.retainedTailTokens);

  const auto maybePersistEvent = [&](const std::string &suffix,
                                     shared::NoticeContent notice) {
    if (!context.config.rollingMemory.emitEventTurns || !persistTurn) {
      return;
    }
    persistTurn(makeRollingEventTurn("rolling-" + suffix, std::move(notice)));
  };

  const auto createObservationChunk = [&](bool activateImmediately) {
    const auto fullCovered = coveredTurnIds(state, true);
    const std::uint32_t overflow =
        currentContext > thresholds.targetThresholdTokens
            ? currentContext - thresholds.targetThresholdTokens
            : std::max<std::uint32_t>(
                  thresholds.minimumChunkTokens,
                  thresholds.targetThresholdTokens > thresholds.bufferThresholdTokens
                      ? thresholds.targetThresholdTokens -
                            thresholds.bufferThresholdTokens
                      : thresholds.minimumChunkTokens);
    const std::uint32_t desiredChunkTokens =
        std::max<std::uint32_t>(thresholds.minimumChunkTokens, overflow);
    const auto selectedTurns = selectObservationTurns(*context.history, fullCovered,
                                                      tailIds, desiredChunkTokens);
    if (selectedTurns.empty()) {
      return false;
    }

    std::uint32_t sourceTokens = 0;
    for (const auto &turn : selectedTurns) {
      sourceTokens += estimateTurnTokens(turn);
    }
    if (sourceTokens < thresholds.minimumChunkTokens &&
        currentContext < thresholds.emergencyThresholdTokens) {
      return false;
    }

    const auto observerChoice =
        resolveMaintenanceModel(context.config, context.config.rollingMemory.observer);
    const std::string observerModelLabel =
        formatMaintenanceModelLabel(observerChoice);
    maybePersistEvent(
        "buffer-start",
        makeRollingNotice(
            "Buffering rolling memory using " + observerModelLabel + ".", "",
            "observation", "start", observerModelLabel,
            selectedTurns.front().turnId, selectedTurns.back().turnId,
            static_cast<std::uint32_t>(selectedTurns.size()), std::nullopt,
            sourceTokens));

    state.observationInFlight = true;
    saveState(context, state);
    const auto payload = generateSummaryForPrompt(observerChoice,
                                                  buildObservationPrompt(selectedTurns),
                                                  abortSignal);

    if (payload.summary.empty()) {
      state.observationInFlight = false;
      saveState(context, state);
      maybePersistEvent(
          "buffer-empty",
          makeRollingNotice(
              "Rolling memory observation returned no summary.",
              "Observer did not produce summary text for selected turns.",
              "observation", "empty", observerModelLabel,
              selectedTurns.front().turnId, selectedTurns.back().turnId,
              static_cast<std::uint32_t>(selectedTurns.size()), std::nullopt,
              sourceTokens));
      return false;
    }

    RollingMemoryChunk chunk;
    chunk.chunkId = "obs-" + std::to_string(nowEpochMs());
    chunk.sourceStartTurnId = selectedTurns.front().turnId;
    chunk.sourceEndTurnId = selectedTurns.back().turnId;
    chunk.summary = payload.summary;
    chunk.currentTask = payload.currentTask;
    chunk.suggestedResponse = payload.suggestedResponse;
    chunk.sourceTokens = sourceTokens;
    chunk.activeGoal = payload.activeGoal;
    chunk.keyActions = payload.keyActions;
    chunk.keyToolResults = payload.keyToolResults;
    chunk.openLoops = payload.openLoops;
    chunk.filesSurfaces = payload.filesSurfaces;
    chunk.retrievalTags = payload.retrievalTags;
    chunk.summaryTokens = estimateTokensForText(chunk.summary);
    chunk.createdAt = nowEpochMs();
    chunk.buffered = !activateImmediately;
    chunk.active = activateImmediately;
    for (const auto &turn : selectedTurns) {
      chunk.sourceTurnIds.push_back(turn.turnId);
    }
    state.lastObservedTurnId = chunk.sourceEndTurnId;
    state.observationChunks.push_back(std::move(chunk));
    const auto &createdChunk = state.observationChunks.back();
    for (std::size_t i = 0; i < payload.anchors.size(); ++i) {
      auto anchor = payload.anchors[i];
      if (anchor.anchorId.empty()) {
        anchor.anchorId = state.observationChunks.back().chunkId + "-anchor-" +
                          std::to_string(i + 1);
      }
      if (anchor.sourceTurnIds.empty()) {
        anchor.sourceTurnIds = state.observationChunks.back().sourceTurnIds;
      }
      state.anchors.push_back(std::move(anchor));
      state.observationChunks.back().anchorIds.push_back(
          state.anchors.back().anchorId);
    }

    state.observationInFlight = false;
    saveState(context, state);

    maybePersistEvent(
        activateImmediately ? "activate" : "buffer-complete",
        makeRollingNotice(
            std::string(activateImmediately ? "Activated" : "Buffered") +
                " rolling memory for turns " + selectedTurns.front().turnId + " .. " +
                selectedTurns.back().turnId + ".",
            "",
            "observation",
            activateImmediately ? "activate" : "complete",
            observerModelLabel,
            createdChunk.sourceStartTurnId,
            createdChunk.sourceEndTurnId,
            static_cast<std::uint32_t>(createdChunk.sourceTurnIds.size()),
            std::nullopt,
            createdChunk.sourceTokens,
            createdChunk.summaryTokens,
            createdChunk.sourceTokens > createdChunk.summaryTokens
                ? std::optional<std::uint32_t>(createdChunk.sourceTokens -
                                               createdChunk.summaryTokens)
                : std::optional<std::uint32_t>(0u)));
    return true;
  };

  if (currentContext >= thresholds.bufferThresholdTokens) {
    bool hasBuffered = std::any_of(
        state.observationChunks.begin(), state.observationChunks.end(),
        [](const auto &chunk) { return chunk.buffered && !chunk.superseded; });
    if (!hasBuffered) {
      createObservationChunk(false);
    }
  }

  if (currentContext >= thresholds.targetThresholdTokens) {
    bool activatedAny = false;
    std::size_t activatedCount = 0;
    std::optional<std::string> activatedStart;
    std::optional<std::string> activatedEnd;
    for (auto &chunk : state.observationChunks) {
      if (chunk.buffered && !chunk.superseded) {
        chunk.buffered = false;
        chunk.active = true;
        activatedAny = true;
        ++activatedCount;
        if (!activatedStart.has_value() || chunk.sourceStartTurnId < *activatedStart) {
          activatedStart = chunk.sourceStartTurnId;
        }
        if (!activatedEnd.has_value() || chunk.sourceEndTurnId > *activatedEnd) {
          activatedEnd = chunk.sourceEndTurnId;
        }
      }
    }
    if (!activatedAny && currentContext >= thresholds.emergencyThresholdTokens) {
      activatedAny = createObservationChunk(true);
    }
    if (activatedAny) {
      maybePersistEvent(
          "activate",
          makeRollingNotice(
              "Rolling memory activation reduced raw prompt pressure while preserving "
              "the append-only transcript.",
              "", "observation", "activate", std::nullopt,
              activatedStart, activatedEnd,
              activatedCount > 0
                  ? std::optional<std::uint32_t>(
                        static_cast<std::uint32_t>(activatedCount))
                  : std::nullopt,
              activatedCount > 0
                  ? std::optional<std::uint32_t>(
                        static_cast<std::uint32_t>(activatedCount))
                  : std::nullopt));
    }
  }

  std::vector<RollingMemoryChunk *> activeObservationPtrs;
  for (auto &chunk : state.observationChunks) {
    if (chunk.active && !chunk.superseded) {
      activeObservationPtrs.push_back(&chunk);
    }
  }

  if (activeObservationPtrs.size() >= 2 &&
      totalSummaryTokens(state.observationChunks, true) >=
          thresholds.reflectionThresholdTokens) {
    std::vector<RollingMemoryChunk> toReflect;
    std::uint32_t total = 0;
    for (std::size_t i = 0; i + 1 < activeObservationPtrs.size(); ++i) {
      toReflect.push_back(*activeObservationPtrs[i]);
      total += activeObservationPtrs[i]->summaryTokens;
      if (total >= thresholds.reflectionThresholdTokens) {
        break;
      }
    }
    if (toReflect.size() >= 2) {
      const auto reflectorChoice = resolveMaintenanceModel(
          context.config, context.config.rollingMemory.reflector);
      const std::string reflectorModelLabel =
          formatMaintenanceModelLabel(reflectorChoice);
      maybePersistEvent(
          "reflect-start",
          makeRollingNotice(
              "Reflecting older rolling-memory observations using " +
                  reflectorModelLabel + ".",
              "", "reflection", "start", reflectorModelLabel,
              toReflect.front().sourceStartTurnId,
              toReflect.back().sourceEndTurnId,
              std::nullopt,
              static_cast<std::uint32_t>(toReflect.size()),
              total));

      state.reflectionInFlight = true;
      saveState(context, state);
      const auto payload = generateSummaryForPrompt(
          reflectorChoice, buildReflectionPrompt(toReflect), abortSignal);

      if (payload.summary.empty()) {
        state.reflectionInFlight = false;
        saveState(context, state);
        maybePersistEvent(
            "reflect-empty",
            makeRollingNotice(
                "Rolling memory reflection returned no summary.",
                "Reflector did not produce summary text for selected chunks.",
                "reflection", "empty", reflectorModelLabel,
                toReflect.front().sourceStartTurnId,
                toReflect.back().sourceEndTurnId,
                std::nullopt,
                static_cast<std::uint32_t>(toReflect.size()),
                total));
      } else {
        RollingMemoryChunk reflection;
        reflection.chunkId = "refl-" + std::to_string(nowEpochMs());
        reflection.sourceStartTurnId = toReflect.front().sourceStartTurnId;
        reflection.sourceEndTurnId = toReflect.back().sourceEndTurnId;
        reflection.summary = payload.summary;
        reflection.currentTask = payload.currentTask;
        reflection.suggestedResponse = payload.suggestedResponse;
        reflection.activeGoal = payload.activeGoal;
        reflection.keyActions = payload.keyActions;
        reflection.keyToolResults = payload.keyToolResults;
        reflection.openLoops = payload.openLoops;
        reflection.filesSurfaces = payload.filesSurfaces;
        reflection.retrievalTags = payload.retrievalTags;
        reflection.sourceTokens = total;
        reflection.summaryTokens = estimateTokensForText(reflection.summary);
        reflection.createdAt = nowEpochMs();
        reflection.active = true;
        for (const auto &chunk : toReflect) {
          reflection.sourceTurnIds.insert(reflection.sourceTurnIds.end(),
                                          chunk.sourceTurnIds.begin(),
                                          chunk.sourceTurnIds.end());
        }
        for (const auto &chunk : toReflect) {
          reflection.derivedFromChunkIds.push_back(chunk.chunkId);
          reflection.anchorIds.insert(reflection.anchorIds.end(), chunk.anchorIds.begin(), chunk.anchorIds.end());
        }
        for (auto &chunk : state.observationChunks) {
          if (std::find_if(toReflect.begin(), toReflect.end(),
                           [&](const auto &candidate) {
                             return candidate.chunkId == chunk.chunkId;
                           }) != toReflect.end()) {
            chunk.active = false;
            chunk.superseded = true;
          }
        }
        state.lastReflectedObservationId = reflection.chunkId;
        state.reflectionChunks.push_back(std::move(reflection));
        const auto &createdReflection = state.reflectionChunks.back();

        state.reflectionInFlight = false;
        saveState(context, state);

        maybePersistEvent(
            "reflect-complete",
            makeRollingNotice(
                "Reflected older rolling-memory observations into a denser summary "
                "chunk.",
                "", "reflection", "complete", reflectorModelLabel,
                createdReflection.sourceStartTurnId,
                createdReflection.sourceEndTurnId,
                std::nullopt,
                static_cast<std::uint32_t>(toReflect.size()),
                createdReflection.sourceTokens,
                createdReflection.summaryTokens,
                createdReflection.sourceTokens > createdReflection.summaryTokens
                    ? std::optional<std::uint32_t>(createdReflection.sourceTokens -
                                                   createdReflection.summaryTokens)
                    : std::optional<std::uint32_t>(0u)));
      }
    }
  }


  if (!state.observationChunks.empty() || !state.reflectionChunks.empty()) {
    std::vector<const RollingMemoryChunk *> activeReflectionPtrs;
    std::vector<const RollingMemoryChunk *> activeObservationViewPtrs;
    for (const auto &chunk : state.reflectionChunks) {
      if (chunk.active && !chunk.superseded) activeReflectionPtrs.push_back(&chunk);
    }
    for (const auto &chunk : state.observationChunks) {
      if (chunk.active && !chunk.superseded) activeObservationViewPtrs.push_back(&chunk);
    }
    const auto updaterChoice = resolveMaintenanceModel(
        context.config, context.config.rollingMemory.workingMemoryUpdater);
    SummaryPayload bridgePayload = generateSummaryForPrompt(
        updaterChoice,
        buildWorkingMemoryPrompt(activeReflectionPtrs, activeObservationViewPtrs,
                                 state.anchors),
        abortSignal);
    RollingMemoryBridgeRecord bridge = buildBridgeRecord(
        state, activeReflectionPtrs, activeObservationViewPtrs);
    if (!bridgePayload.activeGoal.empty()) {
      bridge.targetTaskSignature = bridgePayload.activeGoal;
    }
    if (!bridgePayload.suggestedResponse.empty()) {
      bridge.executionHint = bridgePayload.suggestedResponse;
    }
    if (!bridgePayload.currentTask.empty() && bridge.rationale.empty()) {
      bridge.rationale = bridgePayload.currentTask;
    }
    state.bridgeInFlight = true;
    if (!state.lastBridgeId.empty() && !state.bridges.empty() &&
        state.bridges.back().bridgeId == state.lastBridgeId) {
      state.lastBridgeId = bridge.bridgeId;
      state.bridges.back() = std::move(bridge);
    } else {
      state.lastBridgeId = bridge.bridgeId;
      state.bridges.push_back(std::move(bridge));
    }
    state.bridgeInFlight = false;
  }
  saveState(context, state);
}

shared::AgentHistory
RollingContextManager::filterHistoryForRequest(const shared::AgentContext &context,
                                               const shared::AgentHistory &history) {
  if (!isEnabled(context)) {
    return history;
  }

  const auto thresholds = resolveThresholds(context);
  auto state = loadState(context);
  const auto keepTail = tailTurnIds(history, thresholds.retainedTailTokens);
  const auto covered = coveredTurnIds(state, false);

  // First pass: determine which turns to skip.
  std::vector<bool> skip(history.turns.size(), false);
  for (std::size_t i = 0; i < history.turns.size(); ++i) {
    const auto &turn = history.turns[i];
    if (covered.count(turn.turnId) > 0 && keepTail.count(turn.turnId) == 0) {
      skip[i] = true;
    }
  }

  // Second pass: preserve tool_use / tool_result pairing.
  // Build maps: turnIndex -> set of toolCallIds it provides (tool_use) or
  // requires (tool_result). Then un-skip any turn whose partner is kept.
  // Maps: toolCallId -> index of the turn that has the ToolCallContent for it.
  std::unordered_map<std::string, std::size_t> toolCallIdToTurnIndex;
  // Maps: toolCallId -> index of the turn that has the ToolResultContent for it.
  std::unordered_map<std::string, std::size_t> toolResultIdToTurnIndex;

  for (std::size_t i = 0; i < history.turns.size(); ++i) {
    for (const auto &msg : history.turns[i].messages) {
      for (const auto &part : msg.content) {
        if (const auto *tc = std::get_if<shared::ToolCallContent>(&part)) {
          if (!tc->id.empty()) {
            toolCallIdToTurnIndex[tc->id] = i;
          }
        } else if (const auto *tr =
                       std::get_if<shared::ToolResultContent>(&part)) {
          if (!tr->toolCallId.empty()) {
            toolResultIdToTurnIndex[tr->toolCallId] = i;
          }
        }
      }
    }
  }

  // Iteratively un-skip turns until stable to handle cascading deps.
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t i = 0; i < history.turns.size(); ++i) {
      if (skip[i]) {
        continue; // this turn is being removed, skip checks
      }
      // This turn is kept. Check if it has tool_result that needs a tool_use.
      for (const auto &msg : history.turns[i].messages) {
        for (const auto &part : msg.content) {
          if (const auto *tr =
                   std::get_if<shared::ToolResultContent>(&part)) {
            if (!tr->toolCallId.empty()) {
              auto it = toolCallIdToTurnIndex.find(tr->toolCallId);
              if (it != toolCallIdToTurnIndex.end() && skip[it->second]) {
                skip[it->second] = false;
                changed = true;
              }
            }
          } else if (const auto *tc =
                         std::get_if<shared::ToolCallContent>(&part)) {
            if (!tc->id.empty()) {
              auto it = toolResultIdToTurnIndex.find(tc->id);
              if (it != toolResultIdToTurnIndex.end() && skip[it->second]) {
                skip[it->second] = false;
                changed = true;
              }
            }
          }

        }
      }
    }
  }

  shared::AgentHistory filtered;
  filtered.threadId = history.threadId;
  for (std::size_t i = 0; i < history.turns.size(); ++i) {
    if (!skip[i]) {
      filtered.turns.push_back(history.turns[i]);
    }
  }
  return filtered;
}

std::string RollingContextManager::buildMemoryOverlay(
    const shared::AgentContext &context) {
  if (!isEnabled(context)) {
    return "";
  }

  const auto state = loadState(context);
  std::vector<const RollingMemoryChunk *> activeReflections;
  std::vector<const RollingMemoryChunk *> activeObservations;
  for (const auto &chunk : state.reflectionChunks) {
    if (chunk.active && !chunk.superseded) {
      activeReflections.push_back(&chunk);
    }
  }
  for (const auto &chunk : state.observationChunks) {
    if (chunk.active && !chunk.superseded) {
      activeObservations.push_back(&chunk);
    }
  }
  if (activeReflections.empty() && activeObservations.empty() &&
      state.anchors.empty() && state.bridges.empty()) {
    return "";
  }

  const RollingMemoryBridgeRecord *latestBridge = nullptr;
  if (!state.bridges.empty()) {
    latestBridge = &state.bridges.back();
  }

  std::ostringstream out;
  out << "## ROLLING MEMORY\n";

  if (latestBridge != nullptr) {
    out << "\nWorking-memory bridge\n";
    if (!latestBridge->targetTaskSignature.empty()) {
      out << "- target: " << latestBridge->targetTaskSignature << "\n";
    }
    if (!latestBridge->bridgeId.empty()) {
      out << "- bridge_id: " << latestBridge->bridgeId << "\n";
    }
    out << "- anchors: " << latestBridge->relevantAnchorIds.size() << "\n";
    out << "- episodes: " << latestBridge->relevantEpisodeIds.size() << "\n";
    out << "- reflections: " << latestBridge->relevantReflectionIds.size() << "\n";
    if (!latestBridge->executionHint.empty()) out << "- hint: " << latestBridge->executionHint << "\n";
    if (!latestBridge->rationale.empty()) out << "- rationale: " << latestBridge->rationale << "\n";
  }
  if (!state.anchors.empty()) {
    out << "Canonical anchors\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(5, state.anchors.size()); ++i) {
      const auto &anchor = state.anchors[i];
      out << "- [" << (anchor.anchorType.empty() ? "anchor" : anchor.anchorType)
          << "] " << anchor.canonicalText;
      if (!anchor.importance.empty()) out << " {importance=" << anchor.importance << "}";
      if (!anchor.volatility.empty()) out << " {volatility=" << anchor.volatility << "}";
      if (!anchor.sourceTurnIds.empty()) out << " {turns=" << anchor.sourceTurnIds.front()
                                             << (anchor.sourceTurnIds.size() > 1 ? ".." + anchor.sourceTurnIds.back() : "") << "}";
      out << "\n";
      if (!anchor.exactQuote.empty()) out << "  quote: " << anchor.exactQuote << "\n";
    }
    out << "\n";
  }

  if (!activeReflections.empty()) {
    out << "Active reflections\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(2, activeReflections.size()); ++i) {
      const auto *chunk = activeReflections[i];
      out << "- [" << chunk->sourceStartTurnId << " .. "
          << chunk->sourceEndTurnId << "] " << chunk->summary;
      if (!chunk->activeGoal.empty()) {
        out << " (goal: " << chunk->activeGoal << ")";
      }
      out << "\n";
    }
    out << "\n";
  }

  if (!activeObservations.empty()) {
    out << "Active observations\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(3, activeObservations.size()); ++i) {
      const auto *chunk = activeObservations[i];
      out << "- [" << chunk->sourceStartTurnId << " .. "
          << chunk->sourceEndTurnId << "] " << chunk->summary;
      if (!chunk->activeGoal.empty()) {
        out << " (goal: " << chunk->activeGoal << ")";
      }
      out << "\n";
    }
  }

  return out.str();
}

std::string RollingContextManager::buildStatusOverlay(
    const shared::AgentContext &context) {
  if (!isEnabled(context)) {
    return "";
  }

  const auto thresholds = resolveThresholds(context);
  const auto state = loadState(context);
  std::size_t bufferedObservations = 0;
  const std::size_t criticalAnchors = std::count_if(state.anchors.begin(), state.anchors.end(), [](const auto &a) { return a.importance == "critical"; });
  std::size_t activeObservations = 0;
  std::size_t activeReflections = 0;
  std::string continuationHint;
  for (const auto &chunk : state.observationChunks) {
    if (chunk.buffered && !chunk.superseded) {
      ++bufferedObservations;
    }
    if (chunk.active && !chunk.superseded) {
      ++activeObservations;
      if (continuationHint.empty() && !chunk.suggestedResponse.empty()) {
        continuationHint = chunk.suggestedResponse;
      }
      if (continuationHint.empty() && !chunk.activeGoal.empty()) {
        continuationHint = chunk.activeGoal;
      }
    }
  }
  for (const auto &chunk : state.reflectionChunks) {
    if (chunk.active && !chunk.superseded) {
      ++activeReflections;
      if (continuationHint.empty() && !chunk.suggestedResponse.empty()) {
        continuationHint = chunk.suggestedResponse;
      }
    }
  }

  std::ostringstream out;
  out << "## ROLLING MEMORY STATUS\n";
  out << "Preset: " << thresholds.preset << "\n";
  out << "Context window: " << thresholds.contextWindow << "\n";
  out << "Critical anchors: " << criticalAnchors << "\n";
  out << "Buffer threshold: " << thresholds.bufferThresholdTokens << "\n";
  out << "Target threshold: " << thresholds.targetThresholdTokens << "\n";
  out << "Emergency threshold: " << thresholds.emergencyThresholdTokens << "\n";
  out << "Retained raw tail: " << thresholds.retainedTailTokens << "\n";
  out << "Observer: " << formatMaintenanceModelLabel(resolveMaintenanceModel(
                                 context.config, context.config.rollingMemory.observer))
      << "\n";
  out << "Reflector: " << formatMaintenanceModelLabel(resolveMaintenanceModel(
                                  context.config, context.config.rollingMemory.reflector))
      << "\n";
  out << "Active reflections: " << activeReflections << "\n";
  out << "Active observations: " << activeObservations << "\n";
  out << "Buffered observations: " << bufferedObservations << "\n";
  out << "Canonical anchors: " << state.anchors.size() << "\n";
  out << "Bridge packets: " << state.bridges.size() << "\n";
  if (!state.lastBridgeId.empty()) out << "Latest bridge: " << state.lastBridgeId << "\n";
  if (state.bridgeInFlight) out << "Bridge in flight: yes\n";
  if (!continuationHint.empty()) {
    out << "Suggested continuation: " << continuationHint << "\n";
  }
  return out.str();
}

} // namespace firmius::core
