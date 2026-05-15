#include "components/ChatMeasurementSignature.hpp"

#include <functional>

namespace firmius::tui {

namespace {

template <typename T> void HashCombine(std::size_t &seed, const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

} // namespace

std::size_t BuildFocusedChatLiveMeasurementSignature(
    const StreamStateManager &stream_state, const std::string &focused_agent_id,
    const std::string &thread_id,
    const std::unordered_set<std::string> &persisted_tool_call_ids) {
  std::size_t signature = 0;
  HashCombine(signature, focused_agent_id);
  HashCombine(signature, persisted_tool_call_ids.size());
  HashCombine(signature, thread_id);

  if (const auto *s = stream_state.getStream(focused_agent_id)) {
    HashCombine(signature, s->thinking.size());
    HashCombine(signature, s->text.size());
    HashCombine(signature, s->compaction_thinking.size());
    HashCombine(signature, s->compaction_text.size());
    HashCombine(signature, s->compaction_completion.size());
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

} // namespace firmius::tui
