#ifndef FIRMIUS_COMPONENTS_CHAT_HISTORY_BUILDER_HPP
#define FIRMIUS_COMPONENTS_CHAT_HISTORY_BUILDER_HPP

#include "components/ChatWindow.hpp"

#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace firmius::tui {

class RowComponent : public ftxui::ComponentBase {
public:
  RowComponent(ftxui::Component child, std::function<ftxui::Element()> render);

  ftxui::Element OnRender() override;
  bool Focusable() const override;
  bool OnEvent(ftxui::Event event) override;
  const ftxui::Box &box() const;

private:
  ftxui::Component child_;
  std::function<ftxui::Element()> render_;
  ftxui::Box box_;
};

class CopyableRowComponent : public ftxui::ComponentBase {
public:
  CopyableRowComponent(std::function<ftxui::Element(bool)> render,
                       std::shared_ptr<const std::string> copy_text);

  ftxui::Element OnRender() override;
  bool Focusable() const override;
  const ftxui::Box &box() const;
  const std::string &copyText() const;

private:
  std::function<ftxui::Element(bool)> render_;
  std::shared_ptr<const std::string> copy_text_;
  ftxui::Box box_;
};

struct HistoryRenderSignature {
  const firmius::shared::AgentHistory *history = nullptr;
  std::size_t turn_count = 0;
  std::size_t cheap_key = 0;
  bool show_internal_nudges = false;
  bool hide_errors = false;

  bool operator==(const HistoryRenderSignature &other) const;
  bool operator!=(const HistoryRenderSignature &other) const;
};

struct ChatHistoryCachedBlock {
  size_t turn_start = 0;
  size_t turn_end = 0;
  size_t rows_start = 0;
  size_t rows_count = 0;
  size_t copyable_start = 0;
  size_t copyable_count = 0;
};

struct ChatHistoryState {
  std::vector<ChatHistoryCachedBlock> block_cache;
  std::vector<std::size_t> turn_hashes;
  std::vector<ftxui::Component> rows;
  std::vector<std::shared_ptr<CopyableRowComponent>> copyable_rows;
  std::unordered_map<std::string, std::shared_ptr<firmius::shared::ToolCallView>>
      tool_views;
};

struct ChatHistoryBuildDependencies {
  ToolViewProvider tool_view_provider;
  ProcessStateGetter process_state_getter;
  SubagentStateGetter subagent_state_getter;
  AgentFocusHandler agent_focus_handler;
  HistoryGetter sub_history_getter;
  StreamGetter sub_stream_getter;
  LiveQuickSummaryProvider live_quick_summary_provider;
  std::function<bool()> show_turn_footers_getter;
  EditableMessageSelectedGetter editable_message_selected_getter;
};

HistoryRenderSignature BuildHistoryRenderSignature(
    const firmius::shared::AgentHistory *history, bool show_internal_nudges,
    bool hide_errors);

void RebuildChatHistoryIfNeeded(ChatHistoryState &state, bool &history_dirty,
                                HistoryRenderSignature &last_signature,
                                int current_width, int &last_rebuild_width,
                                const HistoryRenderSignature &signature,
                                const ChatHistoryBuildDependencies &deps);

} // namespace firmius::tui

#endif
