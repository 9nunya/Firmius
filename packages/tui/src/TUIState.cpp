#include "TUIState.hpp"
#include "AgentRegistry.hpp"
#include "ConfigLoader.hpp"
#include "Enums.hpp"
#include "NotificationManager.hpp"
#include "ThemeManager.hpp"
#include "UserPreferences.hpp"
#include "agents/ContextBudget.hpp"
#include "agents/modes/Mode.hpp"
#include "components/ChatWindow.hpp"
#include "controllers/AppController.hpp"
#include "controllers/InputController.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalRegistry.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/ProviderRegistry.hpp"
#include "tui/QuotaPresenter.hpp"
#include "utils/PlatformPaths.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>

namespace {
void trace(const std::string &msg) {
  try {
    std::ofstream f("/tmp/firmius_trace.log", std::ios::app);
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    f << std::put_time(std::localtime(&t), "%H:%M:%S") << " " << msg
      << std::endl;
  } catch (...) {
  }
}
} // namespace

#include "agents/hooks/HookEnvelope.hpp"
#include "agents/hooks/HookState.hpp"
#include "agents/hooks/ScriptRuntime.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <utility>

namespace firmius::tui {

namespace {

std::vector<std::filesystem::path> hookStatusScriptCandidates() {
  std::vector<std::filesystem::path> scripts;
  std::vector<std::filesystem::path> roots;
  auto addRoot = [&roots](const std::filesystem::path &root) {
    if (!std::filesystem::exists(root) ||
        !std::filesystem::is_directory(root)) {
      return;
    }
    if (std::find(roots.begin(), roots.end(), root) == roots.end()) {
      roots.push_back(root);
    }
  };

  if (const char *envDir = std::getenv("FIRMIUS_HOOKS_DIR");
      envDir && *envDir) {
    addRoot(std::filesystem::path(envDir));
  } else if (const char *home = std::getenv("HOME"); home && *home) {
    addRoot(std::filesystem::current_path() / "prompts" / "hooks");
    addRoot(std::filesystem::path(home) / ".firmius" / "prompts" / "hooks");
    addRoot(std::filesystem::path(home) / ".firmius" / "hooks");
  } else {
    addRoot(std::filesystem::current_path() / "prompts" / "hooks");
    addRoot(std::filesystem::current_path() / ".firmius" / "prompts" / "hooks");
    addRoot(std::filesystem::current_path() / ".firmius" / "hooks");
  }

  for (const auto &root : roots) {
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file() || entry.path().filename() != "status.lua") {
        continue;
      }
      const auto parent = entry.path().parent_path();
      if (parent.filename() == "skin") {
        scripts.push_back(entry.path());
      }
    }
  }
  std::sort(scripts.begin(), scripts.end());
  scripts.erase(std::unique(scripts.begin(), scripts.end()), scripts.end());
  return scripts;
}

std::vector<std::string>
formatHookStatusLinesWithLua(const std::string &threadId) {
  std::vector<std::string> lines;
  if (!firmius::core::hooks::ScriptRuntime::enabled()) {
    return lines;
  }
  auto &hookState = firmius::core::hooks::HookState::instance();
  hookState.bindThread(threadId);

  for (const auto &scriptPath : hookStatusScriptCandidates()) {
    if (!std::filesystem::exists(scriptPath)) {
      continue;
    }
    std::ifstream in(scriptPath);
    if (!in.is_open()) {
      continue;
    }
    std::ostringstream body;
    body << in.rdbuf();
    const std::string hookId =
        "tui.status." +
        scriptPath.parent_path().parent_path().filename().string();
    firmius::core::hooks::HookEnvelope env;
    env.hookId = hookId;
    env.hookEvent = "tui_status";
    env.threadId = threadId;
    env.stateSnapshotJson = hookState.snapshotJson(hookId);
    auto runtime = firmius::core::hooks::ScriptRuntime::create();
    auto outcome = runtime->eval(env.hookId, body.str(), env);
    if (outcome.reminderForAgent.has_value() &&
        !outcome.reminderForAgent->empty()) {
      std::istringstream stream(*outcome.reminderForAgent);
      std::string line;
      while (std::getline(stream, line)) {
        if (!line.empty()) {
          lines.push_back(line);
        }
      }
    }
  }
  return lines;
}

template <typename T> void HashCombine(std::size_t &seed, const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

std::optional<std::string> compactionIdFromTurnId(const std::string &turnId) {
  constexpr const char *prefixes[] = {"compaction-start-",
                                      "compaction-summary-", "compaction-end-"};
  for (const char *prefix : prefixes) {
    const std::size_t len = std::char_traits<char>::length(prefix);
    if (turnId.rfind(prefix, 0) == 0) {
      return turnId.substr(len);
    }
  }
  return std::nullopt;
}

bool turnsEquivalentForTranscript(const firmius::shared::AgentTurn &lhs,
                                  const firmius::shared::AgentTurn &rhs) {
  return lhs.turnId == rhs.turnId;
}

bool isTranscriptMeaningfulMessage(const firmius::shared::Message &message) {
  if (message.role == firmius::shared::Role::User ||
      message.role == firmius::shared::Role::Assistant ||
      message.role == firmius::shared::Role::System) {
    for (const auto &part : message.content) {
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
    const std::vector<firmius::shared::AgentTurn> &turns) {
  std::vector<firmius::shared::AgentTurn> filtered;
  filtered.reserve(turns.size());
  for (const auto &turn : turns) {
    bool meaningful = false;
    for (const auto &message : turn.messages) {
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
    const std::vector<firmius::shared::AgentTurn> &snapshotTurns,
    const std::vector<firmius::shared::AgentTurn> &currentTurns,
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
    const std::vector<firmius::shared::AgentTurn> &turns,
    const std::unordered_map<std::string, firmius::core::CompactionSnapshot>
        &snapshots,
    std::unordered_set<std::string> &expanded_ids) {
  std::vector<firmius::shared::AgentTurn> result;
  for (std::size_t i = 0; i < turns.size(); ++i) {
    const auto compactionId = compactionIdFromTurnId(turns[i].turnId);
    if (!compactionId.has_value() ||
        turns[i].turnId.rfind("compaction-start-", 0) != 0) {
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
      result.insert(result.end(), expandedSnapshot.begin(),
                    expandedSnapshot.end());

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

bool parseTuiStartupProfilingEnabledFromEnv() {
  const char *raw = std::getenv("FIRMIUS_TUI_STARTUP_PROFILE");
  if (!raw)
    return false;
  std::string value(raw);
  for (char &ch : value)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return !(value.empty() || value == "0" || value == "false" ||
           value == "off" || value == "no");
}

double nanosToMillis(int64_t nanos) {
  return static_cast<double>(nanos) / 1000000.0;
}

std::string formatCompactCount(uint32_t value) {
  if (value >= 1000000) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << (static_cast<double>(value) / 1000000.0);
    auto text = out.str();
    if (text.size() > 2 && text.substr(text.size() - 2) == ".0") {
      text.erase(text.size() - 2);
    }
    return text + "M";
  }
  if (value >= 1000) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << (static_cast<double>(value) / 1000.0);
    auto text = out.str();
    if (text.size() > 2 && text.substr(text.size() - 2) == ".0") {
      text.erase(text.size() - 2);
    }
    return text + "k";
  }
  return std::to_string(value);
}

std::string agentStatusToDisplayString(firmius::shared::AgentStatus status) {
  switch (status) {
  case firmius::shared::AgentStatus::Idle:
    return "idle";
  case firmius::shared::AgentStatus::Streaming:
    return "streaming";
  case firmius::shared::AgentStatus::ExecutingTool:
    return "executing_tool";
  case firmius::shared::AgentStatus::AwaitingInput:
    return "awaiting_input";
  case firmius::shared::AgentStatus::Compacting:
    return "compacting";
  case firmius::shared::AgentStatus::ProviderWaiting:
    return "provider_waiting";
  case firmius::shared::AgentStatus::Error:
    return "error";
  case firmius::shared::AgentStatus::Cancelled:
    return "cancelled";
  }
  return "unknown";
}

} // namespace

TuiProfilingStats &tuiProfilingStats() {
  static TuiProfilingStats stats;
  return stats;
}

bool isTuiStartupProfilingEnabled() {
  static bool enabled = parseTuiStartupProfilingEnabledFromEnv();
  return enabled;
}

void noteTuiAppEventEnqueued() {
  if (isTuiStartupProfilingEnabled())
    tuiProfilingStats().app_event_enqueued.fetch_add(1,
                                                     std::memory_order_relaxed);
}

void noteTuiCustomEventPosted() {
  if (isTuiStartupProfilingEnabled())
    tuiProfilingStats().custom_event_posted.fetch_add(
        1, std::memory_order_relaxed);
}

void noteTuiCustomEventDrained() {
  if (isTuiStartupProfilingEnabled())
    tuiProfilingStats().custom_event_drained.fetch_add(
        1, std::memory_order_relaxed);
}

void noteTuiOnEventDispatch(std::chrono::nanoseconds elapsed) {
  if (!isTuiStartupProfilingEnabled())
    return;
  auto &stats = tuiProfilingStats();
  stats.on_event_dispatch_count.fetch_add(1, std::memory_order_relaxed);
  stats.on_event_dispatch_ns.fetch_add(static_cast<int64_t>(elapsed.count()),
                                       std::memory_order_relaxed);
}

void noteTuiThreadChanged(std::chrono::nanoseconds elapsed) {
  if (!isTuiStartupProfilingEnabled())
    return;
  auto &stats = tuiProfilingStats();
  stats.thread_changed_count.fetch_add(1, std::memory_order_relaxed);
  stats.thread_changed_ns.fetch_add(static_cast<int64_t>(elapsed.count()),
                                    std::memory_order_relaxed);
}

void noteTuiRebuildToolCalls(std::chrono::nanoseconds elapsed) {
  if (!isTuiStartupProfilingEnabled())
    return;
  auto &stats = tuiProfilingStats();
  stats.rebuild_tool_calls_count.fetch_add(1, std::memory_order_relaxed);
  stats.rebuild_tool_calls_ns.fetch_add(static_cast<int64_t>(elapsed.count()),
                                        std::memory_order_relaxed);
}

void noteTuiChatWindowRebuild(std::chrono::nanoseconds elapsed) {
  if (!isTuiStartupProfilingEnabled())
    return;
  auto &stats = tuiProfilingStats();
  stats.chat_window_rebuild_count.fetch_add(1, std::memory_order_relaxed);
  stats.chat_window_rebuild_ns.fetch_add(static_cast<int64_t>(elapsed.count()),
                                         std::memory_order_relaxed);
}

void noteTuiFrameRendered(std::chrono::nanoseconds elapsed) {
  if (!isTuiStartupProfilingEnabled())
    return;
  auto &stats = tuiProfilingStats();
  stats.frame_render_count.fetch_add(1, std::memory_order_relaxed);
  stats.frame_render_ns.fetch_add(static_cast<int64_t>(elapsed.count()),
                                  std::memory_order_relaxed);
}

std::string tuiProfilingSummaryText() {
  if (!isTuiStartupProfilingEnabled())
    return "";
  const auto &stats = tuiProfilingStats();
  std::ostringstream out;
  out << "TUI Profiling\n";
  out << "AppEvent enqueued: "
      << stats.app_event_enqueued.load(std::memory_order_relaxed) << "\n";
  out << "Event::Custom posted: "
      << stats.custom_event_posted.load(std::memory_order_relaxed) << "\n";
  out << "Event::Custom drained: "
      << stats.custom_event_drained.load(std::memory_order_relaxed) << "\n";

  auto report = [&](const char *label, uint64_t count, int64_t ns) {
    out << label << ": " << count << " (" << std::fixed << std::setprecision(3)
        << nanosToMillis(ns) << " ms total";
    if (count > 0)
      out << ", " << nanosToMillis(ns) / static_cast<double>(count)
          << " ms avg";
    out << ")\n";
  };

  report("onEvent dispatches",
         stats.on_event_dispatch_count.load(std::memory_order_relaxed),
         stats.on_event_dispatch_ns.load(std::memory_order_relaxed));
  report("ThreadChanged handled",
         stats.thread_changed_count.load(std::memory_order_relaxed),
         stats.thread_changed_ns.load(std::memory_order_relaxed));
  report("rebuildToolCallsFromHistory",
         stats.rebuild_tool_calls_count.load(std::memory_order_relaxed),
         stats.rebuild_tool_calls_ns.load(std::memory_order_relaxed));
  report("ChatWindow rebuilds",
         stats.chat_window_rebuild_count.load(std::memory_order_relaxed),
         stats.chat_window_rebuild_ns.load(std::memory_order_relaxed));
  report("Frame renders",
         stats.frame_render_count.load(std::memory_order_relaxed),
         stats.frame_render_ns.load(std::memory_order_relaxed));

  return out.str();
}

std::string TuiState::exitSummaryText() const {
  std::ostringstream out;
  out << "\nFirmius Session Summary\n";
  out << "Thread: " << (thread_.title.empty() ? "New Thread" : thread_.title)
      << "\n";
  out << "Total Prompt Tokens: "
      << AppController::instance().getSessionMetrics().tokens.prompt << "\n";
  out << "Total Completion Tokens: "
      << AppController::instance().getSessionMetrics().tokens.completion
      << "\n";
  out << "Estimated Cost: $" << std::fixed << std::setprecision(4)
      << AppController::instance().getSessionMetrics().estimatedCostUsd << "\n";

  const std::string profiling = tuiProfilingSummaryText();
  if (!profiling.empty())
    out << profiling;
  return out.str();
}

namespace detail {
bool shouldNotifyHiddenChatError(const std::string &focused_id,
                                 const std::string &error_id,
                                 bool hide_errors) {
  if (error_id.empty())
    return false;
  if (!hide_errors)
    return false;
  return !focused_id.empty() && focused_id == error_id;
}
} // namespace detail

std::size_t BuildFocusedChatLiveMeasurementSignature(
    const StreamStateManager &stream_state, const std::string &focused_agent_id,
    const std::string &thread_id,
    const std::unordered_set<std::string> &persisted_tool_call_ids) {
  std::size_t signature = 0;
  HashCombine(signature, focused_agent_id);
  HashCombine(signature, persisted_tool_call_ids.size());
  HashCombine(signature, stream_state.getLiveRenderEpoch());
  auto bucket = [](std::size_t size) { return size / 512; };

  HashCombine(signature, thread_id);
  if (const auto *s = stream_state.getStream(focused_agent_id)) {
    HashCombine(signature, bucket(s->thinking.size()));
    HashCombine(signature, bucket(s->text.size()));
    HashCombine(signature, bucket(s->compaction_thinking.size()));
    HashCombine(signature, bucket(s->compaction_text.size()));
    HashCombine(signature, bucket(s->compaction_completion.size()));
    HashCombine(signature, s->thinking.size() / 512);
    HashCombine(signature, s->compaction_finished);
    HashCombine(signature, s->text.size() / 512);
    HashCombine(signature, s->compaction_active);
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
    if (view && view->agentId == focused_agent_id) {
      focused_tool_calls++;
      HashCombine(signature, static_cast<int>(view->phase));
    }
  }
  HashCombine(signature, focused_tool_calls);

  HashCombine(signature, stream_state.getQueuedMessages().size());
  HashCombine(signature, stream_state.getQueuedInternalMessages().size());

  return signature;
}

std::vector<shared::AgentTurn> expandCompactionTranscriptForDisplay(
    const std::vector<shared::AgentTurn> &turns,
    const std::unordered_map<std::string, firmius::core::CompactionSnapshot>
        &snapshots) {
  std::unordered_set<std::string> expanded_ids;
  return expandCompactionTurns(turns, snapshots, expanded_ids);
}

void noteTuiModalOpenRequested(const std::string &name) { (void)name; }

TuiState &TuiState::instance() {
  static TuiState inst;
  return inst;
}

TuiState::TuiState() { loadUserPreferences(); }

void TuiState::initModels() {
  auto &store = TUIStore::instance();
  title_model_ = store.title_model;
  status_model_ = store.status_model;
  input_model_ = store.input_model;
  agent_strip_model_ = store.agent_strip_model;
  plan_lane_model_ = store.plan_lane_model;
  todo_lane_model_ = store.todo_lane_model;
  context_lane_model_ = store.context_lane_model;

  if (input_model_) {
    input_model_->placeholder = "Type a message...";
    input_model_->compact_mode = (skin_config_.kind == SkinKind::Claudex);
  }

  // CRITICAL: Ensure models have current state after initial assignment
  if (status_model_)
    status_model_->compact_skin_mode = (skin_config_.kind == SkinKind::Claudex);
}

void TuiState::loadUserPreferences() {
  const auto prefs = firmius::tui::loadUserPreferences();
  if (prefs.preferred_permission_mode.has_value()) {
    thread_.permissionMode = *prefs.preferred_permission_mode;
  }
  if (prefs.skin_kind.has_value()) {
    if (*prefs.skin_kind == SkinKind::Claudex &&
        prefs.claudex_skin.has_value()) {
      applySkinConfig(*prefs.claudex_skin);
    } else if (*prefs.skin_kind == SkinKind::Firmius &&
               prefs.firmius_skin.has_value()) {
      applySkinConfig(*prefs.firmius_skin);
    } else {
      skin_config_ = defaultSkinConfig(*prefs.skin_kind);
      skin_config_.kind = *prefs.skin_kind;
      ThemeManager::instance().setTheme(
          skin_config_.kind == SkinKind::Claudex ? "Claudex" : "Firmius");
      TUIStore::instance().skin_kind = *prefs.skin_kind;
      TUIStore::instance().skin_config = skin_config_;
    }
  }
}
void TuiState::clearInputBuffer() {
  if (input_model_ && input_model_->buffer) {
    input_model_->buffer->clear();
    if (input_model_->cursor)
      *input_model_->cursor = 0;
  }
}

void TuiState::persistUserPreferences() const {
  UserPreferences prefs;
  prefs.preferred_permission_mode = thread_.permissionMode;
  prefs.skin_kind = skin_config_.kind;
  if (skin_config_.kind == SkinKind::Claudex) {
    prefs.claudex_skin = skin_config_;
  } else {
    prefs.firmius_skin = skin_config_;
  }
  saveUserPreferences(prefs);
}

void TuiState::init(firmius::core::Harness &harness,
                    const shared::ThreadMetadata &thread,
                    const std::string &focused_agent_id) {
  harness_ = &harness;
  thread_ = thread;
  const auto prefs = firmius::tui::loadUserPreferences();
  if (prefs.preferred_permission_mode.has_value()) {
    if (thread_.threadId.empty()) {
      thread_.permissionMode = *prefs.preferred_permission_mode;
    } else if (thread_.permissionMode ==
                   shared::ThreadPermissionMode::Request &&
               *prefs.preferred_permission_mode !=
                   shared::ThreadPermissionMode::Request) {
      harness_->setCurrentThreadPermissionMode(
          *prefs.preferred_permission_mode);
      thread_.permissionMode = *prefs.preferred_permission_mode;
    }
  }
  focused_agent_id_ = focused_agent_id;
  TUIStore::instance().thread_metadata = thread;
  TUIStore::instance().thread_id = thread.threadId;
  TUIStore::instance().focused_agent_id = focused_agent_id;
  TUIStore::instance().view_mode = thread.threadId.empty()
                                       ? TUIStore::ViewMode::Welcome
                                       : TUIStore::ViewMode::Chat;

  initModels();

  if (title_model_) {
    title_model_->title = thread.title;
    title_model_->thread_id = thread.threadId;
  }

  subscription_id_ =
      harness_->subscribe([this](const firmius::shared::AppEvent &ev) {
        ui_action_queue_.push(UiCoreEventReceived{ev});
        noteTuiAppEventEnqueued();
        if (screen_) {
          postEvent(ftxui::Event::Custom);
        }
      });

  refreshFocusedHistory();
  requestRefresh(RefreshFlags::Status);
  requestRefresh(RefreshFlags::AgentStrip);
  applyPendingRefreshes();
}

void TuiState::syncCurrentThreadMetadataFromHarness(bool preserve_live_state) {
  if (!harness_)
    return;
  const std::string curId = harness_->currentThreadId();
  if (curId.empty())
    return;

  for (const auto &metadata : harness_->listThreads()) {
    if (metadata.threadId != curId)
      continue;
    if (!preserve_live_state) {
      handleAppEvent(shared::ThreadChanged{curId, metadata});
      return;
    }
    applyThreadOpened(metadata,
                      harness_ ? harness_->focusedAgentId()
                               : TUIStore::instance().focused_agent_id,
                      true);
    break;
  }
}

void TuiState::refreshFocusedHistory() {
  if (!harness_ || focused_agent_id_.empty()) {
    history_.reset();
    return;
  }

  // Real implementation for expandHistoryForTranscript bridge
  auto base_history = harness_->getAgentHistoryPtr(focused_agent_id_);
  if (!base_history) {
    if (history_ && !history_->turns.empty()) {
      TranscriptModel::instance().active_history = history_;
      agent_history_cache_[focused_agent_id_] = history_;
      return;
    }
    history_.reset();
    TranscriptModel::instance().active_history.reset();
    return;
  }

  bool hasCompaction = false;
  for (const auto &turn : base_history->turns) {
    if (compactionIdFromTurnId(turn.turnId).has_value()) {
      hasCompaction = true;
      break;
    }
  }

  if (!hasCompaction) {
    history_ = base_history;
  } else {
    firmius::core::ThreadManager tm(
        (firmius::shared::PlatformPaths::firmiusHomeDir() / "threads")
            .string());
    const auto snapshotList =
        tm.loadCompactionSnapshots(thread_.threadId, focused_agent_id_);
    std::unordered_map<std::string, firmius::core::CompactionSnapshot>
        snapshots;
    for (const auto &snapshot : snapshotList) {
      if (!snapshot.compactionId.empty())
        snapshots[snapshot.compactionId] = snapshot;
    }

    auto expanded =
        std::make_shared<firmius::shared::AgentHistory>(*base_history);
    expanded->turns =
        expandCompactionTranscriptForDisplay(base_history->turns, snapshots);
    history_ = expanded;
  }

  TranscriptModel::instance().active_history = history_;
  agent_history_cache_[focused_agent_id_] = history_;
  agent_persisted_tool_call_ids_cache_[focused_agent_id_] =
      firmius::tui::CollectToolCallIdsFromHistory(history_.get());
}

void TuiState::shutdown() {
  if (harness_ && subscription_id_ >= 0) {
    if (animation_tick_thread_.joinable()) {
      animation_tick_thread_.request_stop();
      animation_tick_thread_.join();
    }
    harness_->unsubscribe(subscription_id_);
    subscription_id_ = -1;
  }
  {
    std::lock_guard<std::mutex> lock(background_ui_tasks_mutex_);
    for (auto &task : background_ui_tasks_) {
      if (task.joinable())
        task.request_stop();
    }
    background_ui_tasks_.clear();
  }
  screen_ = nullptr;
}

void TuiState::requestQuit() {
  quit_arm_deadline_.reset();
  if (screen_)
    screen_->ExitLoopClosure()();
}

bool TuiState::isQuitArmed() const {
  return quit_arm_deadline_.has_value() &&
         std::chrono::steady_clock::now() <= *quit_arm_deadline_;
}

void TuiState::drainEvents() {
  custom_event_pending_ = false;
  // Drain until stable so we don't leave events stranded until next interaction
  while (true) {
    auto drained = ui_action_queue_.drainAll();
    if (drained.empty())
      break;
    for (const auto &action : drained) {
      dispatchAction(action);
    }
  }
  drainDeferredUiMutations();
  applyPendingRefreshes();
}

bool TuiState::hasActiveThread() const { return !thread_.threadId.empty(); }
std::string TuiState::currentThreadId() const { return thread_.threadId; }
shared::ThreadPermissionMode TuiState::currentThreadPermissionMode() const {
  return thread_.permissionMode;
}
bool TuiState::cycleThreadPermissionMode() {
  if (!harness_)
    return false;

  auto mode = thread_.permissionMode;
  if (mode == shared::ThreadPermissionMode::Request) {
    mode = shared::ThreadPermissionMode::AlwaysAllow;
  } else {
    mode = shared::ThreadPermissionMode::Request;
  }

  if (harness_->setCurrentThreadPermissionMode(mode)) {
    thread_.permissionMode = mode;
    NotificationManager::instance().notifyInfo(
        "Permissions",
        std::string("Mode: ") +
            (mode == shared::ThreadPermissionMode::AlwaysAllow
                 ? "Auto-approve"
                 : "Request permission"),
        std::chrono::milliseconds(1500));
    requestRefresh(RefreshFlags::Status);
    return true;
  }
  return false;
}

void TuiState::setViewMode(ViewMode mode) {
  view_mode_ = mode;
  TUIStore::instance().view_mode = static_cast<TUIStore::ViewMode>(mode);
}

TuiState::ViewMode TuiState::getViewMode() const { return view_mode_; }

void TuiState::openModal(const std::string &name) {
  ModalRegistry::instance().openModal(name, *this, true);
}

void TuiState::openModalDirect(ftxui::Component modal,
                               const std::string &modal_name) {
  (void)modal_name;
  modals_.push_back(std::move(modal));
  if (modal)
    modal->TakeFocus();
  if (screen_)
    postEvent(ftxui::Event::Custom);
}

void TuiState::popModal() { postEvent(ftxui::Event::Special("PopModal")); }

void TuiState::popModalImmediate() {
  if (!modals_.empty())
    modals_.pop_back();
  if (modals_.empty()) {
    if (input_model_)
      input_model_->is_focused = true;
    if (main_view_)
      main_view_->TakeFocus();
  } else {
    modals_.back()->TakeFocus();
  }
  if (screen_)
    postEvent(ftxui::Event::Custom);
}

void TuiState::deferUiMutation(std::function<void()> action) {
  {
    std::lock_guard<std::mutex> lock(deferred_ui_mutations_mutex_);
    deferred_ui_mutations_.push_back(std::move(action));
  }
  postEvent(ftxui::Event::Special("DeferredUiMutation"));
}

void TuiState::postAction(UiAction action) {
  ui_action_queue_.push(std::move(action));
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
}

void TuiState::submitPrompt(std::string text,
                            std::vector<firmius::shared::ImageContent> images) {
  dispatchAction(UiPromptSubmitted{std::move(text), std::move(images)});
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
}

void TuiState::requestThreadOpen(std::optional<std::string> thread_id,
                                 bool resume_last, std::string loading_message,
                                 std::string loading_detail) {
  dispatchAction(UiThreadOpenRequested{std::move(thread_id), resume_last, true,
                                       std::move(loading_message),
                                       std::move(loading_detail)});
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
}

[[maybe_unused]] void
TuiState::applyThreadOpened(const shared::ThreadMetadata &metadata,
                            const std::string &focused_agent_id,
                            bool preserve_live_state) {
  if (metadata.threadId.empty()) {
    return;
  }

  auto &store = TUIStore::instance();

  // When we're doing an optimistic welcome->chat transition, we may receive a
  // provisional ThreadOpened before the lead agent is known. In that case we
  // must NOT "guess" focus from the previous thread/store/harness, because that
  // manually re-focuses (e.g. Ctrl+N).
  //
  // We also must not clear a focus that was already established by a later
  // AgentSpawned/strong-focus path. The optimistic bootstrap can enqueue a
  // provisional ThreadOpened with empty focus and then, depending on event
  // ordering, drain it after the real lead has already been focused. Preserving
  // an already-established focus here keeps that strong-focus repair intact.
  std::string final_focus = focused_agent_id;
  if (preserve_live_state && final_focus.empty()) {
    if (!focused_agent_id_.empty()) {
      final_focus = focused_agent_id_;
    } else if (harness_) {
      final_focus = harness_->focusedAgentId();
    }
    if (final_focus.empty() && harness_) {
      final_focus = harness_->focusedAgentId();
    }
  } else if (!preserve_live_state) {
    if (final_focus.empty() && harness_) {
      final_focus = harness_->focusedAgentId();
    }
    if (final_focus.empty()) {
      final_focus = store.focused_agent_id;
    }
    if (final_focus.empty() && harness_) {
      const auto agents = harness_->listAgents(metadata.threadId);
      if (!agents.empty()) {
        final_focus = agents.front();
        (void)harness_->setFocusedAgent(final_focus);
      } else if (final_focus.empty()) {
        if (!focused_agent_id_.empty()) {
          final_focus = focused_agent_id_;
        } else if (!store.focused_agent_id.empty()) {
          final_focus = store.focused_agent_id;
        }
      }
    }
  }

  thread_ = metadata;
  std::cerr << "[DBG] applyThreadOpened writing focused_agent_id_="
            << final_focus << " (was " << focused_agent_id_
            << "), preserve_live=" << preserve_live_state << std::endl;
  focused_agent_id_ = final_focus;
  store.thread_metadata = metadata;
  store.thread_id = metadata.threadId;
  store.focused_agent_id = final_focus;

  if (!preserve_live_state) {
    stream_state_.handleThreadChanged();
  }

  setViewMode(ViewMode::Chat);
  if (!final_focus.empty()) {
    refreshFocusedHistory();
  } else if (history_) {
    TranscriptModel::instance().active_history = history_;
  }
  live_row_current_phrase_.clear();

  requestRefresh(RefreshFlags::AgentStrip);
  requestRefresh(RefreshFlags::Status);
  requestRefresh(RefreshFlags::ContextLane);
  requestRefresh(RefreshFlags::PlanLane);
  requestRefresh(RefreshFlags::TodoLane);
  if (chat_component_) {
    chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
  }

  notifyChatTranscriptChanged();
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
}

void TuiState::dispatchAction(const UiAction &action) {
  std::visit(
      [this](const auto &a) {
        using T = std::decay_t<decltype(a)>;

        if constexpr (std::is_same_v<T, UiCoreEventReceived>) {
          onEvent(a.event);
        } else if constexpr (std::is_same_v<T, UiDeferredMutation>) {
          if (a.action) {
            a.action();
          }
        } else if constexpr (std::is_same_v<T, UiThreadOpened>) {
          clearLoadingState();
          applyThreadOpened(a.metadata, a.focused_agent_id,
                            a.preserve_live_state);
        } else if constexpr (std::is_same_v<T, UiThreadOpenFailed>) {
          clearLoadingState();
          NotificationManager::instance().notifyError(
              a.title.empty() ? "Thread" : a.title,
              a.message.empty() ? "Could not open thread." : a.message, false);
          if (a.return_to_welcome) {
            setViewMode(ViewMode::Welcome);
          }
        } else if constexpr (std::is_same_v<T, UiThreadOpenRequested>) {
          if (!harness_) {
            return;
          }
          clearLoadingState();
          setLoadingMessage(!a.loading_message.empty()
                                ? a.loading_message
                                : (a.resume_last ? "Restoring last thread..."
                                                 : "Opening thread..."));
          if (!a.loading_detail.empty()) {
            setLoadingDetail(a.loading_detail);
          } else {
            setLoadingDetail(a.resume_last
                                 ? "Loading the last active thread from disk."
                                 : "Loading the selected thread from disk.");
          }
          setLoadingProgress(0.2f);

          auto thread_id = a.thread_id;
          const bool resume_last = a.resume_last;
          const bool preserve_live_state = a.preserve_live_state;
          runBackgroundTask([this, thread_id, resume_last,
                             preserve_live_state]() {
            auto &h = firmius::core::Harness::instance();
            bool loaded = false;
            shared::ThreadMetadata metadata;
            std::string focusedAgentId;
            try {
              loaded = resume_last
                           ? h.resumeLast()
                           : (thread_id.has_value() ? h.switchThread(*thread_id)
                                                    : false);
              if (loaded) {
                const std::string currentId = h.currentThreadId();
                focusedAgentId = h.focusedAgentId();
                for (const auto &candidate : h.listThreads()) {
                  if (candidate.threadId == currentId) {
                    metadata = candidate;
                    break;
                  }
                }
              }
            } catch (...) {
              loaded = false;
            }

            if (!loaded || metadata.threadId.empty()) {
              deferUiMutation([this, resume_last]() {
                clearLoadingState();
                NotificationManager::instance().notifyError(
                    "Thread",
                    resume_last ? "Could not restore thread."
                                : "Could not open thread.",
                    false);
                if (resume_last) {
                  setViewMode(ViewMode::Welcome);
                }
              });
              return;
            }
            deferUiMutation([this, metadata, focusedAgentId,
                             preserve_live_state]() {
              clearLoadingState();
              applyThreadOpened(metadata, focusedAgentId, preserve_live_state);
            });
          });
        } else if constexpr (std::is_same_v<T, UiPromptSubmitted>) {
          if (!harness_ || (a.text.empty() && a.images.empty())) {
            return;
          }

          if (getViewMode() != ViewMode::Welcome) {
            harness_->send(a.text, a.images);
            requestRender(RefreshFlags::ChatTranscript);
            return;
          }

          // Optimistic welcome->chat transition: leave the welcome screen
          // immediately and render the user's first message locally while the
          // real thread/lead agent are created in the background.
          setViewMode(ViewMode::Chat);
          if (!history_) {
            history_ = std::make_shared<shared::AgentHistory>();
          }
          shared::AgentTurn optimisticTurn;
          shared::Message optimisticMessage;
          optimisticMessage.id = std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count());
          optimisticMessage.role = shared::Role::User;
          optimisticMessage.content = {shared::TextContent{a.text}};
          for (const auto &image : a.images) {
            optimisticMessage.content.push_back(image);
          }
          optimisticTurn.messages.push_back(std::move(optimisticMessage));
          history_->turns.push_back(std::move(optimisticTurn));
          TranscriptModel::instance().active_history = history_;
          notifyChatTranscriptChanged();
          requestRefresh(RefreshFlags::Status);
          requestRefresh(RefreshFlags::ContextLane);
          applyPendingRefreshes();

          const std::string cwd = std::filesystem::current_path().string();
          auto config = harness_->getConfig();
          std::string leadPersona = thread_.leadPersona.empty()
                                        ? config.defaultLeadPersona
                                        : thread_.leadPersona;
          if (leadPersona.empty()) {
            leadPersona = "aster";
          }
          const std::string initialMode = thread_.initialMode;
          const auto permissionMode = thread_.threadId.empty()
                                          ? currentThreadPermissionMode()
                                          : thread_.permissionMode;
          const std::string text = a.text;
          const auto images = a.images;

          clearLoadingState();
          setLoadingMessage("Starting chat...");
          setLoadingDetail("Creating a new thread, starting the lead agent, "
                           "and sending your first message.");
          setLoadingProgress(0.15f);

          runBackgroundTask([this, cwd, leadPersona, initialMode,
                             permissionMode, text, images]() {
            auto &h = firmius::core::Harness::instance();
            bool created = false;
            shared::ThreadMetadata metadata;
            std::string focusedAgentId;
            try {
              const auto threadId =
                  h.newThread({}, cwd, leadPersona, initialMode);
              created = !threadId.empty();
              if (created) {
                h.setCurrentThreadPermissionMode(permissionMode);
                for (const auto &candidate : h.listThreads()) {
                  if (candidate.threadId == threadId) {
                    metadata = candidate;
                    break;
                  }
                }
                if (!metadata.threadId.empty()) {
                  deferUiMutation([this, metadata]() {
                    auto first_agent_id =
                        harness_->listAgents(metadata.threadId)[0];
                    applyThreadOpened(metadata, first_agent_id, false);
                  });
                }
                h.send(text, images);
                if (focusedAgentId.empty()) {
                  const auto agents = h.listAgents(threadId);
                  if (!agents.empty()) {
                    focusedAgentId = agents.front();
                    (void)h.setFocusedAgent(focusedAgentId);
                  }
                }
              }
            } catch (...) {
              created = false;
            }

            if (!created || metadata.threadId.empty()) {
              deferUiMutation([this]() {
                clearLoadingState();
                NotificationManager::instance().notifyError(
                    "Thread", "Could not start chat.", false);
                history_.reset();
                TranscriptModel::instance().active_history.reset();
                setViewMode(ViewMode::Welcome);
              });
              return;
            }
            deferUiMutation([this, metadata, focusedAgentId]() {
              clearLoadingState();
              applyThreadOpened(metadata, focusedAgentId, true);
            });
          });
        } else if constexpr (std::is_same_v<T, UiTick>) {
          (void)a;
          requestRender(RefreshFlags::Welcome | RefreshFlags::LiveStatusRow |
                        RefreshFlags::InputBar);
        }
      },
      action);
}

void TuiState::runBackgroundTask(std::function<void()> action) {
  std::lock_guard<std::mutex> lock(background_ui_tasks_mutex_);
  background_ui_tasks_.emplace_back(
      [action = std::move(action)](std::stop_token stop_token) mutable {
        if (stop_token.stop_requested())
          return;
        action();
      });
}

void TuiState::setLoadingMessage(std::string message) {
  {
    std::lock_guard<std::mutex> lock(loading_message_mutex_);
    loading_message_ = std::move(message);
    loading_auto_clear_at_.reset();
  }
  if (screen_)
    postEvent(ftxui::Event::Custom);
}

void TuiState::clearLoadingMessage() { setLoadingMessage(""); }

void TuiState::setLoadingProgress(float progress) {
  {
    std::lock_guard<std::mutex> lock(loading_progress_mutex_);
    loading_progress_ = std::clamp(progress, 0.0f, 1.0f);
  }
  if (screen_)
    postEvent(ftxui::Event::Custom);
}

void TuiState::clearLoadingProgress() {
  {
    std::lock_guard<std::mutex> lock(loading_progress_mutex_);
    loading_progress_ = -1.0f;
  }
  if (screen_) {
    screen_->RequestAnimationFrame();
    postEvent(ftxui::Event::Custom);
  }
}

float TuiState::loadingProgress() const {
  std::lock_guard<std::mutex> lock(loading_progress_mutex_);
  return loading_progress_;
}

std::string TuiState::loadingMessage() const {
  std::lock_guard<std::mutex> lock(loading_message_mutex_);
  return loading_message_;
}

void TuiState::setLoadingDetail(std::string detail) {
  {
    std::lock_guard<std::mutex> lock(loading_detail_mutex_);
    loading_detail_ = std::move(detail);
  }
  if (screen_)
    postEvent(ftxui::Event::Custom);
}

void TuiState::clearLoadingDetail() { setLoadingDetail(""); }

std::string TuiState::loadingDetail() const {
  std::lock_guard<std::mutex> lock(loading_detail_mutex_);
  return loading_detail_;
}

void TuiState::clearLoadingState() {
  {
    std::lock_guard<std::mutex> lock(loading_message_mutex_);
    loading_message_.clear();
    loading_auto_clear_at_.reset();
  }
  {
    std::lock_guard<std::mutex> lock(loading_detail_mutex_);
    loading_detail_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(loading_progress_mutex_);
    loading_progress_ = -1.0f;
  }
  if (screen_)
    postEvent(ftxui::Event::Custom);
}

void TuiState::expireLoadingStateIfNeeded() {
  bool shouldClear = false;
  {
    std::lock_guard<std::mutex> lock(loading_message_mutex_);
    shouldClear = loading_auto_clear_at_.has_value() &&
                  std::chrono::steady_clock::now() >= *loading_auto_clear_at_;
  }
  if (shouldClear) {
    clearLoadingState();
  }
}

void TuiState::drainDeferredUiMutations() {
  std::vector<std::function<void()>> actions;
  {
    std::lock_guard<std::mutex> lock(deferred_ui_mutations_mutex_);
    actions.swap(deferred_ui_mutations_);
  }
  for (auto &action : actions) {
    if (action)
      action();
  }
}

SkinKind TuiState::currentSkinKind() const { return skin_config_.kind; }

void TuiState::applySkinConfig(const SkinConfig &config) {
  skin_config_ = config;
  ThemeManager::instance().setTheme(
      skin_config_.kind == SkinKind::Claudex ? "Claudex" : "Firmius");
  TUIStore::instance().skin_kind = skin_config_.kind;
  if (TUIStore::instance().input_model) {
    TUIStore::instance().input_model->compact_mode =
        (skin_config_.kind == SkinKind::Claudex);
  }
  if (TUIStore::instance().status_model) {
    TUIStore::instance().status_model->compact_skin_mode =
        (skin_config_.kind == SkinKind::Claudex);
  }
  TUIStore::instance().skin_config = skin_config_;
}

void TuiState::setSkinKind(SkinKind kind) {
  SkinConfig config = defaultSkinConfig(kind);
  config.kind = kind;
  applySkinConfig(config);

  if (status_model_)
    updateStatusModel();
  if (agent_strip_model_)
    updateAgentStripModel();
  if (context_lane_model_)
    updateContextLaneModel();

  TUIStore::instance().skin_config = skin_config_;
  persistUserPreferences();
  if (chat_component_)
    chat_component_->OnEvent(ftxui::Event::Special("ThemeChanged"));
  if (screen_)
    postEvent(ftxui::Event::Custom);
}

void TuiState::requestRefresh(RefreshFlags flags) {
  pending_refresh_flags_ |= static_cast<unsigned int>(flags);
}

void TuiState::requestRender(RefreshFlags flags) {
  pending_render_flags_ |= static_cast<unsigned int>(flags);
  ++render_generation_;
  const unsigned int bits = static_cast<unsigned int>(flags);
  for (std::size_t i = 0; i < render_surface_generation_.size(); ++i) {
    if (bits & (1u << i)) {
      render_surface_generation_[i] = render_generation_;
    }
  }
}

uint64_t TuiState::renderGeneration(RefreshFlags flag) const {
  const unsigned int bits = static_cast<unsigned int>(flag);
  if (bits == 0) {
    return render_generation_;
  }
  for (std::size_t i = 0; i < render_surface_generation_.size(); ++i) {
    if (bits & (1u << i)) {
      return render_surface_generation_[i];
    }
  }
  return render_generation_;
}

void TuiState::notifyChatTranscriptChanged() {
  trace("notifyChatTranscriptChanged focused=" + focused_agent_id_ +
        " view=" + std::to_string(static_cast<int>(getViewMode())));

  if (!chat_component_ && main_view_) {
    chat_component_ = main_view_->getChatComponent();
  }

  // Fallback focus recovery: if we are in chat mode but have no focus,
  // and chunks are arriving, try to latch onto the lead agent.
  if (focused_agent_id_.empty() && getViewMode() == ViewMode::Chat) {
    if (harness_) {
      if (const auto leadId = harness_->focusedAgentId(); !leadId.empty()) {
        focusAgent(leadId);
      }
    }
  }

  requestRefresh(RefreshFlags::ChatTranscript);
  requestRender(RefreshFlags::ChatTranscript);
  if (screen_) {
    postEvent(ftxui::Event::Custom);
    screen_->RequestAnimationFrame();
  }
}

void TuiState::applyPendingRefreshes() {
  const unsigned int flags = pending_refresh_flags_;
  if (flags == 0)
    return;
  pending_refresh_flags_ = 0;

  if (flags & static_cast<unsigned int>(RefreshFlags::TodoLane))
    updateTodoLaneModel();
  if (flags & static_cast<unsigned int>(RefreshFlags::Status))
    updateStatusModel();
  if (flags & static_cast<unsigned int>(RefreshFlags::AgentStrip))
    updateAgentStripModel();
  if (flags & static_cast<unsigned int>(RefreshFlags::PlanLane))
    updatePlanLaneModel();
  if (flags & static_cast<unsigned int>(RefreshFlags::ContextLane))
    updateContextLaneModel();
  if ((flags & static_cast<unsigned int>(RefreshFlags::ChatTranscript)) &&
      chat_component_) {
    std::cerr << "[DBG] applyPendingRefreshes dispatching TranscriptChanged to "
                 "chat_component_"
              << std::endl;
    chat_component_->OnEvent(ftxui::Event::Special("TranscriptChanged"));
  }
}

void TuiState::updateStatusModel() {
  if (!status_model_)
    return;

  bottom_hook_status_lines_.clear();
  if (!thread_.threadId.empty()) {
    auto &hookState = firmius::core::hooks::HookState::instance();
    hookState.bindThread(thread_.threadId);
    bottom_hook_status_lines_ = formatHookStatusLinesWithLua(thread_.threadId);
    while (bottom_hook_status_lines_.size() > 6) {
      bottom_hook_status_lines_.erase(bottom_hook_status_lines_.begin());
    }
  }

  if (focused_agent_id_.empty()) {
    status_model_->agent_name =
        shared::ConfigLoader::instance().getConfig().defaultLeadPersona;
    status_model_->model_name =
        shared::ConfigLoader::instance().getConfig().defaultModelId;
    status_model_->model_variant =
        shared::ConfigLoader::instance().getConfig().defaultModelVariant;
    status_model_->purpose =
        shared::ConfigLoader::instance().getConfig().defaultLeadPersona;
    status_model_->status_text = "idle";
    status_model_->permission_mode = thread_.permissionMode;
    status_model_->title = "Welcome";
    // On the welcome screen, the mode pill mirrors the user's current
    // pre-thread pick (cycled via Ctrl+Y, set via /mode <name>).
    status_model_->active_mode =
        TUIStore::instance().thread_metadata.initialMode;
    // Look up the glyph for that mode so the pill renders e.g. "🔬
    // vellum:pathology" instead of just the bare name.
    if (!status_model_->active_mode.empty()) {
      const std::string persona =
          TUIStore::instance().thread_metadata.leadPersona.empty()
              ? std::string("aster")
              : TUIStore::instance().thread_metadata.leadPersona;
      if (const auto *m =
              firmius::core::modes::ModeRegistry::instance().resolveForPersona(
                  status_model_->active_mode, persona)) {
        status_model_->active_mode_glyph = m->glyph;
      } else {
        status_model_->active_mode_glyph.clear();
      }
    } else {
      status_model_->active_mode_glyph.clear();
    }

    // Restoration: Default context window and quota for Welcome screen
    status_model_->context_used = 0;

    auto mprovider =
        firmius::provider::ProviderRegistry::instance().getProvider(
            shared::ConfigLoader::instance().getConfig().defaultProviderId);
    if (mprovider && harness_) {
      shared::ModelInfo mi = mprovider->getModelInfo(
          shared::ConfigLoader::instance().getConfig().defaultModelId);
      status_model_->context_max = mi.contextWindow;
      auto accounts = harness_->getAccounts(
          shared::ConfigLoader::instance().getConfig().defaultProviderId);
      const std::string providerId =
          shared::ConfigLoader::instance().getConfig().defaultProviderId;
      if (!accounts.empty()) {
        auto quotas = harness_->getCachedAllQuotas(providerId);
        auto it_q = quotas.find(accounts.front().identifier);
        if (it_q != quotas.end()) {
          status_model_->quota_usage = firmius::tui::quota::format(
              mprovider,
              shared::ConfigLoader::instance().getConfig().defaultModelId,
              it_q->second);
        }
        scheduleQuotaRefresh(providerId);
      }
    }
    status_model_->hook_status_lines = bottom_hook_status_lines_;
    status_model_->max_status_lines = 6;
    return;
  }

  auto agent =
      firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
  if (!agent)
    return;

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
      focused_agent_id_, &runtime_snapshot, [&](const std::string &process_id) {
        try {
          return agent->getEnvironment()
              ->getProcessManager()
              .inspectProcess(process_id)
              .running;
        } catch (...) {
          return false;
        }
      });

  status_model_->status_text =
      agentStatusToDisplayString(ctx.state.currentStatus);
  if ((agent->isRunning() || agent->isBooting()) &&
      (ctx.state.currentStatus == shared::AgentStatus::Idle ||
       ctx.state.currentStatus == shared::AgentStatus::Cancelled ||
       ctx.state.currentStatus == shared::AgentStatus::Error)) {
    const auto *stream = stream_state_.getStream(focused_agent_id_);
    status_model_->status_text =
        (stream && stream->provider_waiting) ? "provider_waiting" : "streaming";
  }

  status_model_->model_name = ctx.config.providerId + "/" + ctx.config.modelId;
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

  auto provider = firmius::provider::ProviderRegistry::instance().getProvider(
      ctx.config.providerId);
  if (provider && harness_) {
    auto info = provider->getModelInfo(ctx.config.modelId);
    status_model_->context_max = info.contextWindow;

    auto accounts = harness_->getAccounts(ctx.config.providerId);
    if (!accounts.empty()) {
      auto quotas = harness_->getCachedAllQuotas(ctx.config.providerId);
      auto it_q = quotas.find(accounts.front().identifier);
      if (it_q != quotas.end()) {
        status_model_->quota_usage = firmius::tui::quota::format(
            provider, ctx.config.modelId, it_q->second);
      }
      scheduleQuotaRefresh(ctx.config.providerId);
    }
  }

  status_model_->live_processes = process_counts.live;
  status_model_->background_processes = process_counts.background;
  status_model_->is_active = agent->isRunning();
  status_model_->permission_mode = thread_.permissionMode;
  // Mirror the focused agent's live mode into the status bar so a
  // mode_switch tool call (or a Ctrl+Y cycle) is visible immediately.
  status_model_->active_mode = ctx.state.activeMode;
  // Resolve the glyph alongside the name so the pill renders the visual
  // shorthand the mode files declare (📜 / 🌾 / 🧵 / 🔬 / 🦴 / ⚖️ / …).
  if (!status_model_->active_mode.empty()) {
    if (const auto *m =
            firmius::core::modes::ModeRegistry::instance().resolveForPersona(
                status_model_->active_mode, ctx.config.personaName)) {
      status_model_->active_mode_glyph = m->glyph;
    } else {
      status_model_->active_mode_glyph.clear();
    }
  } else {
    status_model_->active_mode_glyph.clear();
  }
  status_model_->hook_status_lines = bottom_hook_status_lines_;
  status_model_->max_status_lines = 6;
}

void TuiState::scheduleQuotaRefresh(const std::string &providerId) {
  if (providerId.empty() || !harness_) {
    return;
  }

  constexpr auto kMinRefreshInterval = std::chrono::minutes(2);
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(quota_refresh_mutex_);
    if (quota_refresh_inflight_.count(providerId) > 0) {
      return;
    }
    auto last = quota_refresh_last_started_.find(providerId);
    if (last != quota_refresh_last_started_.end() &&
        now - last->second < kMinRefreshInterval) {
      return;
    }
    quota_refresh_last_started_[providerId] = now;
    quota_refresh_inflight_.insert(providerId);
  }

  runBackgroundTask([this, providerId]() {
    try {
      if (harness_) {
        (void)harness_->getAllQuotas(providerId);
      }
    } catch (...) {
    }

    {
      std::lock_guard<std::mutex> lock(quota_refresh_mutex_);
      quota_refresh_inflight_.erase(providerId);
    }

    deferUiMutation([this]() {
      requestRefresh(RefreshFlags::Status);
      requestRefresh(RefreshFlags::ContextLane);
    });
  });
}

void TuiState::updatePlanLaneModel() {
  if (!plan_lane_model_)
    return;
  *plan_lane_model_ = active_plan_state_.model();
  if (!plan_lane_model_->chunks.empty())
    plan_lane_model_->visible = true;

  // Sync TUIStore model for MainView compatibility
  auto &store = TUIStore::instance();
  if (store.plan_lane_model && plan_lane_model_ != store.plan_lane_model) {
    *store.plan_lane_model = *plan_lane_model_;
  }
}

void TuiState::updateTodoLaneModel() {
  if (!todo_lane_model_ || focused_agent_id_.empty() ||
      thread_.threadId.empty())
    return;

  firmius::core::ThreadManager tm(
      (firmius::shared::PlatformPaths::firmiusHomeDir() / "threads").string());
  auto todoList = tm.getAgentTodo(thread_.threadId, focused_agent_id_);

  todo_lane_model_->rows.clear();
  if (!todoList.items.empty()) {
    for (const auto &item : todoList.items) {
      TodoLaneRow row;
      row.id = static_cast<int>(item.id);
      row.text = item.text;
      row.status = item.status;
      todo_lane_model_->rows.push_back(std::move(row));
    }
    auto agent =
        firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
    todo_lane_model_->owner_label =
        agent ? agent->getContext().identity.friendlyName : focused_agent_id_;
    todo_lane_model_->visible = true;
  } else {
    todo_lane_model_->visible = false;
  }

  // Sync TUIStore model for MainView compatibility
  auto &store = TUIStore::instance();
  if (store.todo_lane_model && todo_lane_model_ != store.todo_lane_model) {
    *store.todo_lane_model = *todo_lane_model_;
  }
}

void TuiState::updateContextLaneModel() {
  if (!context_lane_model_ || focused_agent_id_.empty())
    return;
  auto agent =
      firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
  if (!agent)
    return;
  context_lane_model_->visible = true;

  const auto &ctx = agent->getContext();
  const auto *liveMetrics = stream_state_.getLatestMetrics(focused_agent_id_);
  const auto &metrics = liveMetrics ? *liveMetrics : ctx.aggregateMetrics;

  auto provider = firmius::provider::ProviderRegistry::instance().getProvider(
      ctx.config.providerId);
  uint32_t contextWindow = 0;
  if (provider) {
    contextWindow = provider->getModelInfo(ctx.config.modelId).contextWindow;
    context_lane_model_->context_label =
        formatCompactCount(metrics.tokens.contextSize) + "/" +
        formatCompactCount(contextWindow);
  }

  if (contextWindow > 0) {
    context_lane_model_->context_ratio =
        static_cast<float>(metrics.tokens.contextSize) / contextWindow;
  }

  context_lane_model_->context_buckets.clear();
  context_lane_model_->bucket_labels.clear();
  const auto rankedBuckets = firmius::core::rankContextBuckets(metrics.context);
  for (const auto &bucket : rankedBuckets) {
    ContextBucket cb;
    auto shortenBucketLabel = [](const std::string &label) {
      if (label == "system_prompt")
        return std::string("system");
      return label;
    };
    cb.label = shortenBucketLabel(bucket.label);
    cb.tokens =
        bucket.actualTokens > 0 ? bucket.actualTokens : bucket.estimatedTokens;
    context_lane_model_->context_buckets.push_back(cb);
    if (cb.tokens > 0 && context_lane_model_->bucket_labels.size() < 4) {
      context_lane_model_->bucket_labels.push_back(
          cb.label + " " + formatCompactCount(cb.tokens));
    }
  }

  context_lane_model_->owner_label = ctx.identity.friendlyName;
  context_lane_model_->model_label =
      ctx.config.providerId + "/" + ctx.config.modelId;
  context_lane_model_->usage_label =
      formatCompactCount(metrics.context.sentTokens) + " sent / " +
      formatCompactCount(metrics.context.billedPromptTokens) + " billed";
  if (metrics.tokens.completion > 0) {
    context_lane_model_->usage_label +=
        " / " + formatCompactCount(metrics.tokens.completion) + " out";
  }
  if (metrics.estimatedCostUsd > 0.0) {
    std::ostringstream cost;
    cost << "$" << std::fixed << std::setprecision(4)
         << metrics.estimatedCostUsd;
    context_lane_model_->cost_label = cost.str();
  } else {
    context_lane_model_->cost_label.clear();
  }

  context_lane_model_->account_label.clear();
  context_lane_model_->quota_label.clear();
  if (provider && harness_) {
    auto accounts = harness_->getAccounts(ctx.config.providerId);
    if (!accounts.empty()) {
      context_lane_model_->account_label = accounts.front().identifier;
      auto quotas = harness_->getCachedAllQuotas(ctx.config.providerId);
      auto it_q = quotas.find(accounts.front().identifier);
      if (it_q != quotas.end()) {
        context_lane_model_->quota_label = firmius::tui::quota::format(
            provider, ctx.config.modelId, it_q->second);
      }
      scheduleQuotaRefresh(ctx.config.providerId);
    }
  }

  context_lane_model_->rolling_memory = {};
  context_lane_model_->memory_labels.clear();
  if (ctx.config.rollingMemory.enabled && history_) {
    firmius::core::ThreadManager tm(
        (firmius::shared::PlatformPaths::firmiusHomeDir() / "threads")
            .string());
    const auto rolling =
        tm.loadRollingMemoryState(history_->threadId, focused_agent_id_);
    auto &lane = context_lane_model_->rolling_memory;
    lane.enabled = true;
    lane.mode_label = ctx.config.rollingMemory.mode;
    lane.preset_label = ctx.config.rollingMemory.preset;
    if (ctx.config.rollingMemory.observer.enabled) {
      lane.model_label = ctx.config.rollingMemory.observer.providerId + "/" +
                         ctx.config.rollingMemory.observer.modelId;
    }
    lane.context_window_tokens = rolling.lastContextWindow > 0
                                     ? rolling.lastContextWindow
                                     : contextWindow;
    lane.context_occupancy_ratio =
        lane.context_window_tokens > 0
            ? static_cast<float>(metrics.tokens.contextSize) /
                  static_cast<float>(lane.context_window_tokens)
            : 0.0f;
    lane.buffer_threshold_ratio = ctx.config.rollingMemory.bufferOccupancyRatio;
    lane.target_threshold_ratio = ctx.config.rollingMemory.targetOccupancyRatio;
    lane.emergency_threshold_ratio =
        ctx.config.rollingMemory.emergencyOccupancyRatio;
    lane.buffer_threshold_tokens = rolling.lastBufferThresholdTokens;
    lane.target_threshold_tokens = rolling.lastTargetThresholdTokens;
    lane.emergency_threshold_tokens = rolling.lastEmergencyThresholdTokens;
    lane.retained_tail_tokens = rolling.lastRetainedTailTokens;
    lane.observation_in_flight = rolling.observationInFlight;
    lane.reflection_in_flight = rolling.reflectionInFlight;
    lane.active_observations = std::count_if(
        rolling.observationChunks.begin(), rolling.observationChunks.end(),
        [](const auto &chunk) { return chunk.active && !chunk.superseded; });
    lane.buffered_observations = std::count_if(
        rolling.observationChunks.begin(), rolling.observationChunks.end(),
        [](const auto &chunk) { return chunk.buffered && !chunk.superseded; });
    lane.active_reflections = std::count_if(
        rolling.reflectionChunks.begin(), rolling.reflectionChunks.end(),
        [](const auto &chunk) { return chunk.active && !chunk.superseded; });
    for (const auto &chunk : rolling.observationChunks) {
      lane.source_tokens += chunk.sourceTokens;
      lane.summary_tokens += chunk.summaryTokens;
    }
    for (const auto &chunk : rolling.reflectionChunks) {
      lane.source_tokens += chunk.sourceTokens;
      lane.summary_tokens += chunk.summaryTokens;
    }
    lane.saved_tokens = lane.source_tokens > lane.summary_tokens
                            ? (lane.source_tokens - lane.summary_tokens)
                            : 0;
    lane.canonical_anchor_count = rolling.anchors.size();
    lane.bridge_packet_count = rolling.bridges.size();
    lane.latest_bridge_id = rolling.lastBridgeId;
    if (!rolling.bridges.empty()) {
      const auto &bridge = rolling.bridges.back();
      lane.bridge_target = bridge.targetTaskSignature;
      lane.bridge_hint = bridge.executionHint;
    }
  }

  // Sync TUIStore model for MainView compatibility
  auto &store = TUIStore::instance();
  if (store.context_lane_model &&
      context_lane_model_ != store.context_lane_model) {
    *store.context_lane_model = *context_lane_model_;
  }
}

void TuiState::updateAgentStripModel() {
  if (!agent_strip_model_)
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

  auto all_ids = firmius::core::AgentRegistry::instance().listAll();
  agent_strip_model_->items.clear();
  for (const auto &id : all_ids) {
    auto agent = firmius::core::AgentRegistry::instance().getAgent(id);
    if (!agent)
      continue;
    const auto &ctx = agent->getContext();

    AgentStripItem item;
    item.id = id;
    item.parent_id = ctx.identity.parentId;
    item.title =
        ctx.identity.role.empty() ? ctx.identity.name : ctx.identity.role;
    item.is_busy = agent->isRunning();
    item.is_focused = (id == focused_agent_id_);
    item.tool_call_count = countHistoryToolCalls(ctx.history.get());

    // Hierarchy depth
    item.hierarchy_depth = 0;
    std::string cur_parent = ctx.identity.parentId;
    while (!cur_parent.empty()) {
      item.hierarchy_depth++;
      auto p = firmius::core::AgentRegistry::instance().getAgent(cur_parent);
      cur_parent = p ? p->getContext().identity.parentId : "";
    }

    agent_strip_model_->items.push_back(std::move(item));
  }
  ++agent_strip_model_->layout_generation;
}

void TuiState::onEvent(const shared::AppEvent &ev) {
  AppController::instance().dispatch(ev, harness_);
  applyPendingRefreshes();
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
}

void TuiState::postEvent(ftxui::Event event) {
  if (!screen_)
    return;
  if (event == ftxui::Event::Custom &&
      custom_event_pending_.exchange(true, std::memory_order_relaxed))
    return;
  if (event == ftxui::Event::Custom) {
    noteTuiCustomEventPosted();
    screen_->RequestAnimationFrame();
  }
  screen_->PostEvent(event);
}

ftxui::Component TuiState::root() {
  if (!root_component_) {
    if (main_view_ == nullptr) {
      initModels();
      main_view_ = MakeMainView();
    }
    chat_component_ = main_view_->getChatComponent();

    // Catch global hotkeys before they reach the main view or modals
    auto hotkey_handler =
        ftxui::CatchEvent(main_view_, [this](ftxui::Event event) {
          if (event.input() == "\x1b[I") {
            if (input_model_)
              input_model_->is_focused = true;
            return true;
          }
          if (event.input() == "\x1b[O") {
            if (input_model_)
              input_model_->is_focused = false;
            return true;
          }

          if (event == ftxui::Event::Custom) {
            noteTuiCustomEventDrained();
            custom_event_pending_ = false;
            drainEvents();
            return true;
          }
          if (event == ftxui::Event::Special("DeferredUiMutation")) {
            drainDeferredUiMutations();
            applyPendingRefreshes();
            return true;
          }
          if (event == ftxui::Event::Special("PopModal")) {
            popModalImmediate();
            return true;
          }
          if (event == ftxui::Event::CtrlC) {
            return handleCtrlC();
          }

          if (!modals_.empty()) {
            auto topModal = modals_.back();
            if (topModal && topModal->OnEvent(event))
              return true;
            return false;
          }

          if (main_view_->HandleGlobalHotkeys(event))
            return true;
          return false;
        });

    modal_container_ = ftxui::Container::Stacked({hotkey_handler});

    root_component_ = ftxui::Renderer(modal_container_, [this]() {
      expireLoadingStateIfNeeded();

      // Sync modals into the stacked container for event handling
      while (modal_container_->ChildCount() > modals_.size() + 1) {
        modal_container_->ChildAt(modal_container_->ChildCount() - 1)->Detach();
      }
      for (size_t i = 0; i < modals_.size(); ++i) {
        if (modal_container_->ChildCount() <= i + 1) {
          modal_container_->Add(modals_[i]);
        }
      }

      // Custom rendering to apply darkening overlays between layers
      ftxui::Element current = modal_container_->ChildAt(0)->Render();
      for (size_t i = 0; i < modals_.size(); ++i) {
        const auto &theme = ThemeManager::instance().getCurrentTheme();
        current = ftxui::dbox(
            {current, ftxui::filler() | ftxui::bgcolor(theme.base.bg),
             (modals_[i]->Render() | ftxui::clear_under) | ftxui::center});
      }

      auto notifications = NotificationManager::instance().render();
      ftxui::Elements layers = {current, notifications};
      const auto loading = loadingMessage();
      const auto loading_detail = loadingDetail();
      const float progress = loadingProgress();
      if (!loading.empty()) {
        const auto &theme = ThemeManager::instance().getCurrentTheme();
        const int filled_cells =
            progress >= 0.0f
                ? std::clamp(static_cast<int>(std::round(progress * 24.0f)), 0,
                             24)
                : 0;
        const std::string progress_bar =
            std::string(static_cast<std::size_t>(filled_cells), '=') +
            std::string(static_cast<std::size_t>(24 - filled_cells), '.');
        std::string progress_label;
        if (progress >= 0.0f) {
          progress_label =
              std::to_string(static_cast<int>(std::round(progress * 100.0f))) +
              "% loaded";
        }

        ftxui::Elements loading_lines = {
            ftxui::text(loading) | ftxui::bold | ftxui::color(theme.modals.fg) |
                ftxui::center,
        };
        if (progress >= 0.0f) {
          loading_lines.push_back(
              ftxui::text("[" + progress_bar + "] " + progress_label) |
              ftxui::color(theme.modals.highlight_fg) | ftxui::center);
        }
        if (!loading_detail.empty()) {
          loading_lines.push_back(ftxui::text(loading_detail) |
                                  ftxui::color(theme.base.separator) |
                                  ftxui::center);
        }
        loading_lines.push_back(ftxui::text("Working in the background...") |
                                ftxui::color(theme.base.dim) | ftxui::center);

        auto loading_panel =
            ftxui::vbox({ftxui::text(""), ftxui::vbox(std::move(loading_lines)),
                         ftxui::text("")}) |
            ftxui::bgcolor(theme.modals.bg) | ftxui::color(theme.modals.fg) |
            ftxui::borderRounded | ftxui::bgcolor(theme.modals.bg) |
            ftxui::clear_under;

        layers.push_back(std::move(loading_panel) | ftxui::center);
      }
      return ftxui::dbox(std::move(layers));
    });
  }
  return root_component_;
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

  // Rendering is event-driven. Individual animated widgets request their own
  // frames while they are actually animating; an unconditional 30 FPS global
  // ticker makes idle large transcripts re-render continuously and starves
  // keyboard input behind expensive FTXUI layout/syntax-highlight work.
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

void noteTuiRequestAnimationFrameFromAgentStripSpinner() {
  TuiState::instance().requestAnimationTick(std::chrono::milliseconds(250));
}
void noteTuiRequestAnimationFrameFromWelcomeScreen() {
  TuiState::instance().requestAnimationTick(std::chrono::milliseconds(33));
}
void noteTuiRequestAnimationFrameFromLiveStatusRow() {
  TuiState::instance().requestAnimationTick(std::chrono::milliseconds(33));
}

void TuiState::handleAppEvent(const shared::AppEvent &ev) { onEvent(ev); }

std::vector<std::string>
focusCycleCandidates(const std::string &focusedAgentId) {
  std::vector<std::string> candidates;
  auto all_ids = firmius::core::AgentRegistry::instance().listAll();
  if (all_ids.empty())
    return candidates;

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
    if (candidate &&
        candidate->getContext().identity.parentId == focusedAgentId) {
      has_children = true;
      break;
    }
  }
  if (!has_children && !focused_parent.empty()) {
    parent_focus = focused_parent;
  }

  for (const auto &id : all_ids) {
    auto candidate = firmius::core::AgentRegistry::instance().getAgent(id);
    if (candidate &&
        candidate->getContext().identity.parentId == parent_focus) {
      candidates.push_back(id);
    }
  }

  if (candidates.empty() && !focusedAgentId.empty()) {
    if (firmius::core::AgentRegistry::instance().getAgent(focusedAgentId)) {
      candidates.push_back(focusedAgentId);
    }
  }

  if (candidates.empty()) {
    for (const auto &id : all_ids) {
      if (firmius::core::AgentRegistry::instance().getAgent(id)) {
        candidates.push_back(id);
      }
    }
  }

  return candidates;
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

  quit_arm_thread_ = std::jthread([this, generation](std::stop_token st) {
    for (int i = 0; i < 40 && !st.stop_requested(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (quit_arm_generation_ != generation)
        return;
    }
    if (st.stop_requested() || quit_arm_generation_ != generation)
      return;
    quit_arm_deadline_.reset();
    if (screen_)
      postEvent(ftxui::Event::Custom);
  });
  return true;
}

bool TuiState::focusAgent(const std::string &agent_id) {
  if (!harness_ || agent_id.empty() || !harness_->setFocusedAgent(agent_id)) {
    return false;
  }
  focused_agent_id_ = agent_id;
  TUIStore::instance().focused_agent_id = agent_id;

  refreshFocusedHistory();

  // Re-infer activity immediately to update phrase bank
  if (const auto *stream = stream_state_.getStream(focused_agent_id_)) {
    (void)stream;
    auto agent =
        firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
    if (agent) {
      // Force phrase reset to pick from new agent's mode
      live_row_current_phrase_.clear();
    }
  }

  requestRefresh(RefreshFlags::AgentStrip);
  requestRefresh(RefreshFlags::Status);
  requestRefresh(RefreshFlags::PlanLane);
  requestRefresh(RefreshFlags::TodoLane);

  if (chat_component_) {
    chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
  }

  notifyChatTranscriptChanged();
  applyPendingRefreshes();
  if (screen_) {
    postEvent(ftxui::Event::Custom);
    screen_->RequestAnimationFrame();
  }
  return true;
}
void TuiState::triggerTranscriptUndoFromHotkey() {
  if (!harness_)
    return;
  try {
    auto result = harness_->undoTurnsWithRedo(1);
    if (result) {
      last_transcript_undo_action_ = result;
      last_transcript_redo_action_.reset();
      refreshFocusedHistory();
      notifyChatTranscriptChanged();
      NotificationManager::instance().notifyInfo(
          "Undo", "Last turn undone.", std::chrono::milliseconds(1000));
    }
  } catch (const std::exception &ex) {
    NotificationManager::instance().notifyError("Undo", ex.what(), false);
  }
}

void TuiState::triggerTranscriptRedoFromHotkey() {
  if (!harness_ || !last_transcript_undo_action_)
    return;
  try {
    auto result = harness_->redoTranscriptUndoAction(
        last_transcript_undo_action_->undoActionId);
    if (result) {
      last_transcript_redo_action_ = result;
      last_transcript_undo_action_.reset();
      refreshFocusedHistory();
      notifyChatTranscriptChanged();
      NotificationManager::instance().notifyInfo(
          "Redo", "Turn restored.", std::chrono::milliseconds(1000));
    }
  } catch (const std::exception &ex) {
    NotificationManager::instance().notifyError("Redo", ex.what(), false);
  }
}

void TuiState::triggerEditUndoFromHotkey() {
  if (!harness_)
    return;
  std::vector<shared::EditBatchSummary> batches;
  try {
    batches = harness_->listEditBatches(thread_.threadId);
  } catch (const std::exception &ex) {
    NotificationManager::instance().notifyError("Undo Edit", ex.what(), false);
    return;
  }
  if (batches.empty())
    return;

  // Find the last applied batch
  std::string target_id;
  for (auto it = batches.rbegin(); it != batches.rend(); ++it) {
    if (it->status == shared::EditBatchStatus::Applied) {
      target_id = it->editBatchId;
      break;
    }
  }

  if (target_id.empty())
    return;

  try {
    auto result = harness_->undoEditBatch(target_id);
    if (result) {
      last_edit_undo_action_ = result;
      last_edit_redo_action_.reset();
      NotificationManager::instance().notifyInfo(
          "Undo Edit", "File changes reversed.",
          std::chrono::milliseconds(1200));
    }
  } catch (const std::exception &ex) {
    NotificationManager::instance().notifyError("Undo Edit", ex.what(), false);
  }
}

void TuiState::triggerEditRedoFromHotkey() {
  if (!harness_ || !last_edit_undo_action_)
    return;
  try {
    auto result =
        harness_->redoEditUndoAction(last_edit_undo_action_->undoActionId);
    if (result) {
      last_edit_redo_action_ = result;
      last_edit_undo_action_.reset();
      NotificationManager::instance().notifyInfo(
          "Redo Edit", "File changes restored.",
          std::chrono::milliseconds(1200));
    }
  } catch (const std::exception &ex) {
    NotificationManager::instance().notifyError("Redo Edit", ex.what(), false);
  }
}

void TuiState::triggerTranscriptUndoToUserBoundaryFromHotkey() {
  if (!harness_)
    return;

  // Alt+Backspace UX contract: undo back to last user message boundary.
  // Compute an undo count based on the focused agent's persisted history.
  const std::string agentId = harness_->focusedAgentId();
  if (agentId.empty())
    return;
  std::shared_ptr<shared::AgentHistory> history;
  try {
    history = harness_->getAgentHistoryPtr(agentId);
  } catch (const std::exception &ex) {
    NotificationManager::instance().notifyError("Undo", ex.what(), false);
    return;
  }
  if (!history || history->turns.size() <= 2)
    return;
  int count = 1;
  for (std::size_t i = history->turns.size(); i-- > 2;) {
    bool isUser = false;
    for (const auto &msg : history->turns[i].messages) {
      if (msg.role == firmius::shared::Role::User) {
        isUser = true;
        break;
      }
    }
    if (isUser) {
      count = std::max(1, static_cast<int>(history->turns.size() - i));
      break;
    }
  }
  try {
    auto result = harness_->undoTurnsWithRedo(std::max(1, count));
    if (result) {
      last_transcript_undo_action_ = result;
      last_transcript_redo_action_.reset();
      refreshFocusedHistory();
      notifyChatTranscriptChanged();
      NotificationManager::instance().notifyInfo(
          "Undo", "Rewound to last user message.",
          std::chrono::milliseconds(1200));
    }
  } catch (const std::exception &ex) {
    NotificationManager::instance().notifyError("Undo", ex.what(), false);
  }
}

} // namespace firmius::tui
