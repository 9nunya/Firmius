#include "controllers/TranscriptController.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "harness/Harness.hpp"
#include "components/ChatWindow.hpp"
#include "components/Markdown.hpp"
#include "components/ToolBlock.hpp"
#include "components/ErrorDisplay.hpp"
#include "AgentRegistry.hpp"
#include "utils/PlatformPaths.hpp"
#include <unordered_set>

namespace firmius::tui {

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
      
      auto expandedSnapshot = expandCompactionTurns(snapshotTurns, snapshots, expanded_ids);
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

} // namespace

TranscriptController& TranscriptController::instance() {
  static TranscriptController inst;
  return inst;
}

void TranscriptController::expandHistoryForTranscriptIfNeeded() {
  auto& state = TuiState::instance();
  auto& store = TUIStore::instance();
  auto& model = TranscriptModel::instance();
  
  if (!state.harness_ || store.focused_agent_id.empty() || store.thread_id.empty()) {
    return;
  }

  auto base_history = state.harness_->getAgentHistoryPtr(store.focused_agent_id);
  if (!base_history) {
    state.history_.reset();
    model.active_history.reset();
    return;
  }

  bool hasCompaction = false;
  for (const auto& turn : base_history->turns) {
    if (compactionIdFromTurnId(turn.turnId).has_value()) {
      hasCompaction = true;
      break;
    }
  }

  if (!hasCompaction) {
    state.history_ = base_history;
  } else {
    firmius::core::ThreadManager tm(
        (firmius::shared::PlatformPaths::firmiusHomeDir() / "threads").string());
    const auto snapshotList = tm.loadCompactionSnapshots(store.thread_id, store.focused_agent_id);
    std::unordered_map<std::string, firmius::core::CompactionSnapshot> snapshots;
    for (const auto& snapshot : snapshotList) {
      if (!snapshot.compactionId.empty()) snapshots[snapshot.compactionId] = snapshot;
    }
    
    auto expanded = std::make_shared<firmius::shared::AgentHistory>(*base_history);
    std::unordered_set<std::string> expanded_ids;
    expanded->turns = expandCompactionTurns(base_history->turns, snapshots, expanded_ids);
    state.history_ = expanded;
  }
  model.active_history = state.history_;
  state.agent_history_cache_[store.focused_agent_id] = state.history_;
}

void TranscriptController::rebuildEditableUserMessages() {
  auto& state = TuiState::instance();
  auto& model = TranscriptModel::instance();

  model.editable_user_messages.clear();
  model.selected_editable_message_index = -1;
  if (!state.history_) return;
  
  for (const auto& turn : state.history_->turns) {
    for (const auto& message : turn.messages) {
      if (message.role != shared::Role::User) continue;
      EditableUserMessage item;
      item.timestamp = message.timestamp;
      for (const auto& content : message.content) {
        if (const auto* text = std::get_if<shared::TextContent>(&content)) {
          if (!item.text.empty()) item.text += "\n";
          item.text += text->text;
        } else if (const auto* image = std::get_if<shared::ImageContent>(&content)) {
          item.images.push_back(*image);
        }
      }
      model.editable_user_messages.push_back(std::move(item));
    }
  }
  if (!model.editable_user_messages.empty()) {
    model.selected_editable_message_index = static_cast<int>(model.editable_user_messages.size()) - 1;
  }
}

void TranscriptController::toggleEditMode() {
  auto& model = TranscriptModel::instance();
  if (!model.edit_mode_active) {
    rebuildEditableUserMessages();
    model.edit_mode_active = !model.editable_user_messages.empty();
  } else {
    model.edit_mode_active = false;
  }
}

void TranscriptController::selectEditableMessageByTimestamp(uint64_t timestamp) {
  auto& model = TranscriptModel::instance();
  for (int i = 0; i < static_cast<int>(model.editable_user_messages.size()); ++i) {
    if (model.editable_user_messages[i].timestamp == timestamp) {
      model.selected_editable_message_index = i;
      model.edit_mode_active = true;
      return;
    }
  }
}

void TranscriptController::commitSelectedEditableMessageToInput() {
  auto& model = TranscriptModel::instance();
  auto& store = TUIStore::instance();
  if (model.selected_editable_message_index < 0 || model.selected_editable_message_index >= static_cast<int>(model.editable_user_messages.size())) {
    return;
  }
  
  const auto selected = model.editable_user_messages[model.selected_editable_message_index];
  model.pending_edit_message = selected;
  
  if (store.input_model && store.input_model->buffer && store.input_model->cursor) {
    *store.input_model->buffer = selected.text;
    *store.input_model->cursor = static_cast<int>(store.input_model->buffer->size());
    store.input_model->pasted_blocks.clear();
    
    for (size_t i = 0; i < selected.images.size(); ++i) {
      const auto &image = selected.images[i];
      const std::string placeholder = "[Image " + std::to_string(i + 1) + "]";
      const size_t start_pos = store.input_model->buffer->size();
      store.input_model->buffer->append(placeholder);
      PastedBlock block;
      block.type = "image";
      block.id = "edit-image-" + std::to_string(i);
      const auto comma = image.url.find(",");
      block.content = image.url.rfind("data:", 0) == 0 && comma != std::string::npos ? image.url.substr(comma + 1) : image.url;
      block.mime_type = image.mediaType;
      block.start_pos = start_pos;
      block.end_pos = store.input_model->buffer->size();
      store.input_model->pasted_blocks.push_back(std::move(block));
    }
    *store.input_model->cursor = static_cast<int>(store.input_model->buffer->size());
  }
  model.edit_mode_active = false;
}

void TranscriptController::selectPreviousEditableMessage() {
  auto& model = TranscriptModel::instance();
  if (model.editable_user_messages.empty()) return;
  model.selected_editable_message_index = std::max(0, model.selected_editable_message_index - 1);
}

void TranscriptController::selectNextEditableMessage() {
  auto& model = TranscriptModel::instance();
  if (model.editable_user_messages.empty()) return;
  model.selected_editable_message_index = std::min(
      static_cast<int>(model.editable_user_messages.size()) - 1,
      model.selected_editable_message_index + 1);
}

std::vector<ftxui::Element> TranscriptController::generateLiveRows() {
  auto& state = TuiState::instance();
  const std::string agent_id = state.focused_agent_id_;
  const std::string thread_id = state.thread_.threadId;
  const uint64_t epoch = state.stream_state_.getLiveRenderEpoch();
  if (epoch == last_live_epoch_ && agent_id == last_live_agent_id_ &&
      thread_id == last_live_thread_id_) {
    // Keep the Components alive as long as the cached Elements are reused.
    live_components_ = cached_live_components_;
    return cached_live_rows_;
  }

  // Drop the previous render's tool Components. Their rendered Elements went
  // out of scope at the end of the previous Draw() pass, so it is now safe to
  // destroy the Components that backed them.
  live_components_.clear();

  std::vector<ftxui::Element> live_rows;
  
  const auto *s = state.stream_state_.getStream(agent_id);
  const auto &theme = ThemeManager::instance().getCurrentTheme();

  auto decorateMsg = [](const ftxui::Element &content) {
    return firmius::tui::IndentAgentRow(content);
  };

  auto full_width_separator = [](const std::string &label) {
    return ftxui::hbox({
               ftxui::filler() | ftxui::xflex,
               ftxui::text(" " + label + " ") | ftxui::dim,
               ftxui::filler() | ftxui::xflex,
           }) | ftxui::xflex;
  };

  if (s) {
    bool has_compaction_output = s->compaction_active && (!s->compaction_thinking.empty() || !s->compaction_text.empty());
    if (has_compaction_output) {
      live_rows.push_back(full_width_separator("Compaction"));
      if (!s->compaction_thinking.empty()) {
        live_rows.push_back(decorateMsg(RenderMarkdown(ClampTranscriptTextForDisplay(s->compaction_thinking), true)));
      }
      if (!s->compaction_text.empty()) {
        live_rows.push_back(decorateMsg(RenderMarkdown(ClampTranscriptTextForDisplay(s->compaction_text))));
      }
      live_rows.push_back(full_width_separator("Compaction Complete"));
    }
  }

  const auto &timeline = state.stream_state_.getTimeline();
  const auto &tool_calls = state.stream_state_.getToolCalls();
  const auto &persisted_tool_call_ids =
      state.agent_persisted_tool_call_ids_cache_[agent_id];

  for (const auto &entry : timeline) {
    if (entry.agentId != agent_id) continue;

    if (entry.kind == TimelineEntry::Kind::Thinking || entry.kind == TimelineEntry::Kind::Text) {
      const auto trimmed = ClampTranscriptTextForDisplay(entry.message);
      if (trimmed.empty()) continue;
      live_rows.push_back(decorateMsg(RenderMarkdown(trimmed, entry.kind == TimelineEntry::Kind::Thinking)));
      continue;
    }

    if (entry.kind == TimelineEntry::Kind::Error) {
      live_rows.push_back(IndentAgentRow(RenderErrorDisplay(theme, shared::ErrorContent{"Error", "Agent reported an error", entry.message})));
      continue;
    }

    if (entry.kind == TimelineEntry::Kind::ToolCall) {
      auto it_tool = tool_calls.find(entry.id);
      if (it_tool == tool_calls.end() || !it_tool->second) continue;
      if (persisted_tool_call_ids.count(entry.id) > 0) continue;

      auto sub_history_getter = [&](const std::string &agentId) -> const firmius::shared::AgentHistory * {
        auto it = state.agent_history_cache_.find(agentId);
        return it != state.agent_history_cache_.end() ? it->second.get() : nullptr;
      };
      auto sub_stream_getter = [&](const std::string &agentId) -> const firmius::tui::StreamState * {
        return state.stream_state_.getStream(agentId);
      };
      auto process_state_getter = [&](const std::string &toolCallId) -> const firmius::tui::NormalizedProcessState * {
        return state.stream_state_.getProcessStateForToolCall(toolCallId);
      };
      auto subagent_state_getter = [&](const std::string &toolCallId) -> const firmius::tui::NormalizedSubagentState * {
        return state.stream_state_.getSubagentStateForToolCall(toolCallId);
      };

      // Keep the Component alive for the rest of the render cycle. Its
      // rendered Element captures `&box_` via `ftxui::reflect(box_)`; if the
      // Component dies before ftxui's layout pass writes back through that
      // pointer, we get a heap-use-after-free in `Reflect::SetBox` (caught by
      // ASan, see TranscriptController.hpp).
      auto tool_component = ToolBlock(it_tool->second, sub_history_getter,
                                      sub_stream_getter, process_state_getter,
                                      subagent_state_getter);
      auto tool_row = tool_component->Render();
      live_components_.push_back(std::move(tool_component));
      live_rows.push_back(decorateMsg(tool_row));
    }
  }

  // Render Queued Messages
  const auto &queued = state.stream_state_.getQueuedMessages();
  for (const auto &item : queued) {
    if (item.thread_id != thread_id || item.agent_id != agent_id) continue;
    
    ftxui::Elements body;
    std::string prefix = "user";
    body.push_back(ftxui::hbox({
        ftxui::text(prefix) | ftxui::bold | ftxui::dim,
        ftxui::text(" ") | ftxui::dim,
        RenderMarkdown(item.text) | ftxui::xflex
    }) | ftxui::xflex);
    
    if (item.image_count > 0) {
      ftxui::Elements tags;
      for (int i = 0; i < item.image_count; ++i) {
        tags.push_back(ftxui::text(" IMAGE " + std::to_string(i + 1) + " ") 
            | ftxui::bold | ftxui::color(theme.base.bg) | ftxui::bgcolor(theme.base.highlight) | ftxui::dim);
      }
      body.push_back(ftxui::hbox(std::move(tags)) | ftxui::xflex);
    }
    
    live_rows.push_back(decorateMsg(ftxui::vbox(std::move(body)) | ftxui::dim));
  }

  last_live_epoch_ = epoch;
  last_live_agent_id_ = agent_id;
  last_live_thread_id_ = thread_id;
  cached_live_rows_ = live_rows;
  cached_live_components_ = live_components_;

  return live_rows;
}

} // namespace firmius::tui
