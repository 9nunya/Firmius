#include "ActivePlanState.hpp"
#include "TUIState.hpp"
#include "NotificationManager.hpp"

#include "ThemeManager.hpp"
#include "UserPreferences.hpp"
#include "ClaudexPhrases.hpp"
#include "utils/ClaudexActivity.hpp"
#include "components/AgentStrip.hpp"
#include "components/ChatWindow.hpp"
#include "components/ContextLane.hpp"
#include "components/InputBar.hpp"
#include "components/LiveStatusRow.hpp"
#include "components/Markdown.hpp"
#include "components/HelpOverlay.hpp"
#include "components/PlanLane.hpp"
#include "components/StatusBar.hpp"
#include "components/TodoLane.hpp"
#include "agents/ContextBudget.hpp"
#include "agents/modes/Mode.hpp"
#include "agents/PurposeLoader.hpp"
#include "agents/RollingContextManager.hpp"
#include "AgentRegistry.hpp"
#include "components/StatusBar.hpp"
#include "components/TitleBar.hpp"
#include "components/TranscriptGrouping.hpp"
#include "components/ToolBlock.hpp"
#include "components/ErrorDisplay.hpp"
#include "harness/Harness.hpp"
#include "utils/ReferenceAutocomplete.hpp"
#include "utils/ModeCycle.hpp"
#include "modals/ModalRegistry.hpp"
#include "modals/ThreadLockedModal.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/BaseAPIKeyProvider.hpp"
#include "providers/BaseOAuthProvider.hpp"
#include "providers/ProviderRegistry.hpp"
#include "utils/Icons.hpp"
#include "utils/PlatformPaths.hpp"
#include "commands/ICommand.hpp"
#include "commands/CommandManager.hpp"
#include "TUIHotkeys.hpp"
#include "UIState.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/color_info.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <thread>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <cstdint>

namespace firmius::tui {

using namespace firmius::shared;


namespace {
bool parseTuiStartupProfilingEnabledFromEnv() {
  const char *raw = std::getenv("FIRMIUS_TUI_STARTUP_PROFILE");
  if (!raw) {
    return false;
  }
  std::string value(raw);
  for (char &ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (value.empty() || value == "0" || value == "false" || value == "off" ||
      value == "no") {
    return false;
  }
  return true;
}
} // namespace

TuiProfilingStats &tuiProfilingStats() {
  static TuiProfilingStats stats;
  return stats;
}

double nanosToMillis(int64_t nanos) {
  return static_cast<double>(nanos) / 1000000.0;
}

template <typename T>
void HashCombine(std::size_t &seed, const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

std::string truncateText(const std::string &text, int max_len) {
  if (max_len <= 0)
    return "";
  if (static_cast<int>(text.size()) <= max_len)
    return text;
  if (max_len <= 3)
    return text.substr(0, max_len);
  return text.substr(0, max_len - 3) + "...";
}

int workChunkRank(firmius::shared::WorkChunkStatus status) {
  using firmius::shared::WorkChunkStatus;
  switch (status) {
  case WorkChunkStatus::InProgress:
    return 0;
  case WorkChunkStatus::Verifying:
    return 1;
  case WorkChunkStatus::Ready:
    return 2;
  case WorkChunkStatus::Implemented:
    return 3;
  case WorkChunkStatus::Done:
    return 4;
  case WorkChunkStatus::Blocked:
    return 5;
  case WorkChunkStatus::Failed:
    return 6;
  case WorkChunkStatus::Cancelled:
    return 7;
  }
  return 99;
}

uint64_t nextLiveRowRng(uint64_t &state) {
  state = state * 6364136223846793005ULL + 1442695040888963407ULL;
  return state;
}

std::chrono::seconds nextLiveRowPhraseDelay(uint64_t &state) {
  const uint64_t value = nextLiveRowRng(state);
  return std::chrono::seconds(15 + static_cast<int>(value % 16));
}

std::chrono::seconds minimumLiveRowPhraseVisibleDuration() {
  return std::chrono::seconds(6);
}

std::chrono::milliseconds liveRowTransitionDurationForPhrases(
    const std::string &previous_phrase, const std::string &next_phrase) {
  const auto max_len =
      std::max(previous_phrase.size(), next_phrase.size());
  return std::chrono::milliseconds(
      450 + static_cast<int>(std::min<std::size_t>(max_len, 80) * 22));
}

std::string pickLiveRowPhrase(const std::vector<std::string> &phrases,
                              const std::string &current, uint64_t &state) {
  if (phrases.empty()) {
    return "Standing by.";
  }
  if (phrases.size() == 1) {
    return phrases.front();
  }
  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto index =
        static_cast<std::size_t>(nextLiveRowRng(state) % phrases.size());
    if (phrases[index] != current) {
      return phrases[index];
    }
  }
  for (const auto &phrase : phrases) {
    if (phrase != current) {
      return phrase;
    }
  }
  return phrases.front();
}

void queueLiveRowTransition(std::string phrase_key, std::string phrase,
                            std::string &pending_key,
                            std::string &pending_phrase) {
  pending_key = std::move(phrase_key);
  pending_phrase = std::move(phrase);
}

std::size_t buildFocusedChatLiveMeasurementSignature(
    const firmius::tui::StreamStateManager &stream_state,
    const std::string &focused_agent_id, const std::string &thread_id,
    const std::unordered_set<std::string> &persisted_tool_call_ids) {
  std::size_t signature = 0;
  HashCombine(signature, focused_agent_id);
  HashCombine(signature, persisted_tool_call_ids.size());
  HashCombine(signature, thread_id);

  auto bucket = [](std::size_t size) { return size / 512; };

  if (const auto *s = stream_state.getStream(focused_agent_id)) {
    HashCombine(signature, bucket(s->thinking.size()));
    HashCombine(signature, bucket(s->text.size()));
    HashCombine(signature, bucket(s->compaction_thinking.size()));
    HashCombine(signature, bucket(s->compaction_text.size()));
    HashCombine(signature, bucket(s->compaction_completion.size()));
    HashCombine(signature, s->compaction_active);
    HashCombine(signature, s->compaction_finished);
    HashCombine(signature, s->provider_waiting);
  }

  std::size_t focused_timeline = 0;
  for (const auto &entry : stream_state.getTimeline()) {
    if (entry.agentId == focused_agent_id) {
      focused_timeline++;
      HashCombine(signature, static_cast<int>(entry.kind));
    }
  }
  HashCombine(signature, focused_timeline);

  std::size_t focused_tool_calls = 0;
  for (const auto &[id, view] : stream_state.getToolCalls()) {
    (void)id;
    if (view && view->agentId == focused_agent_id) {
      focused_tool_calls++;
      HashCombine(signature, static_cast<int>(view->phase));
      HashCombine(signature, view->success);
    }
  }
  HashCombine(signature, focused_tool_calls);

  // Hash queued + internal-queued messages so the chat invalidates when the
  // operator queues messages while a turn is in flight (matches 08fd932).
  const auto bucketQueued = [](std::size_t size) { return size / 128; };
  std::size_t queued_count = 0;
  for (const auto &entry : stream_state.getQueuedMessages()) {
    if ((!entry.agent_id.empty() && entry.agent_id != focused_agent_id) ||
        (!entry.thread_id.empty() && entry.thread_id != thread_id)) {
      continue;
    }
    queued_count++;
    HashCombine(signature, bucketQueued(entry.text.size()));
    HashCombine(signature, entry.image_count);
  }
  HashCombine(signature, queued_count);

  std::size_t queued_internal_count = 0;
  for (const auto &entry : stream_state.getQueuedInternalMessages()) {
    if ((!entry.agent_id.empty() && entry.agent_id != focused_agent_id) ||
        (!entry.thread_id.empty() && entry.thread_id != thread_id)) {
      continue;
    }
    queued_internal_count++;
    HashCombine(signature, bucketQueued(entry.text.size()));
  }
  HashCombine(signature, queued_internal_count);

  return signature;
}


const std::unordered_set<std::string> kEmptyToolCallIdSet{};
std::shared_ptr<firmius::shared::AgentHistory> EnsureTranscriptHistory(
    std::shared_ptr<firmius::shared::AgentHistory> &history,
    const std::string &thread_id) {
  if (!history) {
    history = std::make_shared<firmius::shared::AgentHistory>();
  }
  if (history->threadId.empty()) {
    history->threadId = thread_id;
  }
  return history;
}

void appendOptimisticUserTurnImpl(
    std::shared_ptr<firmius::shared::AgentHistory> &history,
    const std::string &thread_id, const std::string &text,
    const std::vector<firmius::shared::ImageContent> &images) {
  auto transcript_history = EnsureTranscriptHistory(history, thread_id);
  firmius::shared::AgentTurn turn;
  turn.turnId = "user-optimistic-" +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());

  firmius::shared::Message message;
  message.role = firmius::shared::Role::User;
  message.timestamp = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  message.content = {firmius::shared::TextContent{text}};
  for (const auto &img : images) {
    message.content.push_back(img);
  }
  turn.messages.push_back(std::move(message));
  transcript_history->turns.push_back(std::move(turn));
}



bool isTuiStartupProfilingEnabled() {
  static bool enabled = parseTuiStartupProfilingEnabledFromEnv();
  return enabled;
}

void noteTuiAppEventEnqueued() {
  if (!isTuiStartupProfilingEnabled()) {
    return;
  }
  tuiProfilingStats().app_event_enqueued.fetch_add(1, std::memory_order_relaxed);
}

void noteTuiCustomEventPosted() {
  if (!isTuiStartupProfilingEnabled()) {
    return;
  }
  tuiProfilingStats().custom_event_posted.fetch_add(1, std::memory_order_relaxed);
}

void noteTuiCustomEventDrained() {
  if (!isTuiStartupProfilingEnabled()) {
    return;
  }
  tuiProfilingStats().custom_event_drained.fetch_add(1, std::memory_order_relaxed);
}

void noteTuiOnEventDispatch(std::chrono::nanoseconds elapsed) {
  if (!isTuiStartupProfilingEnabled()) {
    return;
  }
  auto &stats = tuiProfilingStats();
  stats.on_event_dispatch_count.fetch_add(1, std::memory_order_relaxed);
  stats.on_event_dispatch_ns.fetch_add(static_cast<int64_t>(elapsed.count()),
                                      std::memory_order_relaxed);
}

void noteTuiThreadChanged(std::chrono::nanoseconds elapsed) {
  if (!isTuiStartupProfilingEnabled()) {
    return;
  }
  auto &stats = tuiProfilingStats();
  stats.thread_changed_count.fetch_add(1, std::memory_order_relaxed);
  stats.thread_changed_ns.fetch_add(static_cast<int64_t>(elapsed.count()),
                                    std::memory_order_relaxed);
}

void noteTuiRebuildToolCalls(std::chrono::nanoseconds elapsed) {
  if (!isTuiStartupProfilingEnabled()) {
    return;
  }
  auto &stats = tuiProfilingStats();
  stats.rebuild_tool_calls_count.fetch_add(1, std::memory_order_relaxed);
  stats.rebuild_tool_calls_ns.fetch_add(static_cast<int64_t>(elapsed.count()),
                                        std::memory_order_relaxed);
}

void noteTuiChatWindowRebuild(std::chrono::nanoseconds elapsed) {
  if (!isTuiStartupProfilingEnabled()) {
    return;
  }
  auto &stats = tuiProfilingStats();
  stats.chat_window_rebuild_count.fetch_add(1, std::memory_order_relaxed);
  stats.chat_window_rebuild_ns.fetch_add(static_cast<int64_t>(elapsed.count()),
                                         std::memory_order_relaxed);
}

void noteTuiFrameRendered(std::chrono::nanoseconds elapsed) {
  const auto now_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  tuiProfilingStats().last_frame_rendered_at_ms.store(
      now_ms, std::memory_order_relaxed);
  if (!isTuiStartupProfilingEnabled()) {
    return;
  }
  auto &stats = tuiProfilingStats();
  stats.frame_render_count.fetch_add(1, std::memory_order_relaxed);
  stats.frame_render_ns.fetch_add(static_cast<int64_t>(elapsed.count()),
                                  std::memory_order_relaxed);
}

std::uint64_t tuiLastFrameRenderedAtMs() {
  return tuiProfilingStats().last_frame_rendered_at_ms.load(
      std::memory_order_relaxed);
}
void noteTuiModalOpenRequested(const std::string &name) {
  if (!isTuiStartupProfilingEnabled() || name.empty()) {
    return;
  }
  auto &stats = tuiProfilingStats();
  std::lock_guard<std::mutex> lock(stats.modal_profile_mutex);
  stats.modal_open_requested_at[name] = std::chrono::steady_clock::now();
}

void noteTuiModalFirstPaint(const std::string &name) {
  if (!isTuiStartupProfilingEnabled() || name.empty()) {
    return;
  }
  auto &stats = tuiProfilingStats();
  std::lock_guard<std::mutex> lock(stats.modal_profile_mutex);
  auto it = stats.modal_open_requested_at.find(name);
  if (it == stats.modal_open_requested_at.end()) {
    return;
  }
  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - it->second).count();
  std::ostringstream out;
  out << std::fixed << std::setprecision(3);
  out << "[tui_modal_profile] {\"name\":\"" << name
      << "\",\"first_paint_ms\":" << elapsed << "}\n";
  std::cerr << out.str();
  stats.modal_open_requested_at.erase(it);
}

void noteTuiRequestAnimationFrameFromState() {
  if (!isTuiStartupProfilingEnabled()) {
    return;
  }
  tuiProfilingStats().raf_state_count.fetch_add(1, std::memory_order_relaxed);
}

void noteTuiRequestAnimationFrameFromScrollableBoxWidthChange() {
  if (!isTuiStartupProfilingEnabled()) {
    return;
  }
  tuiProfilingStats().raf_scrollable_width_change_count.fetch_add(
      1, std::memory_order_relaxed);
}

void noteTuiRequestAnimationFrameFromAgentStripSpinner() {
  if (isTuiStartupProfilingEnabled()) {
    tuiProfilingStats().raf_agent_strip_spinner_count.fetch_add(
        1, std::memory_order_relaxed);
  }
  TuiState::instance().requestAnimationTick(std::chrono::milliseconds(250));
}

void noteTuiRequestAnimationFrameFromWelcomeScreen() {
  TuiState::instance().requestAnimationTick(std::chrono::milliseconds(33));
}

void noteTuiRequestAnimationFrameFromLiveStatusRow() {
  TuiState::instance().requestAnimationTick(std::chrono::milliseconds(33));
}

std::string tuiProfilingSummaryText() {
  if (!isTuiStartupProfilingEnabled()) {
    return "";
  }

  const auto &stats = tuiProfilingStats();
  std::ostringstream out;
  out << "TUI Profiling\n";
  out << "AppEvent enqueued: "
      << stats.app_event_enqueued.load(std::memory_order_relaxed) << "\n";
  out << "Event::Custom posted: "
      << stats.custom_event_posted.load(std::memory_order_relaxed) << "\n";
  out << "Event::Custom drained: "
      << stats.custom_event_drained.load(std::memory_order_relaxed) << "\n";

  const uint64_t onEventCount =
      stats.on_event_dispatch_count.load(std::memory_order_relaxed);
  const int64_t onEventNs =
      stats.on_event_dispatch_ns.load(std::memory_order_relaxed);
  out << "onEvent dispatches: " << onEventCount << " ("
      << std::fixed << std::setprecision(3) << nanosToMillis(onEventNs)
      << " ms total";
  if (onEventCount > 0) {
    out << ", " << nanosToMillis(onEventNs) / static_cast<double>(onEventCount)
        << " ms avg";
  }
  out << ")\n";

  const uint64_t threadChangedCount =
      stats.thread_changed_count.load(std::memory_order_relaxed);
  const int64_t threadChangedNs =
      stats.thread_changed_ns.load(std::memory_order_relaxed);
  out << "ThreadChanged handled: " << threadChangedCount << " ("
      << nanosToMillis(threadChangedNs) << " ms total";
  if (threadChangedCount > 0) {
    out << ", "
        << nanosToMillis(threadChangedNs) /
               static_cast<double>(threadChangedCount)
        << " ms avg";
  }
  out << ")\n";

  const uint64_t rebuildCount =
      stats.rebuild_tool_calls_count.load(std::memory_order_relaxed);
  const int64_t rebuildNs =
      stats.rebuild_tool_calls_ns.load(std::memory_order_relaxed);
  out << "rebuildToolCallsFromHistory: " << rebuildCount << " ("
      << nanosToMillis(rebuildNs) << " ms total";
  if (rebuildCount > 0) {
    out << ", " << nanosToMillis(rebuildNs) / static_cast<double>(rebuildCount)
        << " ms avg";
  }
  out << ")\n";

  const uint64_t chatRebuildCount =
      stats.chat_window_rebuild_count.load(std::memory_order_relaxed);
  const int64_t chatRebuildNs =
      stats.chat_window_rebuild_ns.load(std::memory_order_relaxed);
  out << "ChatWindow rebuilds: " << chatRebuildCount << " ("
      << nanosToMillis(chatRebuildNs) << " ms total";
  if (chatRebuildCount > 0) {
    out << ", "
        << nanosToMillis(chatRebuildNs) / static_cast<double>(chatRebuildCount)
        << " ms avg";
  }
  out << ")\n";

  const uint64_t frameRenderCount =
      stats.frame_render_count.load(std::memory_order_relaxed);
  const int64_t frameRenderNs =
      stats.frame_render_ns.load(std::memory_order_relaxed);
  out << "Frame renders: " << frameRenderCount << " ("
      << nanosToMillis(frameRenderNs) << " ms total";
  if (frameRenderCount > 0) {
    out << ", "
        << nanosToMillis(frameRenderNs) / static_cast<double>(frameRenderCount)
        << " ms avg";
  }
  out << ")\n";

  out << "RequestAnimationFrame counts: state="
      << stats.raf_state_count.load(std::memory_order_relaxed)
      << ", scrollable_width_change="
      << stats.raf_scrollable_width_change_count.load(std::memory_order_relaxed)
      << ", agent_strip_spinner="
      << stats.raf_agent_strip_spinner_count.load(std::memory_order_relaxed)
      << "\n";

  return out.str();
}
namespace detail {

std::string summarizeQuotaBucketsForModel(
    const std::vector<firmius::shared::QuotaBucket> &buckets,
    const std::string &modelId) {
  if (buckets.empty()) {
    return "";
  }

  auto normalize = [](std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                     return static_cast<char>(std::tolower(c));
                   });
    std::replace(value.begin(), value.end(), '_', '-');
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](unsigned char c) { return std::isspace(c); }),
                value.end());
    return value;
  };

  auto icon = [](const std::string &name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                     return static_cast<char>(std::tolower(c));
                   });
    if (lower.find("5h") != std::string::npos ||
        lower.find("hour") != std::string::npos) {
      return std::string("󱑂");
    }
    if (lower.find("weekly") != std::string::npos ||
        lower.find("week") != std::string::npos) {
      return std::string("󰃭");
    }
    if (lower.find("monthly") != std::string::npos ||
        lower.find("month") != std::string::npos) {
      return std::string("󰃮");
    }
    if (lower.find("annual") != std::string::npos ||
        lower.find("year") != std::string::npos) {
      return std::string("󰸗");
    }
    if (lower.find("credit") != std::string::npos ||
        lower.find("balance") != std::string::npos) {
      return std::string("󰆧");
    }
    if (lower.find("qwen") != std::string::npos) {
      return std::string("󰘦");
    }
    return std::string(firmius::shared::ICON_WARNING);
  };

  auto format = [&icon](const firmius::shared::QuotaBucket &bucket) {
    std::ostringstream out;
    out << icon(bucket.name) << " " << std::fixed
        << std::setprecision(0) << (bucket.remainingFraction * 100.0f) << "%";
    return out.str();
  };

  const std::string normalizedModel = normalize(modelId);
  if (!normalizedModel.empty()) {
    for (const auto &bucket : buckets) {
      if (normalize(bucket.name) == normalizedModel) {
        return format(bucket);
      }
    }

    for (const auto &bucket : buckets) {
      const std::string normalizedBucket = normalize(bucket.name);
      if (normalizedBucket.empty()) {
        continue;
      }
      if (normalizedBucket.find(normalizedModel) != std::string::npos ||
          normalizedModel.find(normalizedBucket) != std::string::npos) {
        return format(bucket);
      }
    }
  }

  return format(buckets.front());
}

} // namespace detail

namespace {

std::optional<std::string> compactionIdFromTurnId(const std::string& turnId) {
  constexpr const char* prefixes[] = {"compaction-start-", "compaction-summary-",
                                      "compaction-end-"};
  for (const char* prefix : prefixes) {
    const std::size_t len = std::char_traits<char>::length(prefix);
    if (turnId.rfind(prefix, 0) == 0) {
      return turnId.substr(len);
    }
  }
  return std::nullopt;
}

bool turnsEquivalentForTranscript(const firmius::shared::AgentTurn& lhs,
                                  const firmius::shared::AgentTurn& rhs) {
  return lhs.turnId == rhs.turnId;
}

bool isTranscriptMeaningfulMessage(const firmius::shared::Message& message) {
  if (message.role == firmius::shared::Role::User ||
      message.role == firmius::shared::Role::Assistant ||
      message.role == firmius::shared::Role::System) {
    for (const auto& part : message.content) {
      if (std::holds_alternative<firmius::shared::TextContent>(part) ||
          std::holds_alternative<firmius::shared::ThinkingContent>(part) ||
          std::holds_alternative<firmius::shared::ImageContent>(part) ||
          std::holds_alternative<firmius::shared::NoticeContent>(part) ||
          std::holds_alternative<firmius::shared::ErrorContent>(part)) {
        return true;
      }
    }
  }
  return false;
}

std::vector<firmius::shared::AgentTurn> filterSnapshotTurnsForTranscript(
    const std::vector<firmius::shared::AgentTurn>& turns) {
  std::vector<firmius::shared::AgentTurn> filtered;
  filtered.reserve(turns.size());
  for (const auto& turn : turns) {
    bool meaningful = false;
    for (const auto& message : turn.messages) {
      if (isTranscriptMeaningfulMessage(message)) {
        meaningful = true;
        break;
      }
    }
    if (meaningful) {
      filtered.push_back(turn);
    }
  }
  return filtered;
}

std::size_t overlappingSnapshotSuffixLength(
    const std::vector<firmius::shared::AgentTurn>& snapshotTurns,
    const std::vector<firmius::shared::AgentTurn>& currentTurns,
    std::size_t currentStart) {
  const std::size_t maxCount =
      std::min(snapshotTurns.size(), currentTurns.size() - currentStart);
  for (std::size_t count = maxCount; count > 0; --count) {
    bool allMatch = true;
    for (std::size_t i = 0; i < count; ++i) {
      if (!turnsEquivalentForTranscript(
              snapshotTurns[snapshotTurns.size() - count + i],
              currentTurns[currentStart + i])) {
        allMatch = false;
        break;
      }
    }
    if (allMatch) {
      return count;
    }
  }
  return 0;
}

std::vector<firmius::shared::AgentTurn> expandCompactionTurns(
    const std::vector<firmius::shared::AgentTurn>& turns,
    const std::unordered_map<std::string, firmius::core::CompactionSnapshot>& snapshots,
    std::unordered_set<std::string>& expanded_ids) {
  std::vector<firmius::shared::AgentTurn> result;
  for (std::size_t i = 0; i < turns.size(); ++i) {
    const auto compactionId = compactionIdFromTurnId(turns[i].turnId);
    if (!compactionId.has_value() || turns[i].turnId.rfind("compaction-start-", 0) != 0) {
      result.push_back(turns[i]);
      continue;
    }

    std::size_t blockEnd = i;
    while (blockEnd + 1 < turns.size()) {
      const auto nextId = compactionIdFromTurnId(turns[blockEnd + 1].turnId);
      if (!nextId.has_value() || *nextId != *compactionId) {
        break;
      }
      ++blockEnd;
      if (turns[blockEnd].turnId.rfind("compaction-end-", 0) == 0) {
        break;
      }
    }

    auto snapshotIt = snapshots.find(*compactionId);
    if (snapshotIt != snapshots.end() && !expanded_ids.count(*compactionId)) {
      expanded_ids.insert(*compactionId);
      const auto snapshotTurns =
          filterSnapshotTurnsForTranscript(snapshotIt->second.turns);
      const std::size_t overlap =
          overlappingSnapshotSuffixLength(snapshotTurns, turns, blockEnd + 1);
      auto expandedSnapshot =
          expandCompactionTurns(snapshotTurns, snapshots, expanded_ids);
      result.insert(result.end(), expandedSnapshot.begin(), expandedSnapshot.end());

      for (std::size_t j = i; j <= blockEnd; ++j) {
        result.push_back(turns[j]);
      }
      i = blockEnd + overlap;
      if (i >= turns.size()) {
        break;
      }
      --i;
      continue;
    }

    for (std::size_t j = i; j <= blockEnd; ++j) {
      result.push_back(turns[j]);
    }
    i = blockEnd;
  }
  return result;
}

std::shared_ptr<firmius::shared::AgentHistory> expandHistoryForTranscript(
    const std::string& thread_id, const std::string& agent_id,
    const firmius::shared::AgentHistory& base_history) {
  if (thread_id.empty() || agent_id.empty()) {
    return std::make_shared<firmius::shared::AgentHistory>(base_history);
  }

  bool hasCompaction = false;
  for (const auto& turn : base_history.turns) {
    if (compactionIdFromTurnId(turn.turnId).has_value()) {
      hasCompaction = true;
      break;
    }
  }
  if (!hasCompaction) {
    return std::make_shared<firmius::shared::AgentHistory>(base_history);
  }

  firmius::core::ThreadManager tm(
      (firmius::shared::PlatformPaths::firmiusHomeDir() / "threads").string());
  const auto snapshotList = tm.loadCompactionSnapshots(thread_id, agent_id);
  if (snapshotList.empty()) {
    return std::make_shared<firmius::shared::AgentHistory>(base_history);
  }

  std::unordered_map<std::string, firmius::core::CompactionSnapshot> snapshots;
  for (const auto& snapshot : snapshotList) {
    if (!snapshot.compactionId.empty()) {
      snapshots[snapshot.compactionId] = snapshot;
    }
  }

  auto expanded = std::make_shared<firmius::shared::AgentHistory>(base_history);
  expanded->turns = expandCompactionTranscriptForDisplay(base_history.turns, snapshots);
  return expanded;
}

const firmius::shared::AgentHistory* resolveAgentHistoryForThread(
    firmius::core::Harness* harness, const std::string& thread_id,
    const std::string& agent_id,
    std::shared_ptr<firmius::shared::AgentHistory>* owned_history = nullptr) {
  if (agent_id.empty()) {
    return nullptr;
  }
  if (thread_id.empty()) {
    return nullptr;
  }

  const firmius::shared::AgentHistory* base_history = nullptr;
  std::shared_ptr<firmius::shared::AgentHistory> loaded_history;

  if (auto agent = firmius::core::AgentRegistry::instance().getAgent(agent_id)) {
    const auto& ctx = agent->getContext();
    if (ctx.history && ctx.history->threadId == thread_id && !ctx.history->turns.empty()) {
      base_history = ctx.history.get();
    }
  }

  if (!base_history && harness) {
    auto history_ptr = harness->getAgentHistoryPtr(agent_id);
    if (history_ptr && history_ptr->threadId == thread_id && !history_ptr->turns.empty()) {
      loaded_history = std::move(history_ptr);
      base_history = loaded_history.get();
    }
  }

  if (!base_history) {
    auto history = firmius::core::ThreadManager(
                       firmius::core::ThreadManager::defaultBasePath())
                       .loadAgentHistory(thread_id, agent_id);
    if (history.turns.empty()) {
      return nullptr;
    }
    loaded_history = std::make_shared<firmius::shared::AgentHistory>(std::move(history));
    base_history = loaded_history.get();
  }

  if (!base_history || base_history->turns.empty()) {
    return nullptr;
  }
  if (owned_history) {
    *owned_history = expandHistoryForTranscript(thread_id, agent_id, *base_history);
    return owned_history->get();
  }
  return nullptr;
}

class PreemptiveEventComponent : public ftxui::ComponentBase {
public:
  PreemptiveEventComponent(ftxui::Component child,
                           std::function<bool(ftxui::Event)> pre_handler)
      : child_(std::move(child)), pre_handler_(std::move(pre_handler)) {
    if (child_) {
      Add(child_);
    }
  }

  ftxui::Element OnRender() override {
    return child_ ? child_->Render() : ftxui::text("");
  }

  bool OnEvent(ftxui::Event event) override {
    if (pre_handler_ && pre_handler_(event)) {
      return true;
    }
    return child_ ? child_->OnEvent(event) : false;
  }

private:
  ftxui::Component child_;
  std::function<bool(ftxui::Event)> pre_handler_;
};

} // namespace

static std::string statusToString(shared::AgentStatus status) {
  using shared::AgentStatus;
  switch (status) {
  case AgentStatus::Idle:
    return "idle";
  case AgentStatus::Streaming:
    return "streaming";
  case AgentStatus::ExecutingTool:
    return "executing_tool";
  case AgentStatus::AwaitingInput:
    return "awaiting_input";
  case AgentStatus::Compacting:
    return "compacting";
  case AgentStatus::ProviderWaiting:
    return "provider_waiting";
  case AgentStatus::Error:
    return "error";
  case AgentStatus::Cancelled:
    return "cancelled";
  }
  return "unknown";
}

static std::string formatCompactCount(uint32_t value) {
  std::ostringstream out;
  if (value >= 1000000) {
    out << std::fixed << std::setprecision(1)
        << (static_cast<double>(value) / 1000000.0);
    std::string text = out.str();
    if (text.size() > 2 && text.substr(text.size() - 2) == ".0") {
      text.erase(text.size() - 2);
    }
    return text + "M";
  }
  if (value >= 1000) {
    out << std::fixed << std::setprecision(1)
        << (static_cast<double>(value) / 1000.0);
    std::string text = out.str();
    if (text.size() > 2 && text.substr(text.size() - 2) == ".0") {
      text.erase(text.size() - 2);
    }
    return text + "k";
  }
  return std::to_string(value);
}

static std::string formatDurationFromMs(uint64_t durationMs) {
  const uint64_t totalSeconds = durationMs / 1000;
  const uint64_t minutes = totalSeconds / 60;
  const uint64_t seconds = totalSeconds % 60;
  std::ostringstream out;
  if (minutes > 0) {
    out << minutes << "m" << seconds << "s";
  } else {
    out << seconds << "s";
  }
  return out.str();
}

struct CurrentQuotaDisplay {
  std::string accountLabel;
  std::string usageLabel;
};

static std::string compactAccountLabel(const std::string &identifier) {
  std::string label = identifier;
  const auto at = label.find('@');
  if (at != std::string::npos) {
    label = label.substr(0, at);
  }
  if (label.size() > 12) {
    label = label.substr(0, 11) + "…";
  }
  return label;
}

static CurrentQuotaDisplay resolveCurrentQuotaDisplay(
    const std::shared_ptr<firmius::provider::IProvider> &provider,
    const std::string &modelId) {
  CurrentQuotaDisplay display;
  if (!provider) {
    return display;
  }

  try {
    if (auto oauth =
            std::dynamic_pointer_cast<firmius::provider::BaseOAuthProvider>(
                provider)) {
      auto current = oauth->getAvailableAccount(modelId);
      if (!current.has_value()) {
        return display;
      }
      display.accountLabel = compactAccountLabel(current->identifier);
      const auto quotas = oauth->getAllQuotas();
      auto it = quotas.find(current->identifier);
      if (it != quotas.end()) {
        display.usageLabel =
            detail::summarizeQuotaBucketsForModel(it->second, modelId);
      } else if (auto balIt = current->metadata.find("total_balance");
                 balIt != current->metadata.end()) {
        display.usageLabel = "balance $" + balIt->second;
      }
      return display;
    }

    if (auto api =
            std::dynamic_pointer_cast<firmius::provider::BaseAPIKeyProvider>(
                provider)) {
      auto current = api->getAvailableAccount(modelId);
      if (!current.has_value() || *current == nullptr) {
        return display;
      }
      display.accountLabel = compactAccountLabel((*current)->identifier);
      if (api->supportsQuotaTracking()) {
        const auto quotas = api->getAllQuotas();
        auto it = quotas.find((*current)->identifier);
        if (it != quotas.end()) {
          display.usageLabel =
              detail::summarizeQuotaBucketsForModel(it->second, modelId);
        }
      }
    }
  } catch (...) {
    return CurrentQuotaDisplay{};
  }

  return display;
}

static std::string resolveDefaultLeadPersona(
    const firmius::core::Harness *harness) {
  if (harness) {
    const auto &cfg = harness->getConfig();
    if (!cfg.defaultLeadPersona.empty() &&
        firmius::core::PurposeLoader::isValid(cfg.defaultLeadPersona)) {
      return cfg.defaultLeadPersona;
    }
  }
  if (firmius::core::PurposeLoader::isValid("lead")) {
    return "lead";
  }
  const auto purposes = firmius::core::PurposeLoader::listPurposes();
  if (!purposes.empty()) {
    return purposes.front();
  }
  return "lead";
}

static std::string resolvePersonaTitle(const std::string &personaName) {
  try {
    return firmius::core::PurposeLoader::load(personaName).title;
  } catch (...) {
    return personaName;
  }
}

static std::pair<std::string, std::string>
resolveStatusMode(const std::string &modeName, const std::string &personaName) {
  if (modeName.empty()) {
    return {"", ""};
  }
  auto &registry = firmius::core::modes::ModeRegistry::instance();
  const auto *mode = registry.resolveForPersona(modeName, personaName);
  if (!mode) {
    return {modeName, ""};
  }
  return {mode->qualifiedName(), mode->glyph};
}

static std::string firmiusThreadsPath() {
  return (firmius::shared::PlatformPaths::firmiusHomeDir() / "threads")
      .string();
}

std::vector<std::string>
getSwitchableLeadPersonas(firmius::core::Harness *) {
  auto purposes = firmius::core::PurposeLoader::listSwitchablePurposes();
  if (purposes.empty()) {
    purposes.push_back("lead");
  }
  return purposes;
}

std::vector<std::string>
focusCycleCandidates(const std::string &focusedAgentId) {
  std::vector<std::string> candidates;
  auto all_ids = firmius::core::AgentRegistry::instance().listAll();
  if (all_ids.empty()) {
    return candidates;
  }

  std::string parent_focus = focusedAgentId;
  std::string focused_parent;
  auto focused_agent =
      firmius::core::AgentRegistry::instance().getAgent(focusedAgentId);
  if (focused_agent) {
    focused_parent = focused_agent->getContext().identity.parentId;
  }

  bool has_children = false;
  for (const auto &id : all_ids) {
    auto candidate = firmius::core::AgentRegistry::instance().getAgent(id);
    if (!candidate) {
      continue;
    }
    if (candidate->getContext().identity.parentId == focusedAgentId) {
      has_children = true;
      break;
    }
  }
  if (!has_children && !focused_parent.empty()) {
    parent_focus = focused_parent;
  }

  for (const auto &id : all_ids) {
    auto candidate = firmius::core::AgentRegistry::instance().getAgent(id);
    if (!candidate) {
      continue;
    }
    if (candidate->getContext().identity.parentId == parent_focus) {
      candidates.push_back(id);
    }
  }

  if (candidates.empty() && !focusedAgentId.empty()) {
    auto candidate =
        firmius::core::AgentRegistry::instance().getAgent(focusedAgentId);
    if (candidate) {
      candidates.push_back(focusedAgentId);
    }
  }

  if (candidates.empty()) {
    for (const auto &id : all_ids) {
      auto candidate = firmius::core::AgentRegistry::instance().getAgent(id);
      if (candidate) {
        candidates.push_back(id);
      }
    }
  }

  return candidates;
}

void TuiState::syncCurrentThreadMetadataFromHarness(bool preserve_live_state) {
  if (!harness_) {
    return;
  }

  const std::string currentThreadId = harness_->currentThreadId();
  if (currentThreadId.empty()) {
    return;
  }

  for (const auto &metadata : harness_->listThreads()) {
    if (metadata.threadId != currentThreadId) {
      continue;
    }

    if (!preserve_live_state) {
      handleAppEvent(shared::ThreadChanged{currentThreadId, metadata});
      return;
    }

    thread_ = metadata;
    focused_agent_id_ = harness_->focusedAgentId();
    if (title_model_) {
      title_model_->title = thread_.title;
      title_model_->thread_id = thread_.threadId;
    }
    active_plan_state_.setExpanded(true);
    active_plan_state_.hydrateForThread(thread_, loadActivePlanForThread(thread_));
    setViewMode(ViewMode::Chat);
    requestRefresh(RefreshFlags::Status);
    requestRefresh(RefreshFlags::AgentStrip);
    requestRefresh(RefreshFlags::TodoLane);
    requestRefresh(RefreshFlags::ContextLane);
    notifyChatTranscriptChanged();
    applyPendingRefreshes();
    if (screen_) {
      postEvent(ftxui::Event::Custom);
    }
    return;
  }
}

void TuiState::refreshFocusedHistory() {
  if (!harness_ || focused_agent_id_.empty()) {
    history_.reset();
    rebuildEditableUserMessages();
    return;
  }

  std::shared_ptr<firmius::shared::AgentHistory> owned_history;
  if (resolveAgentHistoryForThread(harness_, thread_.threadId, focused_agent_id_,
                                   &owned_history)) {
    history_ = std::move(owned_history);
    agent_history_cache_[focused_agent_id_] = history_;
    agent_persisted_tool_call_ids_cache_[focused_agent_id_] =
        firmius::tui::CollectToolCallIdsFromHistory(history_.get());
  } else {
    history_.reset();
    agent_history_cache_.erase(focused_agent_id_);
    agent_persisted_tool_call_ids_cache_.erase(focused_agent_id_);
  }
  rebuildEditableUserMessages();
}

void TuiState::rebuildEditableUserMessages() {
  editable_user_messages_.clear();
  selected_editable_message_index_ = -1;
  if (!history_) {
    return;
  }
  for (const auto &turn : history_->turns) {
    for (const auto &message : turn.messages) {
      if (message.role != shared::Role::User) {
        continue;
      }
      TuiState::EditableUserMessage item;
      item.timestamp = message.timestamp;
      for (const auto &content : message.content) {
        if (const auto *text = std::get_if<shared::TextContent>(&content)) {
          if (!item.text.empty()) {
            item.text += "\n";
          }
          item.text += text->text;
        } else if (const auto *image =
                       std::get_if<shared::ImageContent>(&content)) {
          item.images.push_back(*image);
        }
      }
      editable_user_messages_.push_back(std::move(item));
    }
  }
  if (!editable_user_messages_.empty()) {
    selected_editable_message_index_ =
        static_cast<int>(editable_user_messages_.size()) - 1;
  }
}

bool TuiState::isEditModeSelection(uint64_t timestamp) const {
  return edit_mode_active_ && selected_editable_message_index_ >= 0 &&
         selected_editable_message_index_ <
             static_cast<int>(editable_user_messages_.size()) &&
         editable_user_messages_[selected_editable_message_index_].timestamp ==
             timestamp;
}

void TuiState::selectEditableMessageByTimestamp(uint64_t timestamp) {
  for (int i = 0; i < static_cast<int>(editable_user_messages_.size()); ++i) {
    if (editable_user_messages_[i].timestamp == timestamp) {
      selected_editable_message_index_ = i;
      edit_mode_active_ = true;
      if (input_model_) {
        input_model_->is_focused = false;
      }
      if (chat_component_) {
        chat_component_->OnEvent(ftxui::Event::Special("TranscriptChanged"));
      }
      if (screen_) {
        postEvent(ftxui::Event::Custom);
      }
      return;
    }
  }
}

bool TuiState::commitSelectedEditableMessageToInput() {
  if (selected_editable_message_index_ < 0 ||
      selected_editable_message_index_ >=
          static_cast<int>(editable_user_messages_.size())) {
    return false;
  }
  const auto selected =
      editable_user_messages_[selected_editable_message_index_];
  pending_edit_message_ = selected;

  if (input_model_ && input_model_->buffer && input_model_->cursor) {
    *input_model_->buffer = selected.text;
    *input_model_->cursor =
        static_cast<int>(input_model_->buffer->size());
    input_model_->pasted_blocks.clear();
    for (size_t i = 0; i < selected.images.size(); ++i) {
      const auto &image = selected.images[i];
      const std::string placeholder =
          "[Image " + std::to_string(i + 1) + "]";
      const size_t start_pos = input_model_->buffer->size();
      input_model_->buffer->append(placeholder);
      PastedBlock block;
      block.type = "image";
      block.id = "edit-image-" + std::to_string(i);
      const auto comma = image.url.find(",");
      block.content = image.url.rfind("data:", 0) == 0 &&
                              comma != std::string::npos
                          ? image.url.substr(comma + 1)
                          : image.url;
      block.mime_type = image.mediaType;
      block.start_pos = start_pos;
      block.end_pos = input_model_->buffer->size();
      input_model_->pasted_blocks.push_back(std::move(block));
    }
    *input_model_->cursor = static_cast<int>(input_model_->buffer->size());
  }
  edit_mode_active_ = false;
  if (input_model_) {
    input_model_->is_focused = true;
  }
  if (input_component_) {
    input_component_->TakeFocus();
  }
  NotificationManager::instance().notifyInfo(
      "Rewrite Staged",
      "Editing an earlier message. Press Enter to apply, or Esc to cancel.",
      std::chrono::milliseconds(2500));
  if (screen_) {
    if (chat_component_) {
      chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
    }
    postEvent(ftxui::Event::Custom);
  }
  return true;
}

std::optional<shared::EditBatchSummary> TuiState::latestFocusedEditBatch() const {
  if (!harness_ || thread_.threadId.empty()) {
    return std::nullopt;
  }
  const auto batches = harness_->listEditBatches(thread_.threadId);
  for (const auto &batch : batches) {
    if (batch.status != shared::EditBatchStatus::Undone) {
      return batch;
    }
  }
  return std::nullopt;
}

std::optional<shared::EditBatchSummary> TuiState::latestFocusedUndoneEditBatch() const {
  if (!harness_ || thread_.threadId.empty()) {
    return std::nullopt;
  }
  const auto batches = harness_->listEditBatches(thread_.threadId);
  for (const auto &batch : batches) {
    if (batch.status == shared::EditBatchStatus::Undone &&
        batch.undoActionBatchId.has_value() &&
        !batch.undoActionBatchId->empty()) {
      return batch;
    }
  }
  return std::nullopt;
}

void TuiState::triggerTranscriptUndoFromHotkey() {
  if (!harness_ || focused_agent_id_.empty()) {
    NotificationManager::instance().notifyWarning(
        "Transcript Undo", "No focused agent history to undo.",
        std::chrono::milliseconds(1800));
    return;
  }

  int count = 1;
  auto history = harness_->getAgentHistoryPtr(focused_agent_id_);
  if (history && history->turns.size() > 2) {
    auto is_user_turn = [](const shared::AgentTurn &turn) {
      for (const auto &msg : turn.messages) {
        if (msg.role == shared::Role::User) {
          return true;
        }
      }
      return false;
    };
    auto is_compaction_turn = [](const shared::AgentTurn &turn) {
      return turn.turnId.rfind("compaction-start-", 0) == 0 ||
             turn.turnId.rfind("compaction-summary-", 0) == 0 ||
             turn.turnId.rfind("compaction-end-", 0) == 0;
    };
    for (std::size_t i = history->turns.size(); i-- > 2;) {
      if (is_user_turn(history->turns[i])) {
        count = std::max(1, static_cast<int>(history->turns.size() - i));
        break;
      }
    }
    if (count == 1) {
      int compaction_tail = 0;
      for (std::size_t i = history->turns.size(); i-- > 2;) {
        if (!is_compaction_turn(history->turns[i])) {
          break;
        }
        ++compaction_tail;
      }
      count = std::max(1, compaction_tail);
    }
  }

  auto result = harness_->undoTurnsWithRedo(count);
  if (!result.has_value()) {
    NotificationManager::instance().notifyWarning(
        "Transcript Undo", "Undo was not applied.",
        std::chrono::milliseconds(1800));
    return;
  }

  last_transcript_undo_action_ = result;
  NotificationManager::instance().notifyInfo(
      "Transcript Undo",
      "Undid " + std::to_string(std::max(1, count)) + " turn(s).",
      std::chrono::milliseconds(1800));
}

void TuiState::triggerTranscriptRedoFromHotkey() {
  if (!harness_ || !last_transcript_undo_action_.has_value()) {
    NotificationManager::instance().notifyWarning(
        "Transcript Redo", "No transcript undo is available to replay.",
        std::chrono::milliseconds(1800));
    return;
  }

  auto eligibility =
      harness_->evaluateTranscriptRedo(last_transcript_undo_action_->undoActionId);
  if (!eligibility.redoable) {
    NotificationManager::instance().notifyWarning(
        "Transcript Redo", eligibility.reason.empty() ? "Redo is unavailable."
                                                       : eligibility.reason,
        std::chrono::milliseconds(1800));
    return;
  }

  auto result =
      harness_->redoTranscriptUndoAction(last_transcript_undo_action_->undoActionId);
  if (!result.has_value()) {
    NotificationManager::instance().notifyWarning(
        "Transcript Redo", "Redo was not applied.",
        std::chrono::milliseconds(1800));
    return;
  }

  last_transcript_redo_action_ = result;
  NotificationManager::instance().notifyInfo(
      "Transcript Redo", "Replayed the last transcript undo.",
      std::chrono::milliseconds(1800));
  refreshFocusedHistory();
  notifyChatTranscriptChanged();
}

void TuiState::triggerEditUndoFromHotkey() {
  if (!harness_) {
    return;
  }
  auto batch = latestFocusedEditBatch();
  if (!batch.has_value()) {
    NotificationManager::instance().notifyWarning(
        "Edit Undo", "No applied edit batch is available to undo.",
        std::chrono::milliseconds(1800));
    return;
  }

  auto result = harness_->undoEditBatch(batch->editBatchId);
  if (!result.has_value()) {
    NotificationManager::instance().notifyWarning(
        "Edit Undo", "Undo was not applied.",
        std::chrono::milliseconds(1800));
    return;
  }

  last_edit_undo_action_ = result;
  NotificationManager::instance().notifyInfo(
      "Edit Undo", "Undid edit batch " + batch->editBatchId + ".",
      std::chrono::milliseconds(1800));
  requestRefresh(RefreshFlags::ContextLane);
}

void TuiState::triggerEditRedoFromHotkey() {
  if (!harness_) {
    return;
  }

  std::optional<std::string> undo_action_id;
  if (last_edit_undo_action_.has_value()) {
    undo_action_id = last_edit_undo_action_->undoActionId;
  } else {
    auto batch = latestFocusedUndoneEditBatch();
    if (batch.has_value()) {
      undo_action_id = batch->undoActionBatchId;
    }
  }

  if (!undo_action_id.has_value() || undo_action_id->empty()) {
    NotificationManager::instance().notifyWarning(
        "Edit Redo", "No edit undo is available to replay.",
        std::chrono::milliseconds(1800));
    return;
  }

  auto eligibility = harness_->evaluateEditBatchRedo(*undo_action_id);
  if (!eligibility.redoable) {
    NotificationManager::instance().notifyWarning(
        "Edit Redo", eligibility.reason.empty() ? "Redo is unavailable."
                                                : eligibility.reason,
        std::chrono::milliseconds(1800));
    return;
  }

  auto result = harness_->redoEditUndoAction(*undo_action_id);
  if (!result.has_value()) {
    NotificationManager::instance().notifyWarning(
        "Edit Redo", "Redo was not applied.",
        std::chrono::milliseconds(1800));
    return;
  }

  NotificationManager::instance().notifyInfo(
      "Edit Redo", "Replayed the last edit undo.",
      std::chrono::milliseconds(1800));
  requestRefresh(RefreshFlags::ContextLane);
}

static std::string permissionModeToDisplayName(
    shared::ThreadPermissionMode mode) {
  switch (mode) {
  case shared::ThreadPermissionMode::Request:
    return "Request";
  case shared::ThreadPermissionMode::AlwaysAllow:
    return "Always Allow";
  case shared::ThreadPermissionMode::DenyAll:
    return "Deny All";
  }
  return "Request";
}

static std::string permissionResponseToDisplayName(
    shared::PermissionResponse response) {
  switch (response) {
  case shared::PermissionResponse::AllowOnce:
    return "Allow Once";
  case shared::PermissionResponse::AllowCommandSession:
    return "Allow Command This Session";
  case shared::PermissionResponse::AllowCommandToolSession:
    return "Allow Entire Tool This Session";
  case shared::PermissionResponse::AllowCommandGlobal:
    return "Allow Command Globally";
  case shared::PermissionResponse::AllowPathSession:
    return "Allow Location This Session";
  case shared::PermissionResponse::AllowPathGlobal:
    return "Allow Location Globally";
  case shared::PermissionResponse::AllowAllReadsSession:
    return "Allow All Reads This Session";
  case shared::PermissionResponse::AllowAllToolSession:
    return "Allow Entire Tool This Session";
  case shared::PermissionResponse::AllowAlways:
    return "Allow Always";
  case shared::PermissionResponse::Deny:
    return "Deny";
  }
  return "Deny";
}

static std::vector<std::string> permissionOptionLabels(
    const shared::PermissionEscalationRequest &request) {
  using Type = shared::PermissionRequestType;
  if (request.requestType == Type::Command) {
    return {"Run once",
            "Allow this command for this session",
            "Allow entire tool for this session",
            "Allow this command globally",
            "Deny"};
  }

  if (request.requestType == Type::Read) {
    return {"Allow once",
            "Always allow this location for this session",
            "Always allow this location globally",
            "Allow all directory reads this session",
            "Deny"};
  }

  return {"Allow once",
          "Always allow writes in this location for this session",
          "Always allow writes in this location globally",
          "Deny"};
}

static std::vector<shared::PermissionResponse> permissionOptionResponses(
    const shared::PermissionEscalationRequest &request) {
  using Response = shared::PermissionResponse;
  using Type = shared::PermissionRequestType;
  if (request.requestType == Type::Command) {
    return {Response::AllowOnce,
            Response::AllowCommandSession,
            Response::AllowCommandToolSession,
            Response::AllowCommandGlobal,
            Response::Deny};
  }

  if (request.requestType == Type::Read) {
    return {Response::AllowOnce,
            Response::AllowPathSession,
            Response::AllowPathGlobal,
            Response::AllowAllReadsSession,
            Response::Deny};
  }

  return {Response::AllowOnce,
          Response::AllowPathSession,
          Response::AllowPathGlobal,
          Response::Deny};
}

static ftxui::Color permissionSeverityColor(
    const Theme &theme, shared::CommandSeverity severity) {
  using Severity = shared::CommandSeverity;
  switch (severity) {
  case Severity::VULNERABLE:
  case Severity::HIGH:
    return theme.status_bar.error.normal.fg;
  case Severity::MEDIUM:
    return theme.base.highlight;
  case Severity::LOW:
    return theme.base.fg;
  }
  return theme.base.fg;
}

static shared::ThreadPermissionMode nextPermissionMode(
    shared::ThreadPermissionMode mode) {
  using Mode = shared::ThreadPermissionMode;
  switch (mode) {
  case Mode::Request:
    return Mode::AlwaysAllow;
  case Mode::AlwaysAllow:
    return Mode::DenyAll;
  case Mode::DenyAll:
    return Mode::Request;
  }
  return Mode::Request;
}

class ScreenShaderNode : public ftxui::Node {
public:
  explicit ScreenShaderNode(ftxui::Element child)
      : ftxui::Node({std::move(child)}) {}

  void ComputeRequirement() override {
    requirement_ = children_[0]->requirement();
  }

  void SetBox(ftxui::Box box) override {
    box_ = box;
    children_[0]->SetBox(box);
  }

  void Render(ftxui::Screen &screen) override { children_[0]->Render(screen); }
};

static_assert(sizeof(ftxui::Color) == 5, "ftxui::Color size mismatch for darkening hack");

class DarkenNode : public ScreenShaderNode {
public:
  DarkenNode(ftxui::Element child, float t)
      : ScreenShaderNode(std::move(child)), t_(t) {}

  void Render(ftxui::Screen &screen) override {
    ScreenShaderNode::Render(screen);

    auto dimColor = [this](ftxui::Color c) -> ftxui::Color {
      struct ColorInternal {
        uint8_t type;
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
      };

      if (c == ftxui::Color::Default)
        return c;

      ColorInternal *raw = reinterpret_cast<ColorInternal *>(&c);
      float factor = 1.0f - t_;

      if (raw->type == 1) { // Palette16
        const ftxui::ColorInfo info = ftxui::GetColorInfo(static_cast<ftxui::Color::Palette16>(raw->r));
        raw->type = 3; raw->r = info.red; raw->g = info.green; raw->b = info.blue; raw->a = 255;
      } else if (raw->type == 2) { // Palette256
        const ftxui::ColorInfo info = ftxui::GetColorInfo(static_cast<ftxui::Color::Palette256>(raw->r));
        raw->type = 3; raw->r = info.red; raw->g = info.green; raw->b = info.blue; raw->a = 255;
      }

      if (raw->type == 3) { // TrueColor
        raw->r = static_cast<uint8_t>(raw->r * factor);
        raw->g = static_cast<uint8_t>(raw->g * factor);
        raw->b = static_cast<uint8_t>(raw->b * factor);
        return c;
      }
      return c;
    };

    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        auto &pixel = screen.PixelAt(x, y);
        pixel.foreground_color = dimColor(pixel.foreground_color);
        pixel.background_color = dimColor(pixel.background_color);
      }
    }
  }

private:
  float t_;
};

static ftxui::Element DarkenElement(ftxui::Element child, uint8_t alpha = 96) {
  return std::make_shared<DarkenNode>(std::move(child), static_cast<float>(alpha) / 255.0f);
}

TuiState &TuiState::instance() {
  static TuiState inst;
  return inst;
}

TuiState::TuiState() { loadUserPreferences(); }

void TuiState::loadUserPreferences() {
  const auto preferences = firmius::tui::loadUserPreferences();
  if (preferences.preferred_permission_mode.has_value()) {
    thread_.permissionMode = *preferences.preferred_permission_mode;
  }
  skin_config_ = preferences.skin_kind.has_value()
                     ? defaultSkinConfig(*preferences.skin_kind)
                     : defaultSkinConfig(SkinKind::Firmius);
  if (preferences.skin_kind.has_value() && *preferences.skin_kind == SkinKind::Claudex &&
      preferences.claudex_skin.has_value()) {
    skin_config_ = *preferences.claudex_skin;
  } else if (preferences.skin_kind.has_value() && *preferences.skin_kind == SkinKind::Firmius &&
             preferences.firmius_skin.has_value()) {
    skin_config_ = *preferences.firmius_skin;
  }
  show_agent_strip_ = preferences.show_agent_strip.value_or(true);
  show_work_panel_ = preferences.show_work_panel.value_or(true);
  agent_strip_visible_rows_ =
      std::max(1, preferences.agent_strip_rows.value_or(4));
  work_panel_height_override_ =
      std::max(0, preferences.work_panel_height.value_or(0));
}

void TuiState::persistUserPreferences() const {
  UserPreferences preferences;
  preferences.preferred_permission_mode = thread_.permissionMode;
  preferences.skin_kind = skin_config_.kind;
  if (skin_config_.kind == SkinKind::Claudex) {
    preferences.claudex_skin = skin_config_;
  } else {
    preferences.firmius_skin = skin_config_;
  }
  preferences.show_persistent_live_row = skin_config_.show_persistent_live_row;
  preferences.compact_status_bar = skin_config_.compactStatusBar();
  preferences.compact_tool_display = skin_config_.compactToolDisplay();
  preferences.show_agent_strip = show_agent_strip_;
  preferences.show_work_panel = show_work_panel_;
  preferences.agent_strip_rows = agent_strip_visible_rows_;
  preferences.work_panel_height = work_panel_height_override_;
  saveUserPreferences(preferences);
}

SkinKind TuiState::currentSkinKind() const { return skin_config_.kind; }

void TuiState::applySkinConfig(const SkinConfig &config) {
  skin_config_ = config;
  show_agent_strip_ = skin_config_.show_agent_strip;
  show_work_panel_ = skin_config_.show_work_panel;
  diffs_expanded_ = !skin_config_.diffsCollapsedByDefault();
  UIState::instance().diffsExpanded = diffs_expanded_;
  if (input_model_) {
    input_model_->compact_mode = skin_config_.compact_input;
  }
  if (status_model_) {
    status_model_->compact_skin_mode = skin_config_.compactStatusBar();
  }
  skin_config_.kind = config.kind;
  persistUserPreferences();
  if (chat_component_) {
    chat_component_->OnEvent(ftxui::Event::Special("ThemeChanged"));
  }
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
}

void TuiState::setSkinKind(SkinKind kind) {
  const auto preferences = firmius::tui::loadUserPreferences();
  skin_config_ = defaultSkinConfig(kind);
  if (kind == SkinKind::Claudex && preferences.claudex_skin.has_value()) {
    skin_config_ = *preferences.claudex_skin;
  } else if (kind == SkinKind::Firmius &&
             preferences.firmius_skin.has_value()) {
    skin_config_ = *preferences.firmius_skin;
  }
  skin_config_.kind = kind;
  show_agent_strip_ = skin_config_.show_agent_strip;
  show_work_panel_ = skin_config_.show_work_panel;
  diffs_expanded_ = !skin_config_.diffsCollapsedByDefault();
  UIState::instance().diffsExpanded = diffs_expanded_;
  if (input_model_) {
    input_model_->compact_mode = skin_config_.compact_input;
  }
  if (status_model_) {
    status_model_->compact_skin_mode = skin_config_.compactStatusBar();
  }
  persistUserPreferences();
  if (chat_component_) {
    chat_component_->OnEvent(ftxui::Event::Special("ThemeChanged"));
  }
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
}

const SkinConfig &TuiState::skinConfig() const { return skin_config_; }

void TuiState::activatePermissionRequest(
    const shared::PermissionEscalationRequest &request) {
  pending_permission_request_ = request;
  pending_permission_labels_ = permissionOptionLabels(request);
  pending_permission_responses_ = permissionOptionResponses(request);
  pending_permission_option_boxes_.assign(pending_permission_labels_.size(),
                                         ftxui::Box{});
  pending_permission_selected_ = 0;
  }

void TuiState::clearActivePermissionRequest() {
  pending_permission_request_.reset();
  pending_permission_labels_.clear();
  pending_permission_responses_.clear();
  pending_permission_option_boxes_.clear();
  pending_permission_selected_ = 0;
}

void TuiState::promoteNextPermissionRequest() {
  if (pending_permission_queue_.empty()) {
    clearActivePermissionRequest();
    return;
  }

  const auto nextRequest = pending_permission_queue_.front();
  pending_permission_queue_.erase(pending_permission_queue_.begin());
  activatePermissionRequest(nextRequest);
}

void TuiState::setViewMode(ViewMode mode) { view_mode_ = mode; }

TuiState::ViewMode TuiState::getViewMode() const { return view_mode_; }

void TuiState::openModal(const std::string &name) {
  firmius::tui::ModalRegistry::instance().openModal(name, *this, true);
}

void TuiState::openModalDirect(ftxui::Component modal,
                               const std::string &modal_name) {
  modals_.push_back(modal);
  if (pending_modal_teardown_) {
    modal_teardowns_.push_back(std::move(pending_modal_teardown_));
    pending_modal_teardown_ = nullptr;
  } else {
    modal_teardowns_.push_back({});
  }
  if (!modal_name.empty()) {
    pending_profile_modal_name_ = modal_name;
    painted_profile_modals_.erase(modal_name);
  }
  if (modal) {
    modal->TakeFocus();
  }
}

void TuiState::popModal() { postEvent(ftxui::Event::Special("PopModal")); }

void TuiState::popModalImmediate() {
  if (!modals_.empty()) {
    if (!modal_teardowns_.empty()) {
      auto teardown = std::move(modal_teardowns_.back());
      modal_teardowns_.pop_back();
      if (teardown) {
        teardown();
      }
    }
    modals_.pop_back();
  }
  if (modals_.empty()) {
    if (input_component_) {
      input_component_->TakeFocus();
    }
    // Also explicitly post an event to ensure the screen re-renders and focus
    // is acknowledged
    postEvent(ftxui::Event::Custom);
  } else {
    modals_.back()->TakeFocus();
  }
}

void TuiState::replaceModalDirect(ftxui::Component modal) {
  if (!modals_.empty()) {
    if (!modal_teardowns_.empty()) {
      auto teardown = std::move(modal_teardowns_.back());
      modal_teardowns_.pop_back();
      if (teardown) {
        teardown();
      }
    }
    modals_.pop_back();
  }
  modals_.push_back(modal);
  if (pending_modal_teardown_) {
    modal_teardowns_.push_back(std::move(pending_modal_teardown_));
    pending_modal_teardown_ = nullptr;
  } else {
    modal_teardowns_.push_back({});
  }
  if (modal) {
    modal->TakeFocus();
  }
}

void TuiState::clearModals() {
  while (!modals_.empty()) {
    popModalImmediate();
  }
}

void TuiState::pushModalTeardown(std::function<void()> teardown) {
  if (!teardown) {
    return;
  }
  pending_modal_teardown_ = std::move(teardown);
}

void TuiState::deferUiMutation(std::function<void()> action) {
  if (!action) {
    return;
  }
  deferred_ui_mutations_.push_back(std::move(action));
  postEvent(ftxui::Event::Custom);
}

void TuiState::postEvent(ftxui::Event event) {
  if (!screen_) {
    return;
  }
  if (event == ftxui::Event::Custom) {
    if (custom_event_pending_) {
      return;
    }
    custom_event_pending_ = true;
    noteTuiCustomEventPosted();
  }
  screen_->PostEvent(event);
}

bool TuiState::cycleThreadPermissionMode() {
  if (!harness_ || thread_.threadId.empty()) {
    thread_.permissionMode = nextPermissionMode(thread_.permissionMode);
    persistUserPreferences();
    updateStatusModel();
    NotificationManager::instance().notifyInfo(
        "Permissions",
        "New threads: " + permissionModeToDisplayName(thread_.permissionMode),
        std::chrono::milliseconds(1600));
    if (screen_) {
      postEvent(ftxui::Event::Custom);
    }
    return true;
  }

  thread_.permissionMode = nextPermissionMode(thread_.permissionMode);
  persistUserPreferences();
  updateStatusModel();
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
  harness_->cycleCurrentThreadPermissionMode();
  return true;
}

bool TuiState::hasActiveThread() const { return !thread_.threadId.empty(); }

std::string TuiState::currentThreadId() const { return thread_.threadId; }

shared::ThreadPermissionMode TuiState::currentThreadPermissionMode() const {
  return thread_.permissionMode;
}

bool TuiState::needsAnimationTick() const {
  return screen_ != nullptr;
}

void TuiState::init(firmius::core::Harness &harness,
                    const shared::ThreadMetadata &thread,
                    const std::string &focused_agent_id) {
  harness_ = &harness;
  thread_ = thread;
  session_metrics_ = {};
  quit_arm_deadline_.reset();
  quit_arm_generation_ = 0;
  if (thread_.threadId.empty()) {
    const auto preferences = firmius::tui::loadUserPreferences();
    if (preferences.preferred_permission_mode.has_value()) {
      thread_.permissionMode = *preferences.preferred_permission_mode;
    }
  }
  focused_agent_id_ = focused_agent_id;
  if (harness_ && !focused_agent_id_.empty() &&
      harness_->focusedAgentId() != focused_agent_id_) {
    harness_->setFocusedAgent(focused_agent_id_);
  }
  refreshFocusedHistory();

  title_model_ = std::make_shared<TitleBarModel>();
  title_model_->title = thread_.title;
  title_model_->thread_id = thread_.threadId;

  status_model_ = std::make_shared<StatusBarModel>();
  status_model_->status_text = "idle";
  status_model_->compact_skin_mode = skin_config_.compactStatusBar();

  input_model_ = std::make_shared<InputBarModel>();
  input_model_->buffer = &input_;
  input_model_->cursor = &cursor_;
  input_model_->placeholder = "Type a message...";
  input_model_->compact_mode = skin_config_.compact_input;

  // Set up vision capability check
  input_model_->check_vision_capable = [this]() -> bool {
    auto normalize_model_key = [](const std::string &value) {
      std::string normalized;
      normalized.reserve(value.size());
      for (unsigned char c : value) {
        if (std::isalnum(c)) {
          normalized.push_back(static_cast<char>(std::tolower(c)));
        }
      }
      return normalized;
    };

    auto supports_vision = [normalize_model_key](
                               const std::string &provider_id,
                               const std::string &model_id) -> bool {
      if (provider_id.empty() || model_id.empty()) {
        return false;
      }
      auto provider = firmius::provider::ProviderRegistry::instance().getProvider(
          provider_id);
      if (!provider) {
        return false;
      }
      auto info = provider->getModelInfo(model_id);
      auto supports_image = [](const firmius::shared::ModelInfo &model_info) {
        return std::find(model_info.modalities.begin(), model_info.modalities.end(),
                         "image") != model_info.modalities.end();
      };
      if (supports_image(info)) {
        return true;
      }

      // Fallback for model aliases/variants where getModelInfo() returns a
      // generic model without modalities.
      const std::string requested_key = normalize_model_key(model_id);
      if (requested_key.empty()) {
        return false;
      }
      for (const auto &candidate : provider->listModels()) {
        if (!supports_image(candidate)) {
          continue;
        }
        const std::string candidate_key = normalize_model_key(candidate.id);
        if (candidate_key.empty()) {
          continue;
        }
        if (requested_key == candidate_key ||
            requested_key.rfind(candidate_key, 0) == 0) {
          return true;
        }
      }
      return false;
    };

    if (!harness_) {
      return false;
    }

    if (!focused_agent_id_.empty()) {
      auto agent = firmius::core::AgentRegistry::instance().getAgent(
          focused_agent_id_);
      if (agent) {
        auto &ctx =
            const_cast<firmius::shared::AgentContext &>(agent->getContext());
        if (supports_vision(ctx.config.providerId, ctx.config.modelId)) {
          return true;
        }
      }
    }

    if (status_model_) {
      const std::string &status_model_name = status_model_->model_name;
      const size_t slash = status_model_name.find('/');
      if (slash != std::string::npos && slash > 0 &&
          slash + 1 < status_model_name.size()) {
        const std::string provider_id = status_model_name.substr(0, slash);
        const std::string model_id = status_model_name.substr(slash + 1);
        if (supports_vision(provider_id, model_id)) {
          return true;
        }
      }
    }

    const auto &cfg = harness_->getConfig();
    return supports_vision(cfg.defaultProviderId, cfg.defaultModelId);
  };

  // Set up notification function
  input_model_->show_notification = [](const std::string &title,
                                       const std::string &message) {
    NotificationManager::instance().notifyError(title, message, false);
  };

  input_model_->complete_file_references = [this](const std::string &query) {
    const std::filesystem::path root = thread_.cwd.empty()
                                           ? std::filesystem::current_path()
                                           : std::filesystem::path(thread_.cwd);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
      file_reference_cache_ready_ = false;
      file_reference_cache_paths_.clear();
      return std::vector<std::string>{};
    }

    if (!file_reference_cache_ready_ || file_reference_cache_root_ != root) {
      file_reference_cache_root_ = root;
      file_reference_cache_paths_.clear();

      size_t visited = 0;
      for (std::filesystem::recursive_directory_iterator it(
               root, std::filesystem::directory_options::skip_permission_denied,
               ec),
           end;
           it != end; it.increment(ec)) {
        if (++visited > 20000 || file_reference_cache_paths_.size() >= 1000) {
          break;
        }
        if (ec) {
          ec.clear();
          continue;
        }
        const auto name = it->path().filename().string();
        if (it->is_directory(ec)) {
          // Skip hidden directories and known heavyweight/generated dirs
          if ((!name.empty() && name[0] == '.') ||
              name == "node_modules" || name == "build" || name == "dist" ||
              name == "__pycache__" || name == "target" || name == "vendor" ||
              name == "_build" || name == "out" || name == "coverage" ||
              name == "uploads" || name == ".output" || name == "CMakeFiles") {
            it.disable_recursion_pending();
          }
          continue;
        }
        if (!it->is_regular_file(ec)) {
          continue;
        }

        const auto rel = std::filesystem::relative(it->path(), root, ec);
        if (ec) {
          ec.clear();
          continue;
        }
        file_reference_cache_paths_.push_back(rel.generic_string());
      }
      file_reference_cache_ready_ = true;
    }
    return BuildFileReferenceSuggestions(file_reference_cache_paths_, query, 8);
  };

  input_model_->complete_artifact_references = [this](const std::string &query) {
    if (!harness_ || thread_.threadId.empty()) {
      return std::vector<std::string>{};
    }

    const auto artifacts = harness_->listArtifacts(thread_.threadId);
    return BuildArtifactReferenceSuggestions(artifacts, query, 8);
  };

  agent_strip_model_ = std::make_shared<AgentStripModel>();
  agent_strip_model_->on_item_click = [this](const std::string& agent_id) {
    focusAgent(agent_id);
  };

  plan_lane_model_.reset();
  todo_lane_model_ = std::make_shared<TodoLaneModel>();
  context_lane_model_ = std::make_shared<ContextLaneModel>();
  active_plan_state_.setExpanded(true);
  active_plan_state_.hydrateForThread(thread_, loadActivePlanForThread(thread_));
  updateTodoLaneModel();
  updateContextLaneModel();

  subscription_id_ =
      harness_->subscribe([this](const firmius::shared::AppEvent &ev) {
        event_queue_.push(ev);
        noteTuiAppEventEnqueued();
        if (screen_) {
          postEvent(ftxui::Event::Custom);
        }
      });

  requestRefresh(RefreshFlags::Status);
  requestRefresh(RefreshFlags::AgentStrip);
  requestRefresh(RefreshFlags::TodoLane);
  requestRefresh(RefreshFlags::ContextLane);
  applyPendingRefreshes();
}

bool TuiState::focusAgent(const std::string &agent_id) {
  if (!harness_ || agent_id.empty() || !harness_->setFocusedAgent(agent_id)) {
    return false;
  }
  focused_agent_id_ = agent_id;
  refreshFocusedHistory();
  if (const auto *stream = stream_state_.getStream(focused_agent_id_)) {
    auto agent = firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
    if (agent) {
      live_row_current_phrase_ = inferClaudexActivity(
          agent->getContext(), stream, live_row_current_phrase_);
      live_row_last_phrase_key_ = live_row_current_phrase_;
    }
  }
  requestRefresh(RefreshFlags::AgentStrip);
  requestRefresh(RefreshFlags::Status);
  requestRefresh(RefreshFlags::TodoLane);
  requestRefresh(RefreshFlags::ContextLane);
  notifyChatTranscriptChanged();
  applyPendingRefreshes();
  return true;
}

void TuiState::requestAnimationTick(std::chrono::milliseconds interval,
                                    std::chrono::milliseconds ttl) {
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(animation_tick_mutex_);
    if (animation_tick_interval_.count() == 0) {
      animation_tick_interval_ = interval;
    } else {
      animation_tick_interval_ = std::min(animation_tick_interval_, interval);
    }
    animation_tick_until_ = std::max(animation_tick_until_, now + ttl);
    ++animation_tick_generation_;
  }
  if (screen_) {
    screen_->RequestAnimationFrame();
    postEvent(ftxui::Event::Custom);
  }
  if (animation_tick_thread_.joinable()) {
    return;
  }
  animation_tick_thread_ = std::jthread([this](std::stop_token st) {
    uint64_t last_generation = 0;
    while (!st.stop_requested()) {
      std::chrono::milliseconds interval{250};
      std::chrono::steady_clock::time_point until{};
      uint64_t generation = 0;
      {
        std::lock_guard<std::mutex> lock(animation_tick_mutex_);
        interval = animation_tick_interval_.count() > 0
                       ? animation_tick_interval_
                       : std::chrono::milliseconds(250);
        until = animation_tick_until_;
        generation = animation_tick_generation_;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= until) {
        std::lock_guard<std::mutex> lock(animation_tick_mutex_);
        animation_tick_interval_ = std::chrono::milliseconds(0);
        return;
      }
      if (generation == last_generation) {
        std::this_thread::sleep_for(interval);
      }
      last_generation = generation;
      if (screen_) {
        screen_->RequestAnimationFrame();
        postEvent(ftxui::Event::Custom);
      }
    }
  });
}

void TuiState::attachScreen(ftxui::ScreenInteractive *screen) {
  if (animation_tick_thread_.joinable()) {
    animation_tick_thread_.request_stop();
    animation_tick_thread_.join();
  }
  screen_ = screen;

  if (!screen_) {
    return;
  }

  animation_tick_thread_ = std::jthread([this](std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      if (!needsAnimationTick()) {
        break;
      }

      // Keep purely visual animation ticking without consuming the coalesced
      // Custom-event wakeup used for app-event delivery.
      ftxui::animation::RequestAnimationFrame();
      std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
  });
}

void TuiState::shutdown() {
  if (animation_tick_thread_.joinable()) {
    animation_tick_thread_.request_stop();
    animation_tick_thread_.join();
  }
  screen_ = nullptr;
  if (harness_ && subscription_id_ >= 0) {
    harness_->unsubscribe(subscription_id_);
    subscription_id_ = -1;
  }
}

InputBarModel& TuiState::getInputBarModel() {
  return *input_model_;
}

void TuiState::clearInputBuffer() {
  if (input_model_ && input_model_->buffer) {
    input_model_->buffer->clear();
  }
  if (input_model_ && input_model_->cursor) {
    *input_model_->cursor = 0;
  }
  if (input_model_) {
    input_model_->pasted_blocks.clear();
  }
}

bool TuiState::handleCtrlC() {
  const auto now = std::chrono::steady_clock::now();
  if (quit_arm_deadline_.has_value() && now <= *quit_arm_deadline_) {
    requestQuit();
    return true;
  }

  clearInputBuffer();
  quit_arm_deadline_ = now + std::chrono::seconds(4);
  const std::size_t generation = ++quit_arm_generation_;
  NotificationManager::instance().notifyWarning(
      "Exit Armed", "Press Ctrl+C again within 4s to quit.",
      std::chrono::milliseconds(1800));
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
  // Join any existing thread before starting a new one
  if (quit_arm_thread_.joinable()) {
    quit_arm_thread_.request_stop();
    quit_arm_thread_.join();
  }
  quit_arm_thread_ = std::jthread([this, generation](std::stop_token st) {
    for (int i = 0; i < 40 && !st.stop_requested(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (quit_arm_generation_ != generation) {
        return;
      }
    }
    if (st.stop_requested() || quit_arm_generation_ != generation) {
      return;
    }
    quit_arm_deadline_.reset();
    if (screen_) {
      postEvent(ftxui::Event::Custom);
    }
  });
  return true;
}

void TuiState::requestQuit() {
  quit_arm_deadline_.reset();
  ++quit_arm_generation_;
  if (screen_) {
    screen_->ExitLoopClosure()();
  }
}

bool TuiState::isQuitArmed() const {
  return quit_arm_deadline_.has_value() &&
         std::chrono::steady_clock::now() <= *quit_arm_deadline_;
}

std::string TuiState::exitSummaryText() const {
  std::ostringstream out;
  out << "\nFirmius Session Summary\n";
  out << "Thread: "
      << (thread_.title.empty() ? "New Thread" : thread_.title) << "\n";
  out << "Thread ID: "
      << (thread_.threadId.empty() ? "(none)" : thread_.threadId) << "\n";
  out << "Prompt tokens: " << session_metrics_.tokens.prompt << "\n";
  out << "Completion tokens: " << session_metrics_.tokens.completion << "\n";
  out << "Reasoning tokens: " << session_metrics_.tokens.reasoning << "\n";
  out << "Total billed tokens: " << session_metrics_.tokens.total << "\n";
  out << "Estimated cost: $" << std::fixed << std::setprecision(4)
      << session_metrics_.estimatedCostUsd << "\n";
  if (!session_metrics_.context.empty()) {
    out << "Context summary: "
        << firmius::core::summarizeContextWindowMetrics(
               session_metrics_.context, 4)
        << "\n";
  }
  const std::string profiling = tuiProfilingSummaryText();
  if (!profiling.empty()) {
    out << profiling;
  }
  return out.str();
}

void TuiState::drainEvents() {
  drainDeferredUiMutations();
  custom_event_pending_ = false;
  noteTuiCustomEventDrained();

  // Drain until stable. New app events can arrive while we are processing the
  // current batch; if we only drain once, Custom-event coalescing can leave
  // those later events stranded until some unrelated user input posts another
  // wakeup. That manifests as transcripts/live tool/process output only
  // appearing after a manual keypress.
  while (true) {
    auto drained = event_queue_.drainAll();
    if (drained.empty()) {
      break;
    }
    for (const auto &ev : drained) {
      onEvent(ev);
    }
  }
  applyPendingRefreshes();
  applyPendingRenders();
}

void TuiState::requestRefresh(RefreshFlags flags) {
  pending_refresh_flags_ |= static_cast<unsigned int>(flags);
}

void TuiState::notifyChatTranscriptChanged() {
  requestRefresh(RefreshFlags::ChatTranscript);
  requestRender(RefreshFlags::ChatTranscript);
}

void TuiState::applyPendingRefreshes() {
  const unsigned int flags = pending_refresh_flags_;
  if (flags == 0) {
    return;
  }
  pending_refresh_flags_ = 0;

  if (flags & static_cast<unsigned int>(RefreshFlags::Status)) {
    updateStatusModel();
  }
  if (flags & static_cast<unsigned int>(RefreshFlags::AgentStrip)) {
    updateAgentStripModel();
  }
  if (flags & static_cast<unsigned int>(RefreshFlags::ContextLane)) {
    updateContextLaneModel();
  }
  if ((flags & static_cast<unsigned int>(RefreshFlags::ChatTranscript)) &&
      chat_component_) {
    chat_component_->OnEvent(ftxui::Event::Special("TranscriptChanged"));
  }
}

void TuiState::handleAppEvent(const shared::AppEvent &ev) {
  onEvent(ev);
}

void TuiState::onEvent(const shared::AppEvent &ev) {
  const auto on_event_begin = std::chrono::steady_clock::now();
  requestRefresh(RefreshFlags::Status);
  requestRefresh(RefreshFlags::ContextLane);

  std::visit(
      [&](auto &&e) {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, AgentThinking>) {
          stream_state_.handleAgentThinking(e);
          requestRefresh(RefreshFlags::AgentStrip);
          requestRefresh(RefreshFlags::Status);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentText>) {
          stream_state_.handleAgentText(e);
          requestRefresh(RefreshFlags::AgentStrip);
          requestRefresh(RefreshFlags::Status);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentTurnCompleted>) {
          stream_state_.handleAgentTurnCompleted(e);
          session_metrics_ += e.turn.metrics;
          // Append completed turns to the in-memory transcript immediately.
          // Live stream rows are cleared on completion, so without this the UI
          // can briefly render neither live nor persisted content.
          if (e.agentId == focused_agent_id_) {
            auto transcript_history =
                EnsureTranscriptHistory(history_, thread_.threadId);
            transcript_history->turns.push_back(e.turn);
            agent_history_cache_[focused_agent_id_] = transcript_history;
            agent_persisted_tool_call_ids_cache_[focused_agent_id_] =
                firmius::tui::CollectToolCallIdsFromHistory(
                    transcript_history.get());
          }
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentProviderWaiting>) {
          stream_state_.handleAgentProviderWaiting(e);
        } else if constexpr (std::is_same_v<T, AgentToolCallChunk>) {
          stream_state_.handleAgentToolCallChunk(e);
          requestRefresh(RefreshFlags::AgentStrip);
          requestRefresh(RefreshFlags::Status);
          requestRefresh(RefreshFlags::ContextLane);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentToolCall>) {
          stream_state_.handleAgentToolCall(e);
          requestRefresh(RefreshFlags::AgentStrip);
          requestRefresh(RefreshFlags::Status);
          requestRefresh(RefreshFlags::ContextLane);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentFileEdited>) {
          stream_state_.handleAgentFileEdited(e);
        } else if constexpr (std::is_same_v<T, ThreadChanged>) {
          const auto thread_changed_begin = std::chrono::steady_clock::now();
          thread_ = e.metadata;
          const auto current_focused = harness_ ? harness_->focusedAgentId() : std::string();
          auto all_agents = harness_ ? harness_->listAgents(thread_.threadId)
                                     : std::vector<std::string>{};

          auto is_valid_focus = [&](const std::string &agent_id) {
            if (agent_id.empty()) {
              return false;
            }
            if (std::find(all_agents.begin(), all_agents.end(), agent_id) ==
                all_agents.end()) {
              return false;
            }
            return static_cast<bool>(
                firmius::core::AgentRegistry::instance().getAgent(agent_id));
          };

          if (is_valid_focus(current_focused)) {
            focused_agent_id_ = current_focused;
          } else {
            focused_agent_id_.clear();
            for (const auto &candidate : all_agents) {
              auto agent = firmius::core::AgentRegistry::instance().getAgent(candidate);
              if (agent && agent->getContext().identity.parentId.empty()) {
                focused_agent_id_ = candidate;
                break;
              }
            }
            if (focused_agent_id_.empty() && !all_agents.empty()) {
              focused_agent_id_ = all_agents.front();
            }
            if (harness_ && !focused_agent_id_.empty()) {
              harness_->setFocusedAgent(focused_agent_id_);
            }
          }

          pending_modal_clear_ = true;
          stream_state_.handleThreadChanged();

          agent_history_cache_.clear();
          agent_persisted_tool_call_ids_cache_.clear();

          refreshFocusedHistory();

          if (!focused_agent_id_.empty()) {
            if (history_) {
              auto rebuild_begin = std::chrono::steady_clock::now();
              stream_state_.rebuildToolCallsFromHistory(focused_agent_id_, history_.get(),
                                                        thread_.threadId, false);
              noteTuiRebuildToolCalls(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - rebuild_begin));
            } else {
              auto agent =
                  firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
              const auto *live_history =
                  agent && agent->getContext().history ? agent->getContext().history.get()
                                                      : nullptr;
              if (live_history && !live_history->turns.empty()) {
                auto rebuild_begin = std::chrono::steady_clock::now();
                stream_state_.rebuildToolCallsFromHistory(focused_agent_id_, live_history,
                                                          thread_.threadId, false);
                noteTuiRebuildToolCalls(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - rebuild_begin));
              }
            }
          }

          if (title_model_) {
            title_model_->title = thread_.title;
            title_model_->thread_id = thread_.threadId;
          }
          active_plan_state_.setExpanded(true);
          active_plan_state_.hydrateForThread(thread_, loadActivePlanForThread(thread_));
          setViewMode(ViewMode::Chat);

          requestRefresh(RefreshFlags::Status);
          requestRefresh(RefreshFlags::TodoLane);
          requestRefresh(RefreshFlags::ContextLane);
          notifyChatTranscriptChanged();

          noteTuiThreadChanged(std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - thread_changed_begin));
        } else if constexpr (std::is_same_v<T, ModelSwitched>) {
          requestRefresh(RefreshFlags::Status);
          requestRefresh(RefreshFlags::AgentStrip);
          requestRefresh(RefreshFlags::ContextLane);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, ThreadMetadataUpdated>) {
          if (e.threadId == thread_.threadId) {
            auto previousMode = thread_.permissionMode;
            thread_ = e.metadata;
            if (title_model_) {
              title_model_->title = thread_.title;
              title_model_->thread_id = thread_.threadId;
            }
            if (previousMode != thread_.permissionMode) {
              NotificationManager::instance().notifyInfo(
                  "Permissions",
                  "Thread mode: " +
                      permissionModeToDisplayName(thread_.permissionMode),
                  std::chrono::milliseconds(1500));
              requestRefresh(RefreshFlags::Status);
            }
          }
        } else if constexpr (std::is_same_v<T, PermissionEscalationRequest>) {
          auto sameRequestId = [&](const shared::PermissionEscalationRequest &request) {
            return request.requestId == e.requestId;
          };
          if (pending_permission_request_ &&
              pending_permission_request_->requestId == e.requestId) {
            activatePermissionRequest(e);
          } else if (std::find_if(pending_permission_queue_.begin(),
                                  pending_permission_queue_.end(),
                                  sameRequestId) == pending_permission_queue_.end()) {
            if (!pending_permission_request_) {
              activatePermissionRequest(e);
            } else {
              pending_permission_queue_.push_back(e);
            }
          }
          if (screen_) {
            postEvent(ftxui::Event::Custom);
          }
        } else if constexpr (std::is_same_v<T, ConfigUpdated>) {
          requestRefresh(RefreshFlags::Status);
          requestRefresh(RefreshFlags::ContextLane);
        } else if constexpr (std::is_same_v<T, PermissionEscalationResolved>) {
          if (pending_permission_request_ &&
              pending_permission_request_->requestId == e.requestId) {
            promoteNextPermissionRequest();
          } else {
            auto it = std::remove_if(
                pending_permission_queue_.begin(), pending_permission_queue_.end(),
                [&](const shared::PermissionEscalationRequest &request) {
                  return request.requestId == e.requestId;
                });
            if (it != pending_permission_queue_.end()) {
              pending_permission_queue_.erase(it,
                                              pending_permission_queue_.end());
            }
          }
          NotificationManager::instance().notifyInfo(
              "Permission",
              permissionResponseToDisplayName(e.response),
              std::chrono::milliseconds(1200));
        } else if constexpr (std::is_same_v<T, ThreadLocked>) {
          auto locked_modal =
              ThreadLockedModal::create(*this, e.threadId, e.ownerPid);
          openModalDirect(locked_modal);
        } else if constexpr (std::is_same_v<T, AgentCompacting>) {
          stream_state_.handleAgentCompacting(e);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentCompactionThinking>) {
          stream_state_.handleAgentCompactionThinking(e);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentCompactionText>) {
          stream_state_.handleAgentCompactionText(e);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, ContextCompacted>) {
          stream_state_.handleContextCompacted(e);
          if (e.agentId.empty() || e.agentId == focused_agent_id_) {
            refreshFocusedHistory();
            notifyChatTranscriptChanged();
          }
        } else if constexpr (std::is_same_v<T, AgentProcessSpawned>) {
          stream_state_.handleAgentProcessSpawned(e);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentProcessOutput>) {
          stream_state_.handleAgentProcessOutput(e);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentSpawned>) {
          if (e.parentId.empty() && focused_agent_id_.empty()) {
            focused_agent_id_ = e.agentId;
            if (harness_) {
              harness_->setFocusedAgent(e.agentId);
            }
            refreshFocusedHistory();
            notifyChatTranscriptChanged();
          } else if (!e.parentId.empty() && focused_agent_id_ == e.parentId) {
            // Auto-focus subagent when its parent is currently focused, so its
            // streaming output appears in the chat.
            focused_agent_id_ = e.agentId;
            if (harness_) {
              harness_->setFocusedAgent(e.agentId);
            }
            refreshFocusedHistory();
            notifyChatTranscriptChanged();
          }
          stream_state_.handleAgentSpawned(e, focused_agent_id_);
          requestRefresh(RefreshFlags::AgentStrip);
        } else if constexpr (std::is_same_v<T, UserMessageSent>) {
          if (focused_agent_id_.empty()) {
            focused_agent_id_ = harness_ ? harness_->focusedAgentId() : std::string();
            if (focused_agent_id_.empty() && harness_ && !thread_.threadId.empty()) {
              auto all_agents = harness_->listAgents(thread_.threadId);
              for (const auto &candidate : all_agents) {
                auto agent = firmius::core::AgentRegistry::instance().getAgent(candidate);
                if (agent && agent->getContext().identity.parentId.empty()) {
                  focused_agent_id_ = candidate;
                  harness_->setFocusedAgent(candidate);
                  break;
                }
              }
            }
            refreshFocusedHistory();
          }
          if (e.threadId == thread_.threadId) {
            auto transcript_history =
                EnsureTranscriptHistory(history_, thread_.threadId);
            bool replaced_optimistic = false;
            if (!transcript_history->turns.empty()) {
              auto &last_turn = transcript_history->turns.back();
              if (last_turn.turnId.rfind("user-optimistic-", 0) == 0 &&
                  last_turn.messages.size() == 1 &&
                  last_turn.messages.front().role == shared::Role::User) {
                std::string last_text;
                for (const auto &part : last_turn.messages.front().content) {
                  if (const auto *txt =
                          std::get_if<shared::TextContent>(&part)) {
                    last_text += txt->text;
                  }
                }
                if (last_text == e.text) {
                  last_turn.turnId = "user-live-" + e.messageId;
                  last_turn.messages.front().id = e.messageId;
                  replaced_optimistic = true;
                }
              }
            }
            if (!replaced_optimistic) {
              shared::AgentTurn turn;
              turn.turnId = "user-live-" + e.messageId;
              shared::Message message;
              message.id = e.messageId;
              message.role = shared::Role::User;
              message.timestamp = static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count());
              message.content = {shared::TextContent{e.text}};
              for (const auto &img : e.images) {
                message.content.push_back(img);
              }
              turn.messages.push_back(std::move(message));
              transcript_history->turns.push_back(std::move(turn));
            }
            if (!focused_agent_id_.empty()) {
              agent_history_cache_[focused_agent_id_] = transcript_history;
            }
          }
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, HistoryUndone>) {
          if (e.threadId == thread_.threadId &&
              (e.agentId.empty() || e.agentId == focused_agent_id_)) {
            if (suppress_next_history_undone_refresh_) {
              suppress_next_history_undone_refresh_ = false;
            } else {
              refreshFocusedHistory();
            }
            requestRefresh(RefreshFlags::Status);
            requestRefresh(RefreshFlags::AgentStrip);
            requestRefresh(RefreshFlags::ContextLane);
          }
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentAccountSwitched>) {
          stream_state_.handleAgentAccountSwitched(e);
          NotificationManager::instance().notifyInfo(
              "Account Switch", "Switched to " + e.accountLocator,
              std::chrono::milliseconds(4000));
        } else if constexpr (std::is_same_v<T, AgentError>) {
          stream_state_.handleAgentError(e);
          if (e.agentId.empty() && !e.message.empty()) {
            NotificationManager::instance().notifyWarning(
                "Thread Recovery", e.message, std::chrono::milliseconds(5000));
          } else if (detail::shouldNotifyHiddenChatError(
                         focused_agent_id_, e.agentId,
                         harness_ ? harness_->getConfig().hideErrors : false) &&
                     !e.message.empty()) {
            NotificationManager::instance().notifyError("Agent Error",
                                                        e.message, false);
          }
          requestRefresh(RefreshFlags::AgentStrip);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentRetrying>) {
          stream_state_.handleAgentRetrying(e);
          std::string retryMsg = "Attempt " + std::to_string(e.attempt) + "/" +
                                 std::to_string(e.maxAttempts);
          if (!e.reason.empty())
            retryMsg += " - " + e.reason;
          if (e.delayMs > 0)
            retryMsg += " (~" + std::to_string(e.delayMs / 1000) + "s)";
          NotificationManager::instance().notifyWarning(
              "Retrying", retryMsg,
              std::chrono::milliseconds(std::max(e.delayMs, 3000)));
          requestRefresh(RefreshFlags::AgentStrip);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentRetryFailed>) {
          stream_state_.handleAgentRetryFailed(e);
          NotificationManager::instance().notifyError(
              "Retry Failed",
              e.reason.empty() ? "All retry attempts exhausted" : e.reason,
              false);
          requestRefresh(RefreshFlags::AgentStrip);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentFinished>) {
          stream_state_.handleAgentFinished(e);
          requestRefresh(RefreshFlags::AgentStrip);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, AgentInterrupted>) {
          stream_state_.handleAgentInterrupted(e);
          requestRefresh(RefreshFlags::AgentStrip);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, ThreadTitleUpdated>) {
          if (title_model_) {
            title_model_->title = e.title;
          }
          thread_.title = e.title;
        } else if constexpr (std::is_same_v<T, MessageQueued>) {
          stream_state_.handleMessageQueued(e);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, MessageDequeued>) {
          stream_state_.handleMessageDequeued(e);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, InternalMessageQueued>) {
          stream_state_.handleInternalMessageQueued(e);
          notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, InternalMessageDequeued>) {
          stream_state_.handleInternalMessageDequeued(e);
          notifyChatTranscriptChanged();
        }
      },
      ev);

  notifyChatTranscriptChanged();
  applyPendingRefreshes();
  applyPendingRenders();

  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }

  noteTuiOnEventDispatch(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - on_event_begin));
}

std::string TuiState::statusText() const {
  if (focused_agent_id_.empty()) {
    return "idle";
  }
  auto agent = firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
  if (!agent) {
    return "idle";
  }
  const auto &ctx = agent->getContext();
  if (agent->isRunning() && ctx.state.currentStatus == AgentStatus::Idle) {
    const auto *stream = stream_state_.getStream(focused_agent_id_);
    return (stream && stream->provider_waiting) ? "provider_waiting"
                                                : "streaming";
  }
  return statusToString(ctx.state.currentStatus);
}

void TuiState::updateStatusModel() {
  if (!status_model_)
    return;
  status_model_->permission_mode = thread_.permissionMode;
  status_model_->active_mode.clear();
  status_model_->active_mode_glyph.clear();
  status_model_->live_processes = 0;
  status_model_->background_processes = 0;
  if (!focused_agent_id_.empty()) {
    auto agent =
        firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
    if (agent) {
      const auto &ctx = agent->getContext();
      ProcessRuntimeSnapshot runtime_snapshot;
      runtime_snapshot.owned_process_ids = ctx.state.ownedProcesses;
      runtime_snapshot.blocking_process_ids = ctx.state.blockingProcessIds;
      const auto manager_blocking =
          agent->getEnvironment()->getProcessManager().getBlockingProcessIds();
      runtime_snapshot.blocking_process_ids.insert(
          runtime_snapshot.blocking_process_ids.end(), manager_blocking.begin(),
          manager_blocking.end());
      std::unordered_set<std::string> dedup_blocking(
          runtime_snapshot.blocking_process_ids.begin(),
          runtime_snapshot.blocking_process_ids.end());
      runtime_snapshot.blocking_process_ids.assign(dedup_blocking.begin(),
                                                   dedup_blocking.end());
      auto process_counts = stream_state_.getProcessCounts(
          focused_agent_id_, &runtime_snapshot,
          [&](const std::string &process_id) {
            try {
              return agent->getEnvironment()
                  ->getProcessManager()
                  .inspectProcess(process_id)
                  .running;
            } catch (...) {
              return false;
            }
          });
      status_model_->status_text = statusToString(ctx.state.currentStatus);
      if (agent->isRunning() &&
          ctx.state.currentStatus == AgentStatus::Idle) {
        const auto *stream = stream_state_.getStream(focused_agent_id_);
        status_model_->status_text =
            (stream && stream->provider_waiting) ? "provider_waiting"
                                                 : "streaming";
      }
      status_model_->model_name =
          ctx.config.providerId + "/" + ctx.config.modelId;
      status_model_->model_variant = ctx.config.modelVariant;
      status_model_->purpose = ctx.identity.role;
      status_model_->title = ctx.identity.friendlyName;
      status_model_->agent_name = ctx.identity.friendlyName.empty()
                                      ? ctx.identity.name
                                      : ctx.identity.friendlyName;
      const shared::AgentMetrics *latest_metrics =
          stream_state_.getLatestMetrics(focused_agent_id_);
      const shared::AgentMetrics &metrics =
          latest_metrics ? *latest_metrics : ctx.aggregateMetrics;
      status_model_->context_used = metrics.tokens.contextSize;
      status_model_->sent_prompt = metrics.context.sentTokens;
      status_model_->billed_prompt = metrics.context.billedPromptTokens;
      status_model_->completion_tokens = metrics.tokens.completion;
      status_model_->estimated_cost_usd = metrics.estimatedCostUsd;
      status_model_->bucket_summary =
          firmius::core::summarizeContextWindowMetrics(metrics.context, 3);
      const auto [activeMode, activeModeGlyph] =
          resolveStatusMode(ctx.state.activeMode, ctx.config.personaName);
      status_model_->active_mode = activeMode;
      status_model_->active_mode_glyph = activeModeGlyph;
      auto provider =
          firmius::provider::ProviderRegistry::instance().getProvider(
              ctx.config.providerId);
      if (provider) {
        auto info = provider->getModelInfo(ctx.config.modelId);
        status_model_->context_max = info.contextWindow;
        const auto quotaDisplay =
            resolveCurrentQuotaDisplay(provider, ctx.config.modelId);
        status_model_->account_label = quotaDisplay.accountLabel;
        status_model_->quota_usage = quotaDisplay.usageLabel;
      }
      status_model_->is_active = agent->isRunning();
      status_model_->live_processes = process_counts.live;
      status_model_->background_processes = process_counts.background;
      return;
    }
  }
  status_model_->status_text = "idle";
  std::string personaName = resolveDefaultLeadPersona(harness_);
  std::string personaTitle = resolvePersonaTitle(personaName);
  if (harness_) {
    const auto &cfg = harness_->getConfig();
    status_model_->model_name = cfg.defaultProviderId + "/" + cfg.defaultModelId;
    status_model_->model_variant = cfg.defaultModelVariant;
  } else {
    status_model_->model_name = "";
    status_model_->model_variant.clear();
  }
  status_model_->purpose = personaTitle;
  status_model_->title = "";
  status_model_->agent_name = personaTitle;
  const auto [initialMode, initialModeGlyph] =
      resolveStatusMode(thread_.initialMode, personaName);
  status_model_->active_mode = initialMode;
  status_model_->active_mode_glyph = initialModeGlyph;
  status_model_->context_used = 0;
  status_model_->context_max = 0;
  status_model_->sent_prompt = 0;
  status_model_->billed_prompt = 0;
  status_model_->completion_tokens = 0;
  status_model_->estimated_cost_usd = 0.0;
  status_model_->account_label.clear();
  status_model_->quota_usage.clear();
  status_model_->bucket_summary.clear();
  status_model_->is_active = false;
  status_model_->live_processes = 0;
  status_model_->background_processes = 0;
}

void TuiState::updateAgentStripModel() {
  if (!agent_strip_model_)
    return;
  ++agent_strip_model_->layout_generation;
  agent_strip_model_->visible_rows =
      static_cast<size_t>(std::max(1, agent_strip_visible_rows_));
  agent_strip_model_->items.clear();
  if (focused_agent_id_.empty())
    return;

  auto countHistoryToolCalls =
      [](const firmius::shared::AgentHistory *history) {
        if (!history)
          return 0;
        int count = 0;
        for (const auto &turn : history->turns) {
          for (const auto &msg : turn.messages) {
            for (const auto &part : msg.content) {
              if (std::holds_alternative<firmius::shared::ToolCallContent>(
                      part))
                ++count;
            }
          }
        }
        return count;
      };

  std::unordered_map<std::string, int> live_tool_call_counts;
  const auto &tool_calls = stream_state_.getToolCalls();
  for (const auto &[_, view] : tool_calls) {
    if (!view || view->agentId.empty())
      continue;
    ++live_tool_call_counts[view->agentId];
  }

  auto all_ids = firmius::core::AgentRegistry::instance().listAll();
  std::string parent_focus = focused_agent_id_;
  std::string focused_parent;
  auto focused_agent =
      firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
  if (focused_agent) {
    focused_parent = focused_agent->getContext().identity.parentId;
  }
  bool has_children = false;
  for (const auto &id : all_ids) {
    auto child = firmius::core::AgentRegistry::instance().getAgent(id);
    if (!child)
      continue;
    if (child->getContext().identity.parentId == focused_agent_id_) {
      has_children = true;
      break;
    }
  }
  if (!has_children && !focused_parent.empty()) {
    parent_focus = focused_parent;
  }

  std::vector<AgentStripItem> all_items;
  all_items.reserve(all_ids.size());
  size_t focused_index = 0;
  bool focus_found = false;
  size_t candidate_index = 0;
  uint64_t now_ms =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
  for (const auto &id : all_ids) {
    auto child = firmius::core::AgentRegistry::instance().getAgent(id);
    if (!child)
      continue;
    const auto &ctx = child->getContext();
    if (ctx.identity.parentId != parent_focus)
      continue;
    AgentStripItem item;
    item.id = id;
    std::string display_title = ctx.identity.role;
    if (display_title.empty())
      display_title = ctx.identity.name;
    std::string stream_title = stream_state_.getAgentTitle(id);
    if (!stream_title.empty())
      display_title = stream_title;
    item.title = display_title;
    item.purpose = resolvePersonaTitle(ctx.config.personaName);
    item.model_name = ctx.config.modelId; // Use modelId directly,
                                          // PrettifyModelName handles prefixes
    item.model_variant = ctx.config.modelVariant;
    item.status_text =
        (child->isRunning() && ctx.state.currentStatus == AgentStatus::Idle)
            ? ((stream_state_.getStream(id) &&
                stream_state_.getStream(id)->provider_waiting)
                   ? "provider_waiting"
                   : "streaming")
            : statusToString(ctx.state.currentStatus);
    item.is_busy = child->isRunning();
    if (item.is_busy) {
      auto it = agent_work_start_ms_.find(id);
      if (it == agent_work_start_ms_.end()) {
        auto inserted = agent_work_start_ms_.emplace(id, now_ms);
        it = inserted.first;
      }
      item.working_since_ms = it->second;
    } else {
      agent_work_start_ms_.erase(id);
    }
    const shared::AgentMetrics *latest_metrics =
        stream_state_.getLatestMetrics(id);
    auto provider = firmius::provider::ProviderRegistry::instance().getProvider(
        ctx.config.providerId);
    if (provider) {
      auto info = provider->getModelInfo(ctx.config.modelId);
      if (info.contextWindow > 0) {
        const auto context_size =
            latest_metrics ? latest_metrics->tokens.contextSize
                           : ctx.aggregateMetrics.tokens.contextSize;
        item.context_percent =
            static_cast<float>(context_size) /
            info.contextWindow;
      }
    }
    int history_tool_calls =
        countHistoryToolCalls(child->getContext().history.get());
    auto live_it = live_tool_call_counts.find(id);
    int live_tool_calls =
        (live_it != live_tool_call_counts.end()) ? live_it->second : 0;
    item.tool_call_count = history_tool_calls + live_tool_calls;
    item.is_focused = focused_agent_id_ == id;
    
    // Calculate hierarchy depth
    item.parent_id = ctx.identity.parentId;
    item.hierarchy_depth = 0;
    std::string current_parent = ctx.identity.parentId;
    while (!current_parent.empty()) {
      item.hierarchy_depth++;
      auto parent = firmius::core::AgentRegistry::instance().getAgent(current_parent);
      if (parent) {
        current_parent = parent->getContext().identity.parentId;
      } else {
        break;
      }
    }
    
    // Check if this agent has children
    item.has_children = false;
    for (const auto &other_id : all_ids) {
      if (other_id == id) continue;
      auto other = firmius::core::AgentRegistry::instance().getAgent(other_id);
      if (other && other->getContext().identity.parentId == id) {
        item.has_children = true;
        break;
      }
    }
    
    if (item.is_focused) {
      focused_index = candidate_index;
      focus_found = true;
    }
    all_items.push_back(std::move(item));
    ++candidate_index;
  }

  const size_t total_items = all_items.size();
  if (total_items == 0) {
    agent_strip_model_->view_offset = 0;
    return;
  }

  agent_strip_model_->items = std::move(all_items);

  // Auto-scroll logic: scroll to the focused agent or the first busy agent
  int target_scroll = -1;
  if (focus_found) {
    target_scroll = static_cast<int>(focused_index);
  } else {
    for (size_t i = 0; i < agent_strip_model_->items.size(); ++i) {
      if (agent_strip_model_->items[i].is_busy) {
        target_scroll = static_cast<int>(i);
        break;
      }
    }
  }

  if (target_scroll != -1 && agent_strip_model_->on_scroll_request) {
    agent_strip_model_->on_scroll_request(target_scroll);
  }
}

void TuiState::updatePlanLaneModel() {
  if (!plan_lane_model_) {
    return;
  }
  plan_lane_model_->visible = false;
  plan_lane_model_->plan_id.clear();
  plan_lane_model_->plan_title.clear();
  plan_lane_model_->collapsed_summary.clear();
  plan_lane_model_->chunks.clear();
}

const shared::WorkChunk *
TuiState::findExecutorChunk(const std::optional<shared::Plan> & /*plan*/) const {
  return nullptr;
}

std::string TuiState::findExecutorChunkTitle(
    const std::optional<shared::Plan> & /*plan*/) const {
  return "";
}

void TuiState::updateTodoLaneModel() {
  if (!todo_lane_model_) {
    return;
  }

  todo_lane_model_->visible = false;
  todo_lane_model_->owner_label.clear();
  todo_lane_model_->show_chunk_header = false;
  todo_lane_model_->chunk_title.clear();
  todo_lane_model_->rows.clear();
  todo_lane_model_->toggle_hint.clear();

  if (thread_.threadId.empty() || focused_agent_id_.empty()) {
    return;
  }

  try {
    firmius::core::ThreadManager tm(firmiusThreadsPath());
    const auto todo = tm.getAgentTodo(thread_.threadId, focused_agent_id_);
    if (todo.items.empty()) {
      return;
    }

    todo_lane_model_->visible = true;
    todo_lane_model_->owner_label = focused_agent_id_.substr(0, 8);
    auto focusedAgent =
        firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
    if (focusedAgent) {
      const auto &ctx = focusedAgent->getContext();
      if (!ctx.identity.friendlyName.empty()) {
        todo_lane_model_->owner_label = ctx.identity.friendlyName;
      }
    }

    todo_lane_model_->rows.reserve(todo.items.size());
    for (const auto &item : todo.items) {
      todo_lane_model_->rows.push_back({item.id, item.text, item.status});
    }
    std::sort(todo_lane_model_->rows.begin(), todo_lane_model_->rows.end(),
              [](const TodoLaneRow &lhs, const TodoLaneRow &rhs) {
                return lhs.id < rhs.id;
              });
  } catch (...) {
  }
}

void TuiState::appendOptimisticUserTurn(
    const std::string &text,
    const std::vector<firmius::shared::ImageContent> &images) {
  appendOptimisticUserTurnImpl(history_, thread_.threadId, text, images);
  if (!focused_agent_id_.empty()) {
    agent_history_cache_[focused_agent_id_] = history_;
  }
}

void TuiState::updateContextLaneModel() {
  auto shortenBucketLabel = [](const std::string &label) {
    if (label == "system_prompt") return std::string("system");
    if (label == "conversation_history") return std::string("history");
    if (label == "rolling_observations") return std::string("memory");
    if (label == "rolling_status") return std::string("memory-status");
    if (label == "tool_results") return std::string("tools");
    if (label == "retrieval_results") return std::string("recall");
    if (label == "user_message") return std::string("user");
    if (label == "assistant_response") return std::string("assistant");
    return label;
  };

  if (!context_lane_model_) {
    return;
  }

  context_lane_model_->visible = false;
  context_lane_model_->owner_label.clear();
  context_lane_model_->toggle_hint.clear();
  context_lane_model_->model_label.clear();
  context_lane_model_->account_label.clear();
  context_lane_model_->quota_label.clear();
  context_lane_model_->context_ratio = 0.0f;
  context_lane_model_->context_label.clear();
  context_lane_model_->usage_label.clear();
  context_lane_model_->cost_label.clear();
  context_lane_model_->bucket_labels.clear();
  context_lane_model_->memory_labels.clear();
  context_lane_model_->rolling_memory = {};
  if (focused_agent_id_.empty()) {
    return;
  }

  auto agent =
      firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
  if (!agent) {
    return;
  }

  const auto &ctx = agent->getContext();
  const auto *liveMetrics = stream_state_.getLatestMetrics(focused_agent_id_);
  const auto &metrics = liveMetrics ? *liveMetrics : ctx.aggregateMetrics;

  auto provider = firmius::provider::ProviderRegistry::instance().getProvider(
      ctx.config.providerId);
  const auto quotaDisplay =
      resolveCurrentQuotaDisplay(provider, ctx.config.modelId);
  uint32_t contextWindow = 0;
  if (provider) {
    contextWindow = provider->getModelInfo(ctx.config.modelId).contextWindow;
  }

  context_lane_model_->visible = true;
  context_lane_model_->owner_label =
      (ctx.identity.friendlyName.empty() ? ctx.identity.name
                                         : ctx.identity.friendlyName) +
      " context";
  context_lane_model_->model_label =
      ctx.config.providerId + "/" + ctx.config.modelId;
  context_lane_model_->account_label = quotaDisplay.accountLabel;
  context_lane_model_->quota_label = quotaDisplay.usageLabel;
  if (contextWindow > 0) {
    context_lane_model_->context_ratio =
        static_cast<float>(metrics.tokens.contextSize) / contextWindow;
    context_lane_model_->context_label =
        formatCompactCount(metrics.tokens.contextSize) + "/" +
        formatCompactCount(contextWindow);
  } else if (metrics.tokens.contextSize > 0) {
    context_lane_model_->context_label =
        formatCompactCount(metrics.tokens.contextSize);
  }

  std::ostringstream usage;
  usage << "\xE2\x86\x91" << formatCompactCount(metrics.context.sentTokens);
  if (metrics.context.billedPromptTokens > 0 &&
      metrics.context.billedPromptTokens != metrics.context.sentTokens) {
    usage << "/" << formatCompactCount(metrics.context.billedPromptTokens);
  }
  usage << "  \xE2\x86\x93" << formatCompactCount(metrics.tokens.completion);
  context_lane_model_->usage_label = usage.str();

  if (metrics.estimatedCostUsd > 0.0) {
    std::ostringstream out;
    out << "$" << std::fixed << std::setprecision(4)
        << metrics.estimatedCostUsd;
    context_lane_model_->cost_label = out.str();
  }

  const auto rankedBuckets =
      firmius::core::rankContextBuckets(metrics.context);
  
  // Calculate total tokens across all buckets for ratio calculation
  uint32_t totalBucketTokens = 0;
  for (const auto &bucket : rankedBuckets) {
    const auto tokens = bucket.actualTokens > 0 ? bucket.actualTokens : bucket.estimatedTokens;
    totalBucketTokens += tokens;
  }
  
  // Populate context_buckets with ALL buckets in order
  context_lane_model_->context_buckets.clear();
  for (const auto &bucket : rankedBuckets) {
    const auto tokens = bucket.actualTokens > 0 ? bucket.actualTokens : bucket.estimatedTokens;
    if (tokens == 0) continue;
    
        ContextBucket cb;
    cb.label = shortenBucketLabel(bucket.label);
    cb.tokens = tokens;
    cb.ratio = totalBucketTokens > 0 ? static_cast<float>(tokens) / totalBucketTokens : 0.0f;
    context_lane_model_->context_buckets.push_back(cb);
  }
  
  // Keep old bucket_labels for backward compatibility (top 2)
  context_lane_model_->bucket_labels.clear();
  for (std::size_t i = 0; i < context_lane_model_->context_buckets.size() && i < 2; ++i) {
    context_lane_model_->bucket_labels.push_back(
        context_lane_model_->context_buckets[i].label + " " + 
        formatCompactCount(context_lane_model_->context_buckets[i].tokens));
  }

  const auto rolling =
      firmius::core::RollingContextManager::resolveThresholds(ctx);
  if (rolling.enabled) {
    auto &rollingModel = context_lane_model_->rolling_memory;
    rollingModel.enabled = true;
    rollingModel.mode_label = ctx.config.rollingMemory.mode.empty()
                                  ? std::string("rolling_forever")
                                  : ctx.config.rollingMemory.mode;
    rollingModel.preset_label = rolling.preset;
    if (ctx.config.rollingMemory.observer.enabled &&
        !ctx.config.rollingMemory.observer.providerId.empty() &&
        !ctx.config.rollingMemory.observer.modelId.empty()) {
      rollingModel.model_label = ctx.config.rollingMemory.observer.providerId +
                                 "/" + ctx.config.rollingMemory.observer.modelId;
    } else {
      rollingModel.model_label =
          ctx.config.providerId + "/" + ctx.config.modelId;
    }
    rollingModel.context_window_tokens = rolling.contextWindow;
    if (rolling.contextWindow > 0) {
      rollingModel.context_occupancy_ratio =
          static_cast<float>(metrics.tokens.contextSize) /
          static_cast<float>(rolling.contextWindow);
    } else {
      rollingModel.context_occupancy_ratio = context_lane_model_->context_ratio;
    }
    rollingModel.buffer_threshold_ratio = rolling.bufferOccupancyRatio;
    rollingModel.target_threshold_ratio = rolling.targetOccupancyRatio;
    rollingModel.emergency_threshold_ratio = rolling.emergencyOccupancyRatio;
    rollingModel.buffer_threshold_tokens = rolling.bufferThresholdTokens;
    rollingModel.target_threshold_tokens = rolling.targetThresholdTokens;
    rollingModel.emergency_threshold_tokens = rolling.emergencyThresholdTokens;
    rollingModel.retained_tail_tokens = rolling.retainedTailTokens;

    if (ctx.history && !ctx.history->threadId.empty() && !ctx.identity.id.empty()) {
      try {
        firmius::core::ThreadManager tm(
            firmius::core::ThreadManager::defaultBasePath());
        const auto rollingState =
            tm.loadRollingMemoryState(ctx.history->threadId, ctx.identity.id);
        rollingModel.observation_in_flight = rollingState.observationInFlight;
        rollingModel.reflection_in_flight = rollingState.reflectionInFlight;
        rollingModel.bridge_packet_count = rollingState.bridges.size();
        rollingModel.canonical_anchor_count = rollingState.anchors.size();
        rollingModel.latest_bridge_id = rollingState.lastBridgeId;
        for (const auto &chunk : rollingState.observationChunks) {
          if (chunk.superseded) {
            continue;
          }
          if (chunk.active) {
            ++rollingModel.active_observations;
          } else if (chunk.buffered) {
            ++rollingModel.buffered_observations;
          }
          rollingModel.source_tokens += chunk.sourceTokens;
          rollingModel.summary_tokens += chunk.summaryTokens;
          if (chunk.sourceTokens > chunk.summaryTokens) {
            rollingModel.saved_tokens +=
                (chunk.sourceTokens - chunk.summaryTokens);
          }
        }
        for (const auto &chunk : rollingState.reflectionChunks) {
          if (chunk.superseded) {
            continue;
          }
          if (chunk.active) {
            ++rollingModel.active_reflections;
          }
          rollingModel.source_tokens += chunk.sourceTokens;
          rollingModel.summary_tokens += chunk.summaryTokens;
          if (chunk.sourceTokens > chunk.summaryTokens) {
            rollingModel.saved_tokens +=
                (chunk.sourceTokens - chunk.summaryTokens);
          }
        }
        if (!rollingState.bridges.empty()) {
          const auto &bridge = rollingState.bridges.back();
          rollingModel.bridge_target = bridge.targetTaskSignature;
          rollingModel.bridge_hint = bridge.executionHint;
        }
        if (rollingState.bridgeInFlight) {
          rollingModel.observation_in_flight =
              rollingModel.observation_in_flight || rollingState.bridgeInFlight;
        }
      } catch (...) {
      }
    }
  }
}

std::optional<shared::Plan>
TuiState::loadActivePlanForThread(const shared::ThreadMetadata & /*thread*/) const {
  return std::nullopt;
}

ftxui::Component TuiState::root() {
  if (root_component_)
    return root_component_;

  auto title_bar = TitleBar(title_model_);
  auto status_bar = StatusBar(status_model_);
  auto agent_strip = AgentStrip(agent_strip_model_);
  auto plan_lane = PlanLane(plan_lane_model_);
  auto todo_lane = TodoLane(todo_lane_model_);
  auto context_lane = ContextLane(context_lane_model_);

  auto input_bar = InputBar(
      input_model_,
      [this](const std::string &text,
             const std::vector<firmius::tui::PastedBlock> &images) {
        auto applyPendingRewriteIfNeeded = [this]() -> bool {
          if (!pending_edit_message_ || !harness_) {
            return true;
          }
          const uint64_t cutoff =
              pending_edit_message_->timestamp > 0
                  ? pending_edit_message_->timestamp - 1
                  : 0;
          suppress_next_history_undone_refresh_ = true;
          const auto result = harness_->undoAfterTimestamp(cutoff);
          suppress_next_history_undone_refresh_ = false;
          if (result.turnsRemoved == 0) {
            NotificationManager::instance().notifyWarning(
                "Rewrite Not Applied",
                "Could not prune the earlier turn for rewrite.",
                std::chrono::milliseconds(2500));
            return false;
          }
          pending_edit_message_.reset();
          refreshFocusedHistory();
          notifyChatTranscriptChanged();
          requestRefresh(RefreshFlags::Status);
          requestRefresh(RefreshFlags::AgentStrip);
          requestRefresh(RefreshFlags::ContextLane);
          applyPendingRefreshes();
          return true;
        };

        // Convert pasted image blocks to ImageContent
        std::vector<firmius::shared::ImageContent> image_contents;
        for (const auto &img : images) {
          if (img.type == "image") {
            firmius::shared::ImageContent content;
            const std::string mime_type =
                img.mime_type.empty() ? "image/png" : img.mime_type;
            content.url = "data:" + mime_type + ";base64," + img.content;
            content.mediaType = mime_type;
            content.detail = "auto";
            image_contents.push_back(content);
          }
        }

        if (!text.empty() && text[0] == '/') {
          CommandCtx ctx{this};
          auto &cmdManager = firmius::tui::CommandManager::instance();

          // Check if this is a workflow command before executing
          bool is_workflow_command = false;
          std::string content = text.substr(1);
          size_t space_pos = content.find(' ');
          std::string cmd_name = (space_pos == std::string::npos)
                                     ? content
                                     : content.substr(0, space_pos);

          auto it = cmdManager.getCommand(cmd_name);
          if (it && it->isWorkflow()) {
            is_workflow_command = true;
          }

          // Workflow commands need an active thread before execution because
          // they expand to a normal send() through Harness::executeWorkflow().
          if (view_mode_ == ViewMode::Welcome && harness_ &&
              is_workflow_command) {
            std::string cwd = std::filesystem::current_path().string();
            harness_->newThread(
                {}, cwd, resolveDefaultLeadPersona(harness_));
            harness_->setCurrentThreadPermissionMode(thread_.permissionMode);
            syncCurrentThreadMetadataFromHarness(false);
          }

          if (cmdManager.executeCommand(ctx, text)) {
            return;
          }
        }

        if (view_mode_ == ViewMode::Welcome) {
          // If we are on the welcome screen, typing a message automatically
          // starts a thread
          if (harness_) {
            // Auto-create thread in current directory with default lead persona
            std::string cwd = std::filesystem::current_path().string();
            harness_->newThread({}, cwd, resolveDefaultLeadPersona(harness_));
            harness_->setCurrentThreadPermissionMode(thread_.permissionMode);
            // A welcome-screen send is a fresh thread transition, not a
            // live-stream continuation. Do the full thread rebind before the
            // first send so chat/history/tool state is initialized the same
            // way as any other thread switch.
            syncCurrentThreadMetadataFromHarness(true);
            harness_->send(text, image_contents);

            setViewMode(ViewMode::Chat);
          }
        } else {
          if (harness_) {
            if (!applyPendingRewriteIfNeeded()) {
              return;
            }
            harness_->send(text, image_contents);
          }
        }
        input_component_->TakeFocus();
      },
      [this]() {
        if (pending_permission_request_) {
          return;
        }
        if (pending_edit_message_) {
          pending_edit_message_.reset();
          if (input_model_ && input_model_->buffer && input_model_->cursor) {
            input_model_->buffer->clear();
            input_model_->pasted_blocks.clear();
            *input_model_->cursor = 0;
          }
          NotificationManager::instance().notifyInfo(
              "Rewrite Cancelled", "Skipped applying the staged rewrite.",
              std::chrono::milliseconds(2000));
          if (screen_) {
            postEvent(ftxui::Event::Custom);
          }
          return;
        }
        if (harness_) {
          harness_->abortAndFlushQueuedMessages();
        }
      });
  auto focused_history_getter = [this]() -> const firmius::shared::AgentHistory * {
    return history_.get();
  };

  auto chat = ChatWindow(
      focused_history_getter,
      [this]() {
        std::vector<ftxui::Element> live_rows;
        const auto *s = stream_state_.getStream(focused_agent_id_);
        const auto &theme =
            firmius::tui::ThemeManager::instance().getCurrentTheme();

        auto decorateMsg = [](const ftxui::Element &content) {
          return firmius::tui::IndentAgentRow(content);
        };
        auto renderTag = [&theme](const std::string &label) {
          return ftxui::text(" " + label + " ") | ftxui::bold |
                 ftxui::color(theme.base.bg) |
                 ftxui::bgcolor(theme.base.highlight);
        };
        auto renderUserRow = [&theme, &renderTag](const std::string &text,
                                      const std::string &tag = "",
                                      int image_count = 0) {
          ftxui::Elements body{
              ftxui::hbox({
                  ftxui::text("> ") | ftxui::bold |
                      ftxui::color(theme.chat.user_prefix),
                  firmius::tui::RenderMarkdown(
                      firmius::tui::ClampTranscriptTextForDisplay(text)) |
                      ftxui::xflex,
              }) | ftxui::xflex,
          };
          if (image_count > 0 || !tag.empty()) {
            ftxui::Elements tags;
            if (!tag.empty()) {
              tags.push_back(renderTag(tag));
            }
            for (int i = 0; i < image_count; ++i) {
              if (!tags.empty()) {
                tags.push_back(ftxui::text(" "));
              }
              tags.push_back(renderTag("IMAGE " + std::to_string(i + 1)));
            }
            body.push_back(ftxui::hbox(std::move(tags)) | ftxui::xflex);
          }
          return ftxui::vbox({
                     ftxui::text(""),
                     ftxui::vbox(std::move(body)) | ftxui::xflex,
                     ftxui::text(""),
                 }) |
                 ftxui::bgcolor(theme.input.bg) | ftxui::xflex;
        };
        auto loopFooter = [this, &theme]() -> std::optional<ftxui::Element> {
          if (focused_agent_id_.empty()) {
            return std::nullopt;
          }
          auto agent = firmius::core::AgentRegistry::instance().getAgent(
              focused_agent_id_);
          if (!agent) {
            return std::nullopt;
          }
          const auto &ctx = agent->getContext();
          if (!agent->isRunning()) {
            return std::nullopt;
          }
          const auto now = std::chrono::steady_clock::now();
          if (now - last_live_raf_request_ >= std::chrono::milliseconds(100)) {
            last_live_raf_request_ = now;
            noteTuiRequestAnimationFrameFromState();
            ftxui::animation::RequestAnimationFrame();
          }
          std::string line;
          const std::string title =
              ctx.identity.friendlyName.empty()
                  ? resolvePersonaTitle(ctx.config.personaName)
                  : ctx.identity.friendlyName;
          if (!title.empty()) {
            line += title;
          }
          if (!ctx.config.modelId.empty()) {
            line += " · " + ctx.config.modelId;
          }
          auto it = agent_work_start_ms_.find(focused_agent_id_);
          const auto nowMs = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count());
          if (it == agent_work_start_ms_.end()) {
            it = agent_work_start_ms_.emplace(focused_agent_id_, nowMs).first;
          }
          if (it != agent_work_start_ms_.end()) {
            if (nowMs > it->second) {
              line += " · " + formatDurationFromMs(nowMs - it->second);
            }
          }
          return firmius::tui::IndentAgentRow(
                     ftxui::text(line) | ftxui::color(theme.chat.timestamp)) |
                 ftxui::xflex;
        };

        auto full_width_separator = [](const std::string &label) {
          return ftxui::hbox({
                     ftxui::filler() | ftxui::xflex,
                     ftxui::text(" " + label + " ") | ftxui::dim,
                     ftxui::filler() | ftxui::xflex,
                 }) |
                 ftxui::xflex;
        };

        if (s) {

          auto compaction_separator = [&full_width_separator]() {
            return full_width_separator("Compaction");
          };
          auto compaction_separator_bottom = [&full_width_separator]() {
            return full_width_separator("Compaction Complete");
          };
          bool has_compaction_output =
              s->compaction_active &&
              (!s->compaction_thinking.empty() || !s->compaction_text.empty());
          if (has_compaction_output) {
            live_rows.push_back(compaction_separator());
            if (!s->compaction_thinking.empty()) {
              live_rows.push_back(decorateMsg(
                  firmius::tui::RenderMarkdown(
                      firmius::tui::ClampTranscriptTextForDisplay(
                          s->compaction_thinking),
                      true)));
            }
            if (!s->compaction_text.empty()) {
              live_rows.push_back(decorateMsg(
                  firmius::tui::RenderMarkdown(
                      firmius::tui::ClampTranscriptTextForDisplay(
                          s->compaction_text))));
            }
            live_rows.push_back(compaction_separator_bottom());
          }

          if (s->provider_waiting) {
            // Eliminated diagnostic provider waiting text
          }
        }

        const auto &timeline = stream_state_.getTimeline();
        const auto &tool_calls = stream_state_.getToolCalls();
        const auto persisted_tool_call_ids_it =
            agent_persisted_tool_call_ids_cache_.find(focused_agent_id_);
        const auto &persisted_tool_call_ids =
            persisted_tool_call_ids_it != agent_persisted_tool_call_ids_cache_.end()
                ? persisted_tool_call_ids_it->second
                : kEmptyToolCallIdSet;

        const bool hideErrors = harness_ ? harness_->getConfig().hideErrors : false;

        for (const auto &entry : timeline) {
          if (entry.kind == TimelineEntry::Kind::Thinking ||
              entry.kind == TimelineEntry::Kind::Text) {
            if (entry.agentId != focused_agent_id_) {
              continue;
            }
            const auto trimmed =
                firmius::tui::ClampTranscriptTextForDisplay(entry.message);
            if (trimmed.empty()) {
              continue;
            }
            live_rows.push_back(decorateMsg(firmius::tui::RenderMarkdown(
                trimmed,
                entry.kind == TimelineEntry::Kind::Thinking)));
            continue;
          }

          if (entry.kind == TimelineEntry::Kind::Error) {
            if (entry.agentId != focused_agent_id_) {
              continue;
            }
            if (!hideErrors) {
              live_rows.push_back(firmius::tui::IndentAgentRow(
                  firmius::tui::RenderErrorDisplay(
                      theme, firmius::shared::ErrorContent{
                                 "Provider Rate Limit",
                                 "The provider returned a rate-limit response during this turn.",
                                 entry.message})));
            }
            continue;
          }

          if (entry.kind != TimelineEntry::Kind::ToolCall) {
            continue;
          }

          auto it_tool = tool_calls.find(entry.id);
          if (it_tool == tool_calls.end() || !it_tool->second) {
            continue;
          }

          if (!firmius::tui::ShouldRenderFocusedSubagentToolCall(
                  entry, *it_tool->second, focused_agent_id_)) {
            continue;
          }

          if (persisted_tool_call_ids.count(entry.id) > 0) {
            continue;
          }

          const auto descriptor =
              firmius::tui::DescribeQuickToolCall(*it_tool->second);
          if (firmius::tui::IsQuickToolCategory(descriptor.category)) {
            continue;
          }

          auto sub_history_getter = [this](const std::string &agentId)
              -> const firmius::shared::AgentHistory * {
            auto it = agent_history_cache_.find(agentId);
            return it != agent_history_cache_.end() ? it->second.get()
                                                    : nullptr;
          };
          auto sub_stream_getter = [this](const std::string &agentId)
              -> const firmius::tui::StreamState * {
            if (agentId.empty())
              return nullptr;
            return stream_state_.getStream(agentId);
          };
          auto process_state_getter = [this](const std::string &toolCallId)
              -> const firmius::tui::NormalizedProcessState * {
            if (toolCallId.empty())
              return nullptr;
            return stream_state_.getProcessStateForToolCall(toolCallId);
          };
          auto subagent_state_getter = [this](const std::string &toolCallId)
              -> const firmius::tui::NormalizedSubagentState * {
            if (toolCallId.empty())
              return nullptr;
            return stream_state_.getSubagentStateForToolCall(toolCallId);
          };

          auto tool_row = ToolBlock(it_tool->second, sub_history_getter,
                                    sub_stream_getter, process_state_getter,
                                    subagent_state_getter)
                              ->Render();
          live_rows.push_back(decorateMsg(tool_row));
        }

        if (auto footer = loopFooter(); footer.has_value()) {
          live_rows.push_back(*footer);
        }

        const auto &queued = stream_state_.getQueuedMessages();
        std::vector<QueuedMessageEntry> queued_for_focus;
        queued_for_focus.reserve(queued.size());
        for (const auto &entry : queued) {
          if (!entry.agent_id.empty() && entry.agent_id != focused_agent_id_) {
            continue;
          }
          if (!entry.thread_id.empty() &&
              entry.thread_id != thread_.threadId) {
            continue;
          }
          queued_for_focus.push_back(entry);
        }

        const auto &queued_internal = stream_state_.getQueuedInternalMessages();
        std::vector<QueuedMessageEntry> queued_internal_for_focus;
        queued_internal_for_focus.reserve(queued_internal.size());
        for (const auto &entry : queued_internal) {
          if (!entry.agent_id.empty() && entry.agent_id != focused_agent_id_) {
            continue;
          }
          if (!entry.thread_id.empty() &&
              entry.thread_id != thread_.threadId) {
            continue;
          }
          queued_internal_for_focus.push_back(entry);
        }

        const bool showInternalNudges =
            harness_ ? harness_->getConfig().showInternalNudges : false;
        if (showInternalNudges && !queued_internal_for_focus.empty()) {
          for (const auto &entry : queued_internal_for_focus) {
            live_rows.push_back(
                renderUserRow(entry.text, "INTERNAL", entry.image_count));
          }
        }
        if (!queued_for_focus.empty()) {
          for (const auto &entry : queued_for_focus) {
            live_rows.push_back(
                renderUserRow(entry.text, "QUEUED", entry.image_count));
          }
        }

        return live_rows;
      },
      [this](const std::string &toolCallId) {
        return stream_state_.getToolView(toolCallId);
      },
      [this](const std::string &toolCallId)
          -> const firmius::tui::NormalizedProcessState * {
        return stream_state_.getProcessStateForToolCall(toolCallId);
      },
      [this](const std::string &toolCallId)
          -> const firmius::tui::NormalizedSubagentState * {
        return stream_state_.getSubagentStateForToolCall(toolCallId);
      },
      [this](const std::string &agentId) {
        focusAgent(agentId);
      },
      [this](
          const std::string &agentId) -> const firmius::shared::AgentHistory * {
        auto it = agent_history_cache_.find(agentId);
        return it != agent_history_cache_.end() ? it->second.get() : nullptr;
      },
      [this](const std::string &agentId) -> const firmius::tui::StreamState * {
        if (agentId.empty())
          return nullptr;
        return stream_state_.getStream(agentId);
      },
      [this]() {
        std::unordered_map<int, firmius::tui::LiveQuickSummaryCluster> clusters;
        std::vector<int> cluster_order;
        const auto &timeline = stream_state_.getTimeline();
        const auto &tool_calls = stream_state_.getToolCalls();
        const auto persisted_tool_call_ids_it =
            agent_persisted_tool_call_ids_cache_.find(focused_agent_id_);
        const auto &persisted_tool_call_ids =
            persisted_tool_call_ids_it != agent_persisted_tool_call_ids_cache_.end()
                ? persisted_tool_call_ids_it->second
                : kEmptyToolCallIdSet;

        for (const auto &entry : timeline) {
          if (entry.kind != TimelineEntry::Kind::ToolCall) {
            continue;
          }

          if (entry.agentId != focused_agent_id_) {
            continue;
          }

          auto it_tool = tool_calls.find(entry.id);
          if (it_tool == tool_calls.end() || !it_tool->second) {
            continue;
          }

          const auto &view = it_tool->second;
          if (persisted_tool_call_ids.count(entry.id) > 0) {
            continue;
          }

          if (!firmius::tui::ShouldRenderToolCallView(*view)) {
            continue;
          }

          const auto descriptor = firmius::tui::DescribeQuickToolCall(*view);
          if (!firmius::tui::IsQuickToolCategory(descriptor.category)) {
            continue;
          }

          int cluster_id = stream_state_.getToolCallClusterId(entry.id);
          if (cluster_id < 0) {
            cluster_id = 0;
          }
          if (!clusters.count(cluster_id)) {
            cluster_order.push_back(cluster_id);
          }
          auto &cluster = clusters[cluster_id];
          cluster.merge_with_history = (cluster_id == 0);

          auto key = static_cast<int>(descriptor.category);
          auto &summary = cluster.summaries[key];
          if (summary.category == firmius::tui::QuickToolCategory::None) {
            summary.category = descriptor.category;
            cluster.category_order.push_back(descriptor.category);
          }
          if (!descriptor.target.empty()) {
            summary.targets.push_back(descriptor.target);
          }
          if (view->phase == ToolPhase::Preparing) {
            summary.has_preparing = true;
            summary.preparing_count++;
          } else if (view->phase == ToolPhase::Called) {
            summary.has_live = true;
            summary.live_count++;
          } else if (view->phase == ToolPhase::Error) {
            summary.has_error = true;
          }
        }

        std::vector<firmius::tui::LiveQuickSummaryCluster> result;
        result.reserve(cluster_order.size());
        for (int cluster_id : cluster_order) {
          result.push_back(std::move(clusters[cluster_id]));
        }
        return result;
      }, [this]() {
        const auto persisted_it =
            agent_persisted_tool_call_ids_cache_.find(focused_agent_id_);
        const auto &persisted =
            persisted_it != agent_persisted_tool_call_ids_cache_.end()
                ? persisted_it->second
                : kEmptyToolCallIdSet;
        return buildFocusedChatLiveMeasurementSignature(
            stream_state_, focused_agent_id_, thread_.threadId, persisted);
      },
      [this]() {
        if (!harness_) {
          return false;
        }
        return harness_->getConfig().showInternalNudges;
      },
      [this]() {
        if (!harness_) {
          return false;
        }
        return harness_->getConfig().hideErrors;
      },
      [this]() { return skin_config_.show_turn_footers; },
      [this]() { return edit_mode_active_; },
      [this](uint64_t timestamp) { return isEditModeSelection(timestamp); },
      [this](uint64_t timestamp) { selectEditableMessageByTimestamp(timestamp); });
  chat_component_ = chat;
  if (history_ && !history_->turns.empty()) {
    chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
  }
  input_component_ = input_bar;

  auto live_status_row = ftxui::Renderer([this] {
    if (!skin_config_.show_persistent_live_row || focused_agent_id_.empty()) {
      return ftxui::emptyElement();
    }

    LiveStatusRowModel model;
    model.skin = skin_config_;
    model.focused_agent_id = focused_agent_id_;
    const int terminal_width = std::max(20, ftxui::Terminal::Size().dimx);
    auto agent =
        firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
    const auto *stream = stream_state_.getStream(focused_agent_id_);
    const bool busy =
        (agent && (agent->isRunning() || agent->isBooting())) ||
        (stream && stream->provider_waiting) ||
        (status_model_ && status_model_->status_text == "streaming");
    model.busy = busy;
    if (!busy && skin_config_.live_row_busy_only) {
      return ftxui::emptyElement();
    }

    const auto *active_plan = active_plan_state_.hasActivePlan()
                                  ? active_plan_state_.activePlan().operator->()
                                  : nullptr;

    const auto now = std::chrono::steady_clock::now();

    const auto nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    auto it = agent_work_start_ms_.find(focused_agent_id_);
    if (it == agent_work_start_ms_.end()) {
      it = agent_work_start_ms_.emplace(focused_agent_id_, nowMs).first;
    }
    model.elapsed = (it != agent_work_start_ms_.end() && nowMs > it->second)
                        ? formatDurationFromMs(nowMs - it->second)
                        : std::string("0s");
    if (busy) {
      model.phrase_mode = "working";
      if (status_model_ && status_model_->status_text == "streaming") {
        model.phrase_mode = "thinking";
      }
      if (stream && stream->provider_waiting) {
        model.phrase_mode = "waiting";
      }
    }

    const auto &phrase_bank = claudexLivePhrasesForMode(model.phrase_mode);
    const std::string phrase_mode_key = model.phrase_mode.empty()
                                            ? std::string("idle")
                                            : model.phrase_mode;
    auto transition_duration = liveRowTransitionDurationForPhrases(
        live_row_previous_phrase_, live_row_current_phrase_);
    auto transition_elapsed = now - live_row_phrase_transition_started_at_;
    bool transition_in_progress =
        !live_row_previous_phrase_.empty() &&
        transition_elapsed < transition_duration;
    const bool mode_changed =
        live_row_last_phrase_key_.empty() ||
        live_row_last_phrase_key_.rfind(phrase_mode_key + ":", 0) != 0;
    auto begin_transition = [&](std::string next_key, std::string next_phrase) {
      live_row_previous_phrase_ = live_row_current_phrase_;
      live_row_current_phrase_ = std::move(next_phrase);
      live_row_last_phrase_key_ = std::move(next_key);
      live_row_phrase_transition_started_at_ = now;
      live_row_phrase_next_switch_at_ =
          now + nextLiveRowPhraseDelay(live_row_phrase_rng_state_);
      live_row_phrase_min_visible_until_ =
          now + minimumLiveRowPhraseVisibleDuration();
    };
    if (live_row_current_phrase_.empty()) {
      live_row_current_phrase_ =
          pickLiveRowPhrase(phrase_bank, "", live_row_phrase_rng_state_);
      live_row_last_phrase_key_ = phrase_mode_key + ":" + live_row_current_phrase_;
      live_row_phrase_next_switch_at_ =
          now + nextLiveRowPhraseDelay(live_row_phrase_rng_state_);
      live_row_phrase_min_visible_until_ =
          now + minimumLiveRowPhraseVisibleDuration();
    } else if (mode_changed) {
      std::string next_phrase = pickLiveRowPhrase(
          phrase_bank, live_row_current_phrase_, live_row_phrase_rng_state_);
      std::string next_key = phrase_mode_key + ":" + next_phrase;
      if (transition_in_progress) {
        queueLiveRowTransition(std::move(next_key), std::move(next_phrase),
                               live_row_pending_phrase_key_,
                               live_row_pending_phrase_);
      } else if (now >= live_row_phrase_min_visible_until_) {
        begin_transition(std::move(next_key), std::move(next_phrase));
      } else {
        queueLiveRowTransition(std::move(next_key), std::move(next_phrase),
                               live_row_pending_phrase_key_,
                               live_row_pending_phrase_);
      }
    } else if (now >= live_row_phrase_next_switch_at_) {
      std::string next_phrase = pickLiveRowPhrase(
          phrase_bank, live_row_current_phrase_, live_row_phrase_rng_state_);
      std::string next_key = phrase_mode_key + ":" + next_phrase;
      if (transition_in_progress) {
        queueLiveRowTransition(std::move(next_key), std::move(next_phrase),
                               live_row_pending_phrase_key_,
                               live_row_pending_phrase_);
      } else {
        begin_transition(std::move(next_key), std::move(next_phrase));
      }
    }

    transition_duration = liveRowTransitionDurationForPhrases(
        live_row_previous_phrase_, live_row_current_phrase_);
    transition_elapsed = now - live_row_phrase_transition_started_at_;
    transition_in_progress = !live_row_previous_phrase_.empty() &&
                             transition_elapsed < transition_duration;

    if (!live_row_previous_phrase_.empty() &&
        transition_elapsed < transition_duration) {
      const float t = std::clamp(
          static_cast<float>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 transition_elapsed)
                                 .count()) /
              static_cast<float>(transition_duration.count()),
          0.0f, 1.0f);
      model.phrase_transition_active = true;
      model.phrase_transition_t = t;
      model.phrase_prev = live_row_previous_phrase_;
      model.phrase_next = live_row_current_phrase_;
      model.phrase = "";
      ftxui::animation::RequestAnimationFrame();
    } else {
      live_row_previous_phrase_.clear();
      model.phrase = live_row_current_phrase_.empty() ? std::string("Standing by.")
                                                      : live_row_current_phrase_;
      model.phrase_transition_active = false;
      model.phrase_transition_t = 1.0f;
      model.phrase_prev.clear();
      model.phrase_next.clear();
      if (!live_row_pending_phrase_.empty() &&
          !live_row_pending_phrase_key_.empty() &&
          live_row_pending_phrase_key_ != live_row_last_phrase_key_ &&
          now >= live_row_phrase_min_visible_until_) {
        begin_transition(std::move(live_row_pending_phrase_key_),
                         std::move(live_row_pending_phrase_));
        model.phrase_transition_active = true;
        model.phrase_transition_t = 0.0f;
        model.phrase_prev = live_row_previous_phrase_;
        model.phrase_next = live_row_current_phrase_;
        model.phrase.clear();
      }
      live_row_pending_phrase_key_.clear();
      live_row_pending_phrase_.clear();
    }

    const bool should_animate_live_row =
        busy || model.phrase_transition_active ||
        (skin_config_.show_persistent_live_row && !focused_agent_id_.empty());
    if (should_animate_live_row &&
        now - last_live_raf_request_ >= std::chrono::milliseconds(33)) {
      last_live_raf_request_ = now;
      noteTuiRequestAnimationFrameFromState();
      ftxui::animation::RequestAnimationFrame();
    }

    if (agent) {
      const auto &ctx = agent->getContext();
      model.activity = inferClaudexActivity(ctx, stream, statusText());

      if (skin_config_.live_row_show_todo_excerpt && !thread_.threadId.empty()) {
        try {
          firmius::core::ThreadManager tm(firmiusThreadsPath());
          auto todo = tm.getAgentTodo(thread_.threadId, focused_agent_id_);
          auto it = std::find_if(todo.items.begin(), todo.items.end(),
                                 [](const auto &item) {
                                   return item.status ==
                                          shared::TodoStatus::InProgress;
                                 });
          if (it == todo.items.end()) {
            it = std::find_if(todo.items.begin(), todo.items.end(),
                              [](const auto &item) {
                                return item.status ==
                                       shared::TodoStatus::Pending;
                              });
          }
          if (it != todo.items.end()) {
            model.todo_excerpt =
                truncateText(it->text, std::max(16, terminal_width - 4));
            model.has_todo_excerpt = !model.todo_excerpt.empty();
          }
        } catch (...) {
        }
      }
    }

    if (model.activity.empty()) {
      model.activity = busy ? "working" : "idle";
    }

    if (model.phrase_mode.empty()) {
      if (model.activity == "thinking")
        model.phrase_mode = "thinking";
      else if (model.activity == "verifying")
        model.phrase_mode = "verifying";
      else if (model.activity == "waiting")
        model.phrase_mode = "waiting";
      else if (busy)
        model.phrase_mode = "working";
      else
        model.phrase_mode = "idle";
    }

    if (false && skin_config_.live_row_show_plan_excerpt && active_plan != nullptr) {
      model.plan_title = active_plan->title;
      auto chunks = active_plan->chunks;
      std::stable_sort(chunks.begin(), chunks.end(),
                       [](const auto &a, const auto &b) {
                         return workChunkRank(a.status) < workChunkRank(b.status);
                       });
      const std::size_t shown = std::min<std::size_t>(3, chunks.size());
      model.hidden_plan_count =
          chunks.size() > shown ? (chunks.size() - shown) : 0;
      for (std::size_t i = 0; i < shown; ++i) {
        LiveStatusPlanRow row;
        row.title =
            truncateText(chunks[i].title, std::max(12, terminal_width - 12));
        row.status = chunks[i].status;
        model.plan_rows.push_back(row);
      }
      model.has_plan_excerpt = !model.plan_rows.empty();
    }

    return RenderLiveStatusRow(model);
  });

  auto container = ftxui::Container::Vertical({
      input_bar,
      chat,
      todo_lane,
      plan_lane,
      context_lane,
      agent_strip,
  });

  auto welcome_screen = ftxui::Renderer([] {
    return ftxui::vbox({
               ftxui::text("Welcome to Firmius") | ftxui::bold | ftxui::center,
               ftxui::text("Type a message to start") | ftxui::dim |
                   ftxui::center,
           }) |
           ftxui::flex | ftxui::center;
  });

  auto base_view =
      ftxui::Renderer(container, [this, title_bar, status_bar, plan_lane,
                                  todo_lane, context_lane, agent_strip, input_bar, chat,
                                  welcome_screen, live_status_row] {
        // Deferred modal clearing: drain here where it's safe
        if (pending_modal_clear_) {
          modals_.clear();
          pending_modal_clear_ = false;
        }

    // AGGRESSIVELY take focus if no modals are open
        if (input_model_) {
          input_model_->is_focused = !edit_mode_active_;
        }
        if (modals_.empty() && input_component_ && !pending_permission_request_ &&
            !edit_mode_active_) {
          input_component_->TakeFocus();
        }

        ftxui::Element chat_area;
        if (view_mode_ == ViewMode::Chat) {
          chat_area = chat->Render() | ftxui::flex;
        } else if (view_mode_ == ViewMode::ProcessFocus) {
          // Process focus mode - show process output prominently
          chat_area = chat->Render() | ftxui::flex;
        } else {
          chat_area = welcome_screen->Render();
        }
        const auto &theme = ThemeManager::instance().getCurrentTheme();
        const auto input_separator_color =
            isQuitArmed() ? theme.status_bar.error.normal.fg
                          : theme.base.border;
        const auto terminal = ftxui::Terminal::Size();
        // ScrollableBox uses reflect(box_) and stabilizes over two passes.
        // Trigger one extra pass whenever terminal dimensions change.
        if (terminal.dimx != last_terminal_width_ ||
            terminal.dimy != last_terminal_height_) {
          last_terminal_width_ = terminal.dimx;
          last_terminal_height_ = terminal.dimy;
          if (screen_) {
            postEvent(ftxui::Event::Custom);
          }
        }
        const int work_panel_max_height =
            computeWorkPanelMaxHeight(terminal.dimy);
        const int context_panel_height =
            std::max(work_panel_max_height, std::min(terminal.dimy / 3, 14));
        const bool hasPlan = false;
        const bool hasTodo = todo_lane_model_ && todo_lane_model_->visible;
        const bool hasContext =
            context_lane_model_ && context_lane_model_->visible;
        const auto visibleTab =
            normalizeWorkPanelTab(selected_work_panel_tab_, hasPlan, hasTodo,
                                  hasContext);
        selected_work_panel_tab_ = visibleTab;

        ftxui::Element work_panel = ftxui::text("");
        bool show_work_panel = false;
        const auto tabs = availableWorkPanelTabs(hasPlan, hasTodo, hasContext);
        if (show_work_panel_ && !tabs.empty()) {
          auto renderTab = [&](WorkPanelTab tab) {
            const bool selected = tab == visibleTab;
            const std::string icon =
                tab == WorkPanelTab::Plan
                    ? shared::ICON_BOOK
                    : (tab == WorkPanelTab::Todo ? shared::ICON_TODO
                                                 : shared::ICON_CONTEXT);
            std::string label =
                tab == WorkPanelTab::Plan
                    ? "PLAN"
                    : (tab == WorkPanelTab::Todo ? "TODO" : "CONTEXT");
            std::string text_str = selected ? (" " + icon + " " + label + " ") : (" " + icon + " ");
            return ftxui::text(text_str) | ftxui::bold |
                   ftxui::color(selected ? theme.base.bg : theme.base.dim) |
                   ftxui::bgcolor(selected ? theme.base.highlight
                                           : theme.agent_strip.bg);
          };

          ftxui::Element kb_hint =
              ftxui::text(" Ctrl+O to cycle ") | ftxui::color(theme.base.dim);

          ftxui::Element selected_panel = ftxui::text("");
          int panel_height = work_panel_height_override_ > 0
                                 ? std::min(work_panel_height_override_,
                                            std::max(4, terminal.dimy - 8))
                                 : work_panel_max_height;
          if (visibleTab == WorkPanelTab::Todo && hasTodo) {
            selected_panel = todo_lane->Render();
          } else if (visibleTab == WorkPanelTab::Context && hasContext) {
            selected_panel = context_lane->Render();
            panel_height = work_panel_height_override_ > 0
                               ? std::min(work_panel_height_override_,
                                          std::max(4, terminal.dimy - 8))
                               : context_panel_height;
          }

          ftxui::Elements tab_elements;
          for (std::size_t i = 0; i < tabs.size(); ++i) {
            if (i > 0) {
              tab_elements.push_back(ftxui::text(" "));
            }
            tab_elements.push_back(renderTab(tabs[i]));
          }
          work_panel =
              ftxui::vbox({
                  ftxui::hbox({ftxui::hbox(std::move(tab_elements)), ftxui::filler(), kb_hint}) |
                      ftxui::bgcolor(theme.agent_strip.bg) | ftxui::xflex,
                  selected_panel |
                      ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN,
                                  panel_height),
              }) |
              ftxui::xflex;
          show_work_panel = true;
        }
        // Ultra-compact bottom bar layout
        ftxui::Elements bottom_bar_children;
        const bool has_agent_strip =
            show_agent_strip_ && agent_strip_model_ &&
            !agent_strip_model_->items.empty();
        if (has_agent_strip) {
          bottom_bar_children.push_back(agent_strip->Render());
        }
        if (skin_config_.kind == SkinKind::Claudex &&
            skin_config_.show_persistent_live_row && !focused_agent_id_.empty()) {
          auto focused_agent =
              firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
          const auto *stream = stream_state_.getStream(focused_agent_id_);
          const bool busy =
              (focused_agent &&
               (focused_agent->isRunning() || focused_agent->isBooting())) ||
              (stream && stream->provider_waiting) ||
              (status_model_ && status_model_->status_text == "streaming");
          const bool show_live_row = busy || !skin_config_.live_row_busy_only;
          if (show_live_row) {
            bottom_bar_children.push_back(live_status_row->Render());
          }
        }
        bottom_bar_children.push_back((ftxui::separator() |
                                      ftxui::color(input_separator_color)) |
                                      ftxui::reflect(agent_strip_separator_box_));
        if (pending_permission_request_) {
          pending_permission_option_boxes_.assign(
              pending_permission_labels_.size(), ftxui::Box{});
          const auto &request = *pending_permission_request_;
          const auto accent = permissionSeverityColor(theme, request.severity);
          const std::string detail =
              request.command.empty() ? request.targetPath : request.command;

          ftxui::Elements option_rows;
          option_rows.reserve(pending_permission_labels_.size());
          for (size_t i = 0; i < pending_permission_labels_.size(); ++i) {
            const bool selected =
                static_cast<int>(i) == pending_permission_selected_;
            option_rows.push_back(
                ftxui::text(" " + std::to_string(i + 1) + " " +
                            pending_permission_labels_[i] + " ") |
                ftxui::reflect(pending_permission_option_boxes_[i]) |
                ftxui::color(selected ? theme.modals.highlight_fg
                                      : theme.modals.fg) |
                ftxui::bgcolor(selected ? theme.modals.highlight_bg
                                        : theme.input.bg));
            if (i + 1 < pending_permission_labels_.size()) {
              option_rows.push_back(ftxui::text(" "));
            }
          }

          bottom_bar_children.push_back(
              ftxui::vbox({
                  ftxui::hbox({
                      ftxui::text(" Permission Required ") | ftxui::bold |
                          ftxui::color(accent),
                      ftxui::filler(),
                      ftxui::text("Arrows move  Enter confirm  Esc deny") |
                          ftxui::color(theme.base.dim),
                  }),
                  ftxui::paragraph(request.message) | ftxui::color(theme.base.dim),
                  ftxui::paragraph(detail) | ftxui::color(theme.input.fg),
                  ftxui::hbox(std::move(option_rows)),
              }) |
              ftxui::xflex | ftxui::bgcolor(theme.input.bg));
        } else if (edit_mode_active_) {
          bottom_bar_children.push_back(
              (input_bar->Render() | ftxui::dim) |
              ftxui::bgcolor(theme.input.bg));
        } else {
          bottom_bar_children.push_back(input_bar->Render());
        }
        bottom_bar_children.push_back(ftxui::separator() |
                                      ftxui::color(input_separator_color));
        bottom_bar_children.push_back(status_bar->Render());

        auto bottom_bar = ftxui::vbox(std::move(bottom_bar_children));

        ftxui::Element main_view;
        if (view_mode_ == ViewMode::Welcome) {
          main_view = ftxui::vbox({chat_area, bottom_bar}) | ftxui::flex;
        } else {
          if (show_work_panel) {
            main_view = ftxui::vbox({
                            title_bar->Render(),
                            chat_area | ftxui::flex,
                            (ftxui::separator() | ftxui::color(theme.base.border)) |
                                ftxui::reflect(work_panel_separator_box_),
                            work_panel,
                            bottom_bar,
                        }) |
                        ftxui::flex;
          } else {
            main_view = ftxui::vbox({
                            title_bar->Render(),
                            chat_area | ftxui::flex,
                            bottom_bar,
                        }) |
                        ftxui::flex;
          }
        }

        return main_view | ftxui::bgcolor(theme.base.bg);
      });
  // Layer modals using dbox
  auto modal_renderer = ftxui::Renderer(base_view, [this, base_view]() {
    if (!event_queue_.empty()) {
      drainEvents();
    }
    const auto render_begin = std::chrono::steady_clock::now();
    ftxui::Element current = base_view->Render();
    for (const auto &modal : modals_) {
      current = ftxui::dbox(
          {DarkenElement(current), modal->Render() | ftxui::center});
    }
    if (pending_profile_modal_name_.has_value() &&
        !painted_profile_modals_.count(*pending_profile_modal_name_)) {
      painted_profile_modals_.insert(*pending_profile_modal_name_);
      noteTuiModalFirstPaint(*pending_profile_modal_name_);
      pending_profile_modal_name_.reset();
    }
    // Layer notifications above modals (must be last so they are always visible).
    auto notifications = NotificationManager::instance().render();
    current = ftxui::dbox({current, notifications});
    noteTuiFrameRendered(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - render_begin));
    return current;
  });

  auto routed_component = ftxui::CatchEvent(modal_renderer, [this, chat, plan_lane, todo_lane, context_lane](
                                                ftxui::Event event) {
    auto resolve_inline_permission = [this](PermissionResponse response) {
      if (!pending_permission_request_) {
        return;
      }
      const std::string requestId = pending_permission_request_->requestId;
      promoteNextPermissionRequest();
      if (harness_) {
        harness_->resolvePermissionEscalation(requestId, response);
      }
      if (input_component_) {
        input_component_->TakeFocus();
      }
      if (screen_) {
        postEvent(ftxui::Event::Custom);
      }
    };

    if (event == ftxui::Event::Custom) {
      if (!deferred_ui_mutations_.empty()) {
        auto deferred = std::move(deferred_ui_mutations_);
        deferred_ui_mutations_.clear();
        for (auto &mutation : deferred) {
          mutation();
        }
      }
      drainEvents();
      if (chat_component_) {
        chat_component_->OnEvent(ftxui::Event::Custom);
      }
      return true;
    }

    if (event == ftxui::Event::CtrlC) {
      return handleCtrlC();
    }

    if (pending_permission_request_) {
      const int option_count =
          static_cast<int>(pending_permission_responses_.size());
      if (event == ftxui::Event::Escape) {
        resolve_inline_permission(PermissionResponse::Deny);
        return true;
      }
      if (event == ftxui::Event::Return) {
        if (pending_permission_selected_ >= 0 &&
            pending_permission_selected_ < option_count) {
          resolve_inline_permission(
              pending_permission_responses_[pending_permission_selected_]);
        } else {
          resolve_inline_permission(PermissionResponse::Deny);
        }
        return true;
      }
      if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::ArrowUp) {
        if (option_count > 0) {
          pending_permission_selected_ =
              (pending_permission_selected_ + option_count - 1) % option_count;
        }
        return true;
      }
      if (event == ftxui::Event::ArrowRight ||
          event == ftxui::Event::ArrowDown || event == ftxui::Event::Tab) {
        if (option_count > 0) {
          pending_permission_selected_ =
              (pending_permission_selected_ + 1) % option_count;
        }
        return true;
      }
      if (event.is_character()) {
        const std::string chars = event.character();
        if (!chars.empty() &&
            std::isdigit(static_cast<unsigned char>(chars.front()))) {
          const int index = chars.front() - '1';
          if (index >= 0 && index < option_count) {
            pending_permission_selected_ = index;
            resolve_inline_permission(pending_permission_responses_[index]);
            return true;
          }
        }
      }
      if (event.is_mouse()) {
        const auto mouse = event.mouse();
        for (int i = 0; i < static_cast<int>(pending_permission_option_boxes_.size());
             ++i) {
          if (!pending_permission_option_boxes_[i].Contain(mouse.x, mouse.y)) {
            continue;
          }
          pending_permission_selected_ = i;
          if (mouse.button == ftxui::Mouse::Left &&
              mouse.motion == ftxui::Mouse::Pressed && i < option_count) {
            resolve_inline_permission(pending_permission_responses_[i]);
          }
          return true;
        }
      }
      return true;
    }

    if (event == ftxui::Event::Special("PopModal")) {
      if (!modals_.empty()) {
        modals_.pop_back();
      }
      if (modals_.empty()) {
        if (input_component_) {
          input_component_->TakeFocus();
        }
        // Force re-render to ensure focus state is propagated
        postEvent(ftxui::Event::Custom);
      } else {
        modals_.back()->TakeFocus();
      }
      return true;
    }

    // Handle terminal focus gained event (escape sequence \x1b[I)
    if (event.input() == "\x1b[I") {
      if (input_model_)
        input_model_->is_focused = true;
      // Terminal regained focus - restore focus to input
      if (input_component_) {
        input_component_->TakeFocus();
      }
      return true;
    }

    // Handle terminal focus lost event (escape sequence \x1b[O)
    if (event.input() == "\x1b[O") {
      if (input_model_)
        input_model_->is_focused = false;
      return true;
    }

    if (!modals_.empty()) {
      // Forward event to the top modal
      bool handled = modals_.back()->OnEvent(event);
      if (handled)
        return true;

      // Block background interaction if a modal is up
      if (event.is_mouse() || event.is_character()) {
        return true;
      }
    }

    if (event.is_mouse()) {
      const auto mouse = event.mouse();
      if (mouse.button == ftxui::Mouse::Left &&
          mouse.motion == ftxui::Mouse::Pressed) {
        if (show_work_panel_ &&
            work_panel_separator_box_.Contain(mouse.x, mouse.y)) {
          if (screen_) {
            active_drag_mouse_ = screen_->CaptureMouse();
          }
          active_drag_target_ = DragTarget::WorkPanel;
          drag_origin_y_ = mouse.y;
          drag_origin_work_panel_height_ =
              work_panel_height_override_ > 0 ? work_panel_height_override_ : 10;
          return true;
        }
        if (show_agent_strip_ &&
            agent_strip_separator_box_.Contain(mouse.x, mouse.y)) {
          if (screen_) {
            active_drag_mouse_ = screen_->CaptureMouse();
          }
          active_drag_target_ = DragTarget::AgentStrip;
          drag_origin_y_ = mouse.y;
          drag_origin_agent_strip_rows_ = agent_strip_visible_rows_;
          return true;
        }
      }
      if (active_drag_target_ != DragTarget::None &&
          mouse.motion == ftxui::Mouse::Moved) {
        const int delta = drag_origin_y_ - mouse.y;
        if (active_drag_target_ == DragTarget::WorkPanel) {
          work_panel_height_override_ =
              std::clamp(drag_origin_work_panel_height_ + delta, 4,
                         std::max(6, last_terminal_height_ - 8));
        } else if (active_drag_target_ == DragTarget::AgentStrip) {
          agent_strip_visible_rows_ =
              std::clamp(drag_origin_agent_strip_rows_ + delta, 1, 12);
          updateAgentStripModel();
        }
        persistUserPreferences();
        if (screen_) {
          postEvent(ftxui::Event::Custom);
        }
        return true;
      }
      if (active_drag_target_ != DragTarget::None &&
          mouse.motion == ftxui::Mouse::Released) {
        active_drag_mouse_.reset();
        active_drag_target_ = DragTarget::None;
        return true;
      }
    }

    if (event.is_mouse()) {
      auto &m = event.mouse();
      if (m.button == ftxui::Mouse::WheelUp ||
          m.button == ftxui::Mouse::WheelDown) {
        const bool hasPlan = false;
        const bool hasTodo = todo_lane_model_ && todo_lane_model_->visible;
        const bool hasContext =
            context_lane_model_ && context_lane_model_->visible;
        const auto visibleTab = normalizeWorkPanelTab(
            selected_work_panel_tab_, hasPlan, hasTodo, hasContext);
        if (visibleTab == WorkPanelTab::Context && context_lane &&
            context_lane->OnEvent(event)) {
          return true;
        }
        if (visibleTab == WorkPanelTab::Todo && todo_lane &&
            todo_lane->OnEvent(event)) {
          return true;
        }
        if (chat_component_ && chat_component_->OnEvent(event)) {
          return true;
        }
      }
    }
    if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown ||
        event == ftxui::Event::Home || event == ftxui::Event::End) {
      if (chat_component_) {
        return chat_component_->OnEvent(event);
      }
    }
    if (event == ftxui::Event::Escape) {
      if (edit_mode_active_) {
        edit_mode_active_ = false;
        if (input_model_) {
          input_model_->is_focused = true;
        }
        if (chat_component_) {
          chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
        }
        if (screen_) {
          postEvent(ftxui::Event::Custom);
        }
        return true;
      }
      if (!modals_.empty()) {
        popModal();
        return true;
      }
      if (harness_)
        harness_->abortAndFlushQueuedMessages();
      return true;
    }
    if (IsModeCycleEvent(event)) {
      if (view_mode_ == ViewMode::Welcome) {
        const std::string persona = thread_.leadPersona.empty()
                                        ? std::string("lead")
                                        : thread_.leadPersona;
        thread_.initialMode = cycleMode(thread_.initialMode, persona, +1);
        NotificationManager::instance().notifyInfo(
            "Mode",
            thread_.initialMode.empty() ? "Initial mode cleared."
                                        : "Initial mode: " + thread_.initialMode,
            std::chrono::milliseconds(1500));
        if (screen_) {
          postEvent(ftxui::Event::Custom);
        }
        return true;
      }

      auto agent = firmius::core::AgentRegistry::instance().getAgent(
          focused_agent_id_);
      if (!agent) {
        NotificationManager::instance().notifyWarning(
            "Mode", "No focused agent.", std::chrono::milliseconds(1500));
        return true;
      }
      auto &agentCtx = agent->getMutableContext();
      const std::string next =
          cycleMode(agentCtx.state.activeMode, agentCtx.config.personaName, +1);
      agentCtx.state.activeMode = next;
      NotificationManager::instance().notifyInfo(
          "Mode",
          next.empty() ? "Mode cleared on " + agentCtx.identity.friendlyName
                       : "Mode: " + next + " (" +
                             agentCtx.identity.friendlyName + ")",
          std::chrono::milliseconds(1500));
      if (screen_) {
        postEvent(ftxui::Event::Custom);
      }
      return true;
    }
    if (IsPermissionCycleEvent(event)) { // Ctrl+L (Permission Mode)
      cycleThreadPermissionMode();
      return true;
    }
    if (IsRetryLastRequestEvent(event)) {
      if (!harness_) {
        return true;
      }
      std::string statusMessage;
      if (harness_->retryLastRequest(statusMessage)) {
        NotificationManager::instance().notifyInfo(
            "Retry Request", statusMessage, std::chrono::milliseconds(1800));
      } else {
        NotificationManager::instance().notifyWarning(
            "Retry Request", statusMessage, std::chrono::milliseconds(1800));
      }
      return true;
    }
    if (IsTranscriptUndoEvent(event)) {
      triggerTranscriptUndoFromHotkey();
      return true;
    }
    if (IsTranscriptRedoEvent(event)) {
      triggerTranscriptRedoFromHotkey();
      return true;
    }
    if (IsEditUndoEvent(event)) {
      triggerEditUndoFromHotkey();
      return true;
    }
    if (IsEditRedoEvent(event)) {
      triggerEditRedoFromHotkey();
      return true;
    }
    if (event.input() == "\x1b[Z") { // Shift+Tab (cycle lead purpose)
      if (!harness_)
        return true;

      if (!focused_agent_id_.empty()) {
        auto agent = firmius::core::AgentRegistry::instance().getAgent(
            focused_agent_id_);
        if (agent) {
          const auto &ctx = agent->getContext();
          if (!ctx.identity.parentId.empty()) {
            NotificationManager::instance().notifyWarning(
                "Lead Purpose",
                "Cannot switch purpose while focused on a subagent.",
                std::chrono::milliseconds(1500));
            return true;
          }
          if (agent->isRunning() || agent->isBooting()) {
            NotificationManager::instance().notifyWarning(
                "Lead Purpose",
                "Lead agent is busy. Cancel or wait before switching.",
                std::chrono::milliseconds(1500));
            return true;
          }
        }
      }

      auto modes = getSwitchableLeadPersonas(harness_);
      std::string current;
      if (!focused_agent_id_.empty()) {
        auto agent = firmius::core::AgentRegistry::instance().getAgent(
            focused_agent_id_);
        if (agent) {
          current = agent->getContext().identity.name;
        }
      }
      if (current.empty() && !thread_.leadPersona.empty()) {
        current = thread_.leadPersona;
      }
      if (current.empty()) {
        current = resolveDefaultLeadPersona(harness_);
      }

      size_t next_index = 0;
      auto it = std::find(modes.begin(), modes.end(), current);
      if (it != modes.end()) {
        next_index =
            (static_cast<size_t>(it - modes.begin()) + 1) % modes.size();
      }
      std::string next = modes[next_index];

      auto cfg = harness_->getConfig();
      cfg.defaultLeadPersona = next;
      harness_->updateConfig(cfg);
      harness_->saveConfig();

      bool switched = false;
      if (!thread_.threadId.empty() &&
          harness_->currentThreadId() == thread_.threadId) {
        switched = harness_->switchLeadPersona(next);
        if (switched) {
          focused_agent_id_ = harness_->focusedAgentId();
          refreshFocusedHistory();
        }
      }
      thread_.leadPersona = next;

      updateStatusModel();
      updateAgentStripModel();
      updateTodoLaneModel();
      updateContextLaneModel();
      if (chat_component_) {
        chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
      }
      if (screen_) {
        postEvent(ftxui::Event::Custom);
      }

      NotificationManager::instance().notifyInfo(
          "Lead Purpose", "Lead persona: " + resolvePersonaTitle(next),
          std::chrono::milliseconds(1500));
      return true;
    }
    if (event == ftxui::Event::Special("\x10")) { // Ctrl+P (Parent)
      if (harness_) {
        auto agent = firmius::core::AgentRegistry::instance().getAgent(
            focused_agent_id_);
        if (agent && !agent->getContext().identity.parentId.empty()) {
          std::string parentId = agent->getContext().identity.parentId;
          if (harness_->setFocusedAgent(parentId)) {
            focused_agent_id_ = parentId;
            refreshFocusedHistory();
            if (chat_component_)
              chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
            updateStatusModel();
            updateAgentStripModel();
            updateTodoLaneModel();
            updateContextLaneModel();
            if (screen_)
              postEvent(ftxui::Event::Custom);
          }
        }
      }
      return true;
    }

    if (event == ftxui::Event::Special("\x0E") ||
        event ==
            ftxui::Event::Special("\x02")) { // Ctrl+N (Next), Ctrl+B (Prev)
      if (harness_) {
        auto agents = focusCycleCandidates(focused_agent_id_);
        if (!agents.empty()) {
          auto it = std::find(agents.begin(), agents.end(), focused_agent_id_);
          if (it == agents.end()) {
            it = agents.begin();
          } else if (event == ftxui::Event::Special("\x0E")) { // Next
            ++it;
            if (it == agents.end())
              it = agents.begin();
          } else { // Prev
            if (it == agents.begin())
              it = agents.end();
            --it;
          }
          if (harness_->setFocusedAgent(*it)) {
            focused_agent_id_ = *it;
            refreshFocusedHistory();
            if (chat_component_)
              chat_component_->OnEvent(
                  ftxui::Event::Special("ThreadChanged"));
            updateStatusModel();
            updateAgentStripModel();
            updateTodoLaneModel();
            updateContextLaneModel();
            if (screen_)
              postEvent(ftxui::Event::Custom);
          }
        }
      }
      return true;
    }

    if (event == ftxui::Event::Special("\x14")) { // Ctrl+T (Cycle Theme)
      ThemeManager::instance().cycleTheme();
      if (chat_component_) {
        chat_component_->OnEvent(ftxui::Event::Special("ThemeChanged"));
      }
      NotificationManager::instance().notifyInfo(
          "Theme Changed", ThemeManager::instance().getCurrentTheme().name,
          std::chrono::milliseconds(1500));
      return true;
    }

    if (IsVariantCycleEvent(event)) {
      if (harness_ && !focused_agent_id_.empty()) {
        auto agent = firmius::core::AgentRegistry::instance().getAgent(
            focused_agent_id_);
        if (agent) {
          const auto &ctx = agent->getContext();
          auto provider =
              firmius::provider::ProviderRegistry::instance().getProvider(
                  ctx.config.providerId);
          if (provider) {
            auto info = provider->getModelInfo(ctx.config.modelId);
            if (!info.variants.empty()) {
              std::string current = ctx.config.modelVariant;
              std::string next = "";
              if (current.empty()) {
                next = info.variants.front().variantName;
              } else {
                auto it = std::find_if(
                    info.variants.begin(), info.variants.end(),
                    [&](const auto &v) { return v.variantName == current; });
                if (it != info.variants.end() &&
                    std::next(it) != info.variants.end()) {
                  next = std::next(it)->variantName;
                } else {
                  next = "";
                }
              }
              harness_->switchModel(ctx.config.providerId, ctx.config.modelId,
                                    next);
              updateStatusModel();
              if (chat_component_) {
                chat_component_->OnEvent(
                    ftxui::Event::Special("ThreadChanged"));
              }
            }
          }
        }
      }
      return true;
    }

    // Ctrl+H - Toggle notifications
    if (event == ftxui::Event::Special("\x08")) {
      NotificationManager::instance().toggleVisibility();
      return true;
    }

    // Ctrl+E - Toggle edit mode / apply selected edit
    if (event == ftxui::Event::Special("\x05")) {
      if (!edit_mode_active_) {
        rebuildEditableUserMessages();
        edit_mode_active_ = !editable_user_messages_.empty();
      if (edit_mode_active_ && input_model_) {
          input_model_->is_focused = false;
        }
        if (chat_component_) {
          chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
        }
      } else if (selected_editable_message_index_ >= 0) {
        commitSelectedEditableMessageToInput();
      } else {
        edit_mode_active_ = false;
        if (input_model_) {
          input_model_->is_focused = true;
        }
        if (chat_component_) {
          chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
        }
      }
      if (screen_) {
        postEvent(ftxui::Event::Custom);
      }
      return true;
    }

    if (edit_mode_active_ && event == ftxui::Event::Return) {
      if (selected_editable_message_index_ >= 0) {
        commitSelectedEditableMessageToInput();
      }
      return true;
    }

    if (edit_mode_active_ &&
        (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown)) {
      if (!editable_user_messages_.empty()) {
        const int dir = event == ftxui::Event::ArrowUp ? -1 : 1;
        if (selected_editable_message_index_ < 0) {
          selected_editable_message_index_ = 0;
        } else {
          selected_editable_message_index_ = std::clamp(
              selected_editable_message_index_ + dir, 0,
              static_cast<int>(editable_user_messages_.size()) - 1);
        }
        if (screen_) {
          if (chat_component_) {
            chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
          }
          postEvent(ftxui::Event::Custom);
        }
      }
      return true;
    }
    if (edit_mode_active_ &&
        (event.is_character() || event == ftxui::Event::Backspace ||
         event == ftxui::Event::Delete || event == ftxui::Event::Tab)) {
      return true;
    }

    // F6 - toggle agent strip
    if (event == ftxui::Event::F6) {
      show_agent_strip_ = !show_agent_strip_;
      persistUserPreferences();
      if (screen_) {
        postEvent(ftxui::Event::Custom);
      }
      return true;
    }

    // F7 - toggle work panel
    if (event == ftxui::Event::F7) {
      show_work_panel_ = !show_work_panel_;
      persistUserPreferences();
      if (screen_) {
        postEvent(ftxui::Event::Custom);
      }
      return true;
    }

    // Ctrl+O - Cycle work-lane tabs
    if (event == ftxui::Event::Special("\x0F")) {
      const bool hasPlan = false;
      const bool hasTodo = todo_lane_model_ && todo_lane_model_->visible;
      const bool hasContext =
          context_lane_model_ && context_lane_model_->visible;
      selected_work_panel_tab_ =
          nextWorkPanelTab(selected_work_panel_tab_, hasPlan, hasTodo,
                           hasContext);
      if (screen_) {
        postEvent(ftxui::Event::Custom);
      }
      persistUserPreferences();
      return true;
    }

    // Ctrl+G - Toggle diff expansion
    if (event == ftxui::Event::Special("\x07")) {
      diffs_expanded_ = !diffs_expanded_;
      UIState::instance().diffsExpanded = diffs_expanded_;

      if (diffs_expanded_) {
        NotificationManager::instance().notifyInfo(
            "Diffs Expanded", "Showing full diff content",
            std::chrono::milliseconds(2000));
      } else {
        NotificationManager::instance().notifyInfo(
            "Diffs Collapsed", "Showing top 3 relevant hunks (10 lines max)",
            std::chrono::milliseconds(2000));
      }
      return true;
    }

    // ? / F1 - Open help overlay
    if (event == ftxui::Event::Character("?")) {
      if (input_.empty() && cursor_ == 0) {
        openModalDirect(HelpOverlay(*this));
        return true;
      }
    }
    if (event == ftxui::Event::F1) {
      if (modals_.empty()) {
        openModalDirect(HelpOverlay(*this));
        return true;
      }
    }

    // Ctrl+F - Focus on process (interactive mode)
    if (event == ftxui::Event::Special("\x06")) {
      if (!focused_agent_id_.empty()) {
        auto agent = firmius::core::AgentRegistry::instance().getAgent(
            focused_agent_id_);
        if (agent) {
          const auto &ctx = agent->getContext();
          if (!ctx.state.ownedProcesses.empty()) {
            focused_process_id_ = ctx.state.ownedProcesses.front();
            view_mode_ = ViewMode::ProcessFocus;
            NotificationManager::instance().notifyInfo(
                "Process Focus",
                "Interactive mode enabled. Type to send input to process.",
                std::chrono::milliseconds(3000));
            return true;
          }
        }
      }
      NotificationManager::instance().notifyWarning(
          "No Process", "No active background process to focus on.",
          std::chrono::milliseconds(2000));
      return true;
    }

    // Exit process focus mode with Escape
    if (event == ftxui::Event::Escape && view_mode_ == ViewMode::ProcessFocus) {
      view_mode_ = ViewMode::Chat;
      focused_process_id_.clear();
      NotificationManager::instance().notifyInfo(
          "Process Focus", "Exited process focus mode.",
          std::chrono::milliseconds(2000));
      return true;
    }

    return false;
  });

  root_component_ = ftxui::Make<PreemptiveEventComponent>(
      routed_component, [this](ftxui::Event event) {
        if (event != ftxui::Event::Escape) {
          return false;
        }
        if (pending_permission_request_) {
          const std::string requestId = pending_permission_request_->requestId;
          promoteNextPermissionRequest();
          if (harness_) {
            harness_->resolvePermissionEscalation(
                requestId, PermissionResponse::Deny);
          }
          if (input_component_) {
            input_component_->TakeFocus();
          }
          if (screen_) {
            postEvent(ftxui::Event::Custom);
          }
          return true;
        }
        if (!modals_.empty()) {
          popModal();
          return true;
        }
        if (harness_) {
          harness_->abortAndFlushQueuedMessages();
        }
        return true;
      });

  return root_component_;
}

// ---------------------------------------------------------------------------
// Loading-state plumbing (used by startup pane, /new, /benchmark, etc.).
// All accessors are mutex-guarded so background tasks can poke them safely.
// ---------------------------------------------------------------------------

void TuiState::setLoadingMessage(std::string message) {
  {
    std::lock_guard<std::mutex> lk(loading_message_mutex_);
    loading_message_ = std::move(message);
  }
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
}

void TuiState::clearLoadingMessage() { setLoadingMessage(std::string{}); }

std::string TuiState::loadingMessage() const {
  std::lock_guard<std::mutex> lk(loading_message_mutex_);
  return loading_message_;
}

void TuiState::setLoadingDetail(std::string detail) {
  {
    std::lock_guard<std::mutex> lk(loading_detail_mutex_);
    loading_detail_ = std::move(detail);
  }
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
}

void TuiState::clearLoadingDetail() { setLoadingDetail(std::string{}); }

std::string TuiState::loadingDetail() const {
  std::lock_guard<std::mutex> lk(loading_detail_mutex_);
  return loading_detail_;
}

void TuiState::setLoadingProgress(float progress) {
  {
    std::lock_guard<std::mutex> lk(loading_progress_mutex_);
    loading_progress_ = progress;
  }
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
}

void TuiState::clearLoadingProgress() { setLoadingProgress(-1.0f); }

float TuiState::loadingProgress() const {
  std::lock_guard<std::mutex> lk(loading_progress_mutex_);
  return loading_progress_;
}

void TuiState::clearLoadingState() {
  clearLoadingMessage();
  clearLoadingDetail();
  clearLoadingProgress();
}

void TuiState::expireLoadingStateIfNeeded() {
  if (!loading_auto_clear_at_.has_value()) {
    return;
  }
  if (std::chrono::steady_clock::now() >= *loading_auto_clear_at_) {
    loading_auto_clear_at_.reset();
    clearLoadingState();
  }
}

// ---------------------------------------------------------------------------
// Background work
// ---------------------------------------------------------------------------

void TuiState::runBackgroundTask(std::function<void()> action) {
  if (!action) {
    return;
  }
  if (!background_task_pool_) {
    background_task_pool_ = std::make_unique<BackgroundTaskPool>(4);
  }
  background_task_pool_->post(std::move(action));
}

// ---------------------------------------------------------------------------
// Prompt + thread-open shims (kept on TuiState directly; the welcome→chat
// path lives inside root()'s send callback). These exist so command/modal
// callers and tests can drive the UI without reaching into root().
// ---------------------------------------------------------------------------

void TuiState::submitPrompt(std::string text,
                            std::vector<firmius::shared::ImageContent> images) {
  if (!harness_) {
    return;
  }
  if (view_mode_ == ViewMode::Welcome) {
    const std::string cwd = std::filesystem::current_path().string();
    std::string lead;
    if (!thread_.leadPersona.empty()) {
      lead = thread_.leadPersona;
    } else if (harness_) {
      const auto &cfg = harness_->getConfig();
      lead = cfg.defaultLeadPersona.empty() ? std::string("lead")
                                            : cfg.defaultLeadPersona;
    } else {
      lead = "lead";
    }
    harness_->newThread({}, cwd, lead);
    harness_->setCurrentThreadPermissionMode(thread_.permissionMode);
    syncCurrentThreadMetadataFromHarness(true);
    appendOptimisticUserTurn(text, images);
    notifyChatTranscriptChanged();
    setViewMode(ViewMode::Chat);
  }
  harness_->send(text, images);
}

void TuiState::applyThreadOpened(const shared::ThreadMetadata &metadata,
                                 const std::string &focused_agent_id,
                                 bool preserve_live_state) {
  thread_ = metadata;
  if (!focused_agent_id.empty()) {
    focused_agent_id_ = focused_agent_id;
    if (harness_ && harness_->focusedAgentId() != focused_agent_id_) {
      harness_->setFocusedAgent(focused_agent_id_);
    }
  }
  if (preserve_live_state) {
    syncCurrentThreadMetadataFromHarness(true);
  } else {
    handleAppEvent(shared::ThreadChanged{thread_.threadId, thread_});
  }
  setViewMode(ViewMode::Chat);
  clearLoadingState();
}

void TuiState::requestThreadOpen(std::optional<std::string> thread_id,
                                 bool resume_last,
                                 std::string loading_message,
                                 std::string loading_detail) {
  if (!loading_message.empty()) {
    setLoadingMessage(std::move(loading_message));
  }
  if (!loading_detail.empty()) {
    setLoadingDetail(std::move(loading_detail));
  }
  setLoadingProgress(0.1f);

  runBackgroundTask([this, thread_id = std::move(thread_id), resume_last]() {
    if (!harness_) {
      deferUiMutation([this]() { clearLoadingState(); });
      return;
    }
    try {
      std::string opened_id;
      if (resume_last) {
        if (harness_->resumeLast()) {
          opened_id = harness_->currentThreadId();
        }
      } else if (thread_id.has_value() && !thread_id->empty()) {
        if (harness_->switchThread(*thread_id)) {
          opened_id = *thread_id;
        }
      }
      shared::ThreadMetadata metadata;
      bool found = false;
      if (!opened_id.empty()) {
        for (const auto &t : harness_->listThreads()) {
          if (t.threadId == opened_id) {
            metadata = t;
            found = true;
            break;
          }
        }
      }
      const std::string focused = harness_->focusedAgentId();
      deferUiMutation([this, metadata, focused, found]() {
        if (found) {
          applyThreadOpened(metadata, focused, true);
        }
        clearLoadingState();
      });
    } catch (const std::exception &e) {
      const std::string what = e.what();
      deferUiMutation([this, what]() {
        clearLoadingState();
        NotificationManager::instance().notifyError(
            "Thread Open Failed", what, false);
      });
    } catch (...) {
      deferUiMutation([this]() {
        clearLoadingState();
        NotificationManager::instance().notifyError(
            "Thread Open Failed", "Unknown error.", false);
      });
    }
  });
}

// ---------------------------------------------------------------------------
// Misc declared-but-unused helpers (kept to honor the public header API).
// ---------------------------------------------------------------------------

void TuiState::initModels() {
  // Lazily ensure the view-models exist. init() builds them when a Harness
  // is attached; tests that drive TuiState without a screen rely on this
  // method to populate the same shapes.
  if (!title_model_) title_model_ = std::make_shared<TitleBarModel>();
  if (!status_model_) status_model_ = std::make_shared<StatusBarModel>();
  if (!input_model_) {
    input_model_ = std::make_shared<InputBarModel>();
    input_model_->buffer = &input_;
    input_model_->cursor = &cursor_;
  }
  if (!agent_strip_model_) agent_strip_model_ = std::make_shared<AgentStripModel>();
  plan_lane_model_.reset();
  if (!todo_lane_model_) todo_lane_model_ = std::make_shared<TodoLaneModel>();
  if (!context_lane_model_) context_lane_model_ = std::make_shared<ContextLaneModel>();
}

void TuiState::triggerTranscriptUndoToUserBoundaryFromHotkey() {
  // Same semantics as the standard transcript undo for now (08fd932 collapsed
  // multi-turn undo to user boundary on Ctrl+Z). Kept as a separate hook so
  // the keybinding system can target it independently.
  triggerTranscriptUndoFromHotkey();
}

void TuiState::postAction(UiAction action) {
  ui_action_queue_.push(std::move(action));
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
}

void TuiState::dispatchAction(const UiAction & /*action*/) {
  // Reserved for future UiAction-driven dispatch. The current god-class
  // root() handles flow inline; this hook exists so external producers can
  // still queue typed actions without crashing.
}

void TuiState::requestRender(RefreshFlags flags) {
  pending_render_flags_ |= static_cast<unsigned int>(flags);
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
}

void TuiState::applyPendingRenders() {
  const unsigned int flags = pending_render_flags_;
  if (flags == 0) {
    return;
  }
  pending_render_flags_ = 0;
  ++render_generation_;
  for (std::size_t bit = 0; bit < render_surface_generation_.size(); ++bit) {
    if (flags & (1u << bit)) {
      ++render_surface_generation_[bit];
    }
  }
}

uint64_t TuiState::renderGeneration(RefreshFlags flag) const {
  const unsigned int v = static_cast<unsigned int>(flag);
  if (v == 0) return render_generation_;
  for (std::size_t bit = 0; bit < render_surface_generation_.size(); ++bit) {
    if (v & (1u << bit)) {
      return render_surface_generation_[bit];
    }
  }
  return render_generation_;
}

void TuiState::scheduleQuotaRefresh(const std::string & /*providerId*/) {
  // Quota refresh is driven by ProvidersModal/StatusBar today; this is a
  // declared hook reserved for a future centralized scheduler.
}

void TuiState::drainDeferredUiMutations() {
  std::vector<std::function<void()>> drained;
  {
    std::lock_guard<std::mutex> lk(deferred_ui_mutations_mutex_);
    drained.swap(deferred_ui_mutations_);
  }
  for (auto &fn : drained) {
    if (fn) fn();
  }
}

} // namespace firmius::tui
