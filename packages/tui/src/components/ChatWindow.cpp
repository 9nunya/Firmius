#include "components/ChatWindow.hpp"
#include "components/ChatHistoryBuilder.hpp"
#include "components/ChatCopySelection.hpp"
#include "components/ChatViewportModel.hpp"
#include "Context.hpp"
#include "components/ScrollableBox.hpp"
#include "components/DiffRenderer.hpp"
#include "components/Markdown.hpp"
#include "components/SyntaxHighlighter.hpp"
#include "NotificationManager.hpp"
#include "utils/Clipboard.hpp"

#include <algorithm>
#include <cmath>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <limits>
#include <memory>
#include <vector>

namespace {
constexpr int kChatTailPaddingLines = 3;

template <typename T>
void HashCombine(std::size_t &seed, const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

using CachedBlock = firmius::tui::ChatHistoryCachedBlock;
using CopyableRowComponent = firmius::tui::CopyableRowComponent;
using HistoryRenderSignature = firmius::tui::HistoryRenderSignature;
using RowComponent = firmius::tui::RowComponent;

class ChatWindowComponent : public ftxui::ComponentBase {
public:
  explicit ChatWindowComponent(
      std::function<const firmius::shared::AgentHistory *()> history_getter,
      std::function<std::vector<ftxui::Element>()> live_rows_provider,
      firmius::tui::ToolViewProvider tool_view_provider,
      firmius::tui::ProcessStateGetter process_state_getter,
      firmius::tui::SubagentStateGetter subagent_state_getter,
      firmius::tui::AgentFocusHandler agent_focus_handler,
      firmius::tui::HistoryGetter sub_history_getter,
      firmius::tui::StreamGetter sub_stream_getter,
      firmius::tui::LiveQuickSummaryProvider live_quick_summary_provider,
      std::function<std::size_t()> live_measurement_signature_getter,
      std::function<bool()> show_internal_nudges_getter,
      std::function<bool()> hide_errors_getter,
      std::function<bool()> show_turn_footers_getter,
      firmius::tui::EditableModeEnabledGetter editable_mode_enabled_getter,
      firmius::tui::EditableMessageSelectedGetter editable_message_selected_getter,
      firmius::tui::EditableMessageClickHandler editable_message_click_handler)
      : history_getter_(std::move(history_getter)),
        live_rows_provider_(std::move(live_rows_provider)),
        tool_view_provider_(std::move(tool_view_provider)),
        process_state_getter_(std::move(process_state_getter)),
        subagent_state_getter_(std::move(subagent_state_getter)),
        agent_focus_handler_(std::move(agent_focus_handler)),
        sub_history_getter_(std::move(sub_history_getter)),
        sub_stream_getter_(std::move(sub_stream_getter)),
        live_quick_summary_provider_(std::move(live_quick_summary_provider)),
        live_measurement_signature_getter_(
            std::move(live_measurement_signature_getter)),
        show_internal_nudges_getter_(
            std::move(show_internal_nudges_getter)),
        hide_errors_getter_(std::move(hide_errors_getter)),
        show_turn_footers_getter_(std::move(show_turn_footers_getter)),
        editable_mode_enabled_getter_(std::move(editable_mode_enabled_getter)),
        editable_message_selected_getter_(std::move(editable_message_selected_getter)),
        editable_message_click_handler_(std::move(editable_message_click_handler)) {

    history_inner_ = ftxui::Container::Vertical({});
    history_container_ = ftxui::Renderer(history_inner_, [this] { return RenderHistoryWindow(); });

    tail_spacer_ =
        ftxui::Make<RowComponent>(nullptr, [this] {
          const bool show_turn_footers =
              show_turn_footers_getter_ ? show_turn_footers_getter_() : true;
          const int padding_lines = show_turn_footers ? kChatTailPaddingLines : 0;
          ftxui::Elements padding_rows;
          padding_rows.reserve(padding_lines > 0 ? padding_lines : 1);
          for (int i = 0; i < padding_lines; ++i) {
            padding_rows.push_back(ftxui::text(" "));
          }
          if (padding_rows.empty()) {
            padding_rows.push_back(ftxui::text(""));
          }
          return ftxui::vbox(std::move(padding_rows)) | ftxui::xflex;
        });
    auto live_rows_cmp = ftxui::Renderer([this] {
      if (!live_rows_provider_) {
        return ftxui::vbox(ftxui::Elements{});
      }
      // Live rows can be expensive (markdown render / tool row composition).
      // Avoid recomputing them on unrelated redraws (e.g. animation ticks for
      // other widgets) by caching against the same signature we use for scroll
      // measurement invalidation.
      const std::size_t sig = live_measurement_signature_getter_
                                  ? live_measurement_signature_getter_()
                                  : 0u;
      if (has_cached_live_rows_ && sig == last_live_rows_signature_) {
        return ftxui::vbox(cached_live_rows_);
      }

      // Drop any null Elements before they reach VBox::SetBox / Flex::SetBox.
      // A single bad row otherwise turns the whole frame into a SIGSEGV.
      auto rows = live_rows_provider_();
      rows.erase(std::remove_if(rows.begin(), rows.end(),
                                [](const ftxui::Element &e) { return !e; }),
                 rows.end());
      cached_live_rows_ = rows;
      last_live_rows_signature_ = sig;
      has_cached_live_rows_ = true;
      return ftxui::vbox(std::move(rows));
    });

    container_ = ftxui::Container::Vertical(
        {history_container_, live_rows_cmp, tail_spacer_});
    container_ = ftxui::Renderer(container_, [this, live_rows_cmp] {
      ftxui::Elements parts;
      parts.reserve(3);
      auto push = [&](ftxui::Element e) {
        if (e) parts.push_back(std::move(e));
      };
      push(history_container_->Render());
      push(live_rows_cmp->Render());
      push(tail_spacer_->Render());
      return ftxui::vbox(std::move(parts));
    });

    scrollable_ = firmius::tui::ScrollableBox(
        container_,
        {.startAtBottom = true,
         .overlayScrollbar = true,
         .measurement_signature_getter =
             [this]() {
               std::size_t seed = last_history_signature_.turn_count;
               HashCombine(seed, last_history_signature_.cheap_key);
               if (live_measurement_signature_getter_) {
                 HashCombine(seed, live_measurement_signature_getter_());
               }
               return seed;
             }});
    Add(scrollable_);

    if (scrollable_) {
        scrollable_->options().custom_size_getter = [this](int width) {
            return GetTotalHistoryHeight(width);
        };
    }
  }

  ftxui::Element OnRender() override {
    EnsureHistoryRows();
    if (pending_bottom_restore_ && scrollable_) {
      if (scrollable_->ViewportHeight() > 1 && scrollable_->ContentWidth() > 1) {
        scrollable_->TakeFocus();
        scrollable_->RequestScrollToBottom();
        pending_bottom_restore_ = false;
      }
    }
    return scrollable_ ? scrollable_->Render() : ftxui::text("");
  }

  bool Focusable() const override { return false; }

  bool OnEvent(ftxui::Event event) override {
    if (event == ftxui::Event::Special("TranscriptChanged")) {
      const bool should_follow_bottom =
          !scrollable_ || scrollable_->IsAtBottom() || pending_bottom_restore_;
      MarkHistoryDirty(false);
      EnsureHistoryRows();
      if (scrollable_) {
        scrollable_->TakeFocus();
        if (should_follow_bottom) {
          scrollable_->RequestScrollToBottom();
          pending_bottom_restore_ = true;
        } else {
          scrollable_->InvalidateLayout();
        }
      }
      return true;
    }

    if (event == ftxui::Event::Special("ThreadChanged") ||
        event == ftxui::Event::Special("ThemeChanged")) {
      if (event == ftxui::Event::Special("ThemeChanged")) {
        firmius::tui::ClearToolPresentationDiffCache();
        firmius::tui::ClearMarkdownCache();
        firmius::tui::SyntaxHighlighter::instance().clearRenderCache();
      }
      MarkHistoryDirty(true);
      EnsureHistoryRows();
      if (scrollable_) {
        scrollable_->RequestScrollToBottom();
        scrollable_->TakeFocus();
        pending_bottom_restore_ = true;
      }
      return true;
    }

    if (event == ftxui::Event::Custom) {
      if (pending_bottom_restore_ && scrollable_) {
        EnsureHistoryRows();
        if (scrollable_->ViewportHeight() <= 1 || scrollable_->ContentWidth() <= 1) {
          return true;
        }
        scrollable_->TakeFocus();
        scrollable_->RequestScrollToBottom();
        pending_bottom_restore_ = false;
        return true;
      }
      pending_bottom_restore_ = false;
    }

    const bool copy_handler_consumed = HandleCopySelection(event);
    EnsureHistoryRows();
    // Do not invalidate transcript layout for ordinary keypresses. The input
    // bar shares the event loop with chat; forcing a full chat re-measure on
    // every typed character makes large threads feel seconds behind.
    const bool handled = scrollable_ ? scrollable_->OnEvent(event) : false;
    FinalizePendingCopy();
    if (copy_drag_candidate_ && event.is_mouse() &&
        (event.mouse().button == ftxui::Mouse::WheelUp ||
         event.mouse().button == ftxui::Mouse::WheelDown)) {
      if (auto *screen = ftxui::ScreenInteractive::Active()) {
        auto resumed = event.mouse();
        resumed.button = ftxui::Mouse::Left;
        resumed.motion = ftxui::Mouse::Moved;
        resumed.x = last_drag_x_;
        resumed.y = last_drag_y_;
        screen->PostEvent(ftxui::Event::Mouse("", resumed));
      }
    }
    return copy_handler_consumed || handled;
  }

private:
  bool HandleCopySelection(ftxui::Event event) {
    if (!event.is_mouse()) {
      return false;
    }

    EnsureHistoryRows();
    const auto mouse = event.mouse();
    if (mouse.button != ftxui::Mouse::Left) {
      return false;
    }

    const int hovered =
        firmius::tui::FindCopyableRowAt(copyable_rows_, mouse.x, mouse.y);
    if (mouse.motion == ftxui::Mouse::Pressed) {
      copy_drag_candidate_ = (hovered >= 0);
      copy_drag_started_ = false;
      press_x_ = mouse.x;
      press_y_ = mouse.y;
      last_drag_x_ = mouse.x;
      last_drag_y_ = mouse.y;
      return false;
    }

    if (!copy_drag_candidate_) {
      return false;
    }

    if (mouse.motion == ftxui::Mouse::Moved) {
      const int dx = std::abs(mouse.x - press_x_);
      const int dy = std::abs(mouse.y - press_y_);
      copy_drag_started_ = copy_drag_started_ || dx > 0 || dy > 0;
      last_drag_x_ = mouse.x;
      last_drag_y_ = mouse.y;
      return false;
    }

    if (mouse.motion == ftxui::Mouse::Released) {
      last_drag_x_ = mouse.x;
      last_drag_y_ = mouse.y;
      if (copy_drag_started_) {
        pending_copy_release_ = true;
        pending_release_x_ = mouse.x;
        pending_release_y_ = mouse.y;
      }
      copy_drag_candidate_ = false;
      copy_drag_started_ = false;
      return false;
    }

    return false;
  }

  void FinalizePendingCopy() {
    if (!pending_copy_release_) {
      return;
    }
    pending_copy_release_ = false;
    auto *screen = ftxui::ScreenInteractive::Active();
    if (!screen) {
      return;
    }

    std::string copied = screen->GetSelection();
    if (copied.empty()) {
      copied = firmius::tui::ExtractCopyableTextFromScreen(
          copyable_rows_, *screen, press_x_, press_y_, pending_release_x_,
          pending_release_y_);
    }

    if (!copied.empty() && Clipboard::setText(copied)) {
      firmius::tui::NotificationManager::instance().notifySuccess(
          "Copied!", "Transcript selection copied.",
          std::chrono::milliseconds(1200));
      firmius::tui::ClearFrameworkSelection(*screen, pending_release_x_,
                                            pending_release_y_);
      return;
    }

    firmius::tui::NotificationManager::instance().notifyWarning(
        "Copy Failed", "Selection could not be copied.",
        std::chrono::milliseconds(1500));
  }

  void AddRow(ftxui::Component row) {
    rows_.push_back(std::move(row));
  }

  void AddCopyableRow(std::function<ftxui::Element(bool)> render,
                      std::shared_ptr<const std::string> copy_text) {
    auto row = ftxui::Make<CopyableRowComponent>(std::move(render),
                                                 std::move(copy_text));
    copyable_rows_.push_back(row);
    rows_.push_back(row);
  }

  static constexpr int kDefaultEstimatedRowHeight = 4;
  static constexpr int kVirtualizationOverscanLines = 12;

  int GetTotalHistoryHeight(int width) {
    (void)width;
    if (row_height_cache_.empty() && !rows_.empty()) {
      row_height_cache_.assign(rows_.size(), kDefaultEstimatedRowHeight);
    }
    return firmius::tui::GetTotalChatHistoryHeight(row_height_cache_);
  }

  ftxui::Element RenderHistoryWindow() {
    if (rows_.empty()) {
      return ftxui::text("");
    }
    firmius::tui::EnsureChatRowHeightCache(row_height_cache_, rows_.size(),
                                           kDefaultEstimatedRowHeight);
    firmius::tui::RefreshCachedVisibleHeights(
        rows_, row_height_cache_, last_visible_start_, last_visible_end_,
        kDefaultEstimatedRowHeight);

    auto window = scrollable_
                      ? firmius::tui::ComputeChatViewportWindow(
                            rows_.size(), row_height_cache_,
                            scrollable_->ViewportHeight(),
                            scrollable_->ScrollOffset(), scrollable_->IsAtBottom(),
                            last_visible_start_, last_visible_end_,
                            kVirtualizationOverscanLines)
                      : firmius::tui::ChatViewportWindow{.start = 0,
                                                         .end = rows_.size()};
    last_visible_start_ = window.start;
    last_visible_end_ = window.end;

    // Sync history_inner_ children with the visible window to keep event processing O(1)
    if (window.start != last_attached_start_ || window.end != last_attached_end_ ||
        history_inner_->ChildCount() == 0) {
        history_inner_->DetachAllChildren();
        for (size_t i = window.start; i < window.end; ++i) {
            if (i < rows_.size()) {
                history_inner_->Add(rows_[i]);
            }
        }
        last_attached_start_ = window.start;
        last_attached_end_ = window.end;
    }

    ftxui::Elements elements;
    if (window.top_padding > 0) {
      elements.push_back(ftxui::text("") |
                         ftxui::size(ftxui::HEIGHT, ftxui::EQUAL,
                                     window.top_padding));
    }
    
    for (size_t i = 0; i < history_inner_->ChildCount(); ++i) {
        auto e = history_inner_->ChildAt(i)->Render();
        if (e) {
          elements.push_back(std::move(e));
        }
    }

    if (window.bottom_padding > 0) {
      elements.push_back(ftxui::text("") |
                         ftxui::size(ftxui::HEIGHT, ftxui::EQUAL,
                                     window.bottom_padding));
    }
    return ftxui::vbox(std::move(elements));
  }

  void RebuildIfNeeded() {
    const auto signature = currentHistorySignature();
    const int current_width = scrollable_ ? scrollable_->ContentWidth() : -1;
    if (current_width != last_rebuild_width_) {
      MarkHistoryDirty(true);
      last_rebuild_width_ = current_width;
    } else if (signature != last_history_signature_ &&
               signature.cheap_key != last_history_signature_.cheap_key) {
      MarkHistoryDirty(true);
    }

    if (!history_dirty_ && signature == last_history_signature_) {
      return;
    }
    if (scrollable_) {
      scrollable_->InvalidateLayout();
    }

    firmius::tui::ChatHistoryState state;
    state.block_cache.swap(block_cache_);
    state.turn_hashes.swap(turn_hashes_);
    state.rows.swap(rows_);
    state.copyable_rows.swap(copyable_rows_);
    state.tool_views.swap(tool_views_);

    firmius::tui::RebuildChatHistoryIfNeeded(
        state, history_dirty_, last_history_signature_, current_width,
        last_rebuild_width_, signature,
        {.tool_view_provider = tool_view_provider_,
         .process_state_getter = process_state_getter_,
         .subagent_state_getter = subagent_state_getter_,
         .agent_focus_handler = agent_focus_handler_,
         .sub_history_getter = sub_history_getter_,
         .sub_stream_getter = sub_stream_getter_,
         .live_quick_summary_provider = live_quick_summary_provider_,
         .show_turn_footers_getter = show_turn_footers_getter_,
         .editable_message_selected_getter = editable_message_selected_getter_});

    block_cache_.swap(state.block_cache);
    turn_hashes_.swap(state.turn_hashes);
    rows_.swap(state.rows);
    copyable_rows_.swap(state.copyable_rows);
    tool_views_.swap(state.tool_views);

    if (row_height_cache_.size() < rows_.size()) {
      row_height_cache_.resize(rows_.size(), kDefaultEstimatedRowHeight);
    } else if (row_height_cache_.size() > rows_.size()) {
      row_height_cache_.resize(rows_.size());
    }
  }

  HistoryRenderSignature currentHistorySignature() const {
    auto *history = history_getter_ ? history_getter_() : nullptr;
    const bool showInternalNudges =
        show_internal_nudges_getter_ ? show_internal_nudges_getter_() : false;
    const bool hideErrors =
        hide_errors_getter_ ? hide_errors_getter_() : false;
    return firmius::tui::BuildHistoryRenderSignature(history, showInternalNudges,
                                                     hideErrors);
  }

  void MarkHistoryDirty(bool reset_full = false) {
    history_dirty_ = true;
    if (reset_full) {
      last_history_signature_ = {};
      block_cache_.clear();
      turn_hashes_.clear();
      rows_.clear();
      copyable_rows_.clear();
      row_height_cache_.clear();
      last_attached_start_ = 0;
      last_attached_end_ = 0;
    }
  }

  void EnsureHistoryRows() {
    RebuildIfNeeded();
  }

  std::function<const firmius::shared::AgentHistory *()> history_getter_;
  std::function<std::vector<ftxui::Element>()> live_rows_provider_;
  firmius::tui::ToolViewProvider tool_view_provider_;
  firmius::tui::ProcessStateGetter process_state_getter_;
  firmius::tui::SubagentStateGetter subagent_state_getter_;
  firmius::tui::AgentFocusHandler agent_focus_handler_;
  firmius::tui::HistoryGetter sub_history_getter_;
  firmius::tui::StreamGetter sub_stream_getter_;
  firmius::tui::LiveQuickSummaryProvider live_quick_summary_provider_;
  std::function<std::size_t()> live_measurement_signature_getter_;
  std::function<bool()> show_internal_nudges_getter_;
  std::function<bool()> hide_errors_getter_;
  std::function<bool()> show_turn_footers_getter_;
  firmius::tui::EditableModeEnabledGetter editable_mode_enabled_getter_;
  firmius::tui::EditableMessageSelectedGetter editable_message_selected_getter_;
  firmius::tui::EditableMessageClickHandler editable_message_click_handler_;
  HistoryRenderSignature last_history_signature_{};
  bool history_dirty_ = true;

  std::vector<CachedBlock> block_cache_;
  std::vector<std::size_t> turn_hashes_;
  std::vector<ftxui::Component> rows_;
  std::vector<std::shared_ptr<CopyableRowComponent>> copyable_rows_;
  ftxui::Component history_inner_;
  ftxui::Component history_container_;
  std::vector<int> row_height_cache_;
  size_t last_attached_start_ = 0;
  size_t last_attached_end_ = 0;
  size_t last_visible_start_ = 0;
  size_t last_visible_end_ = 0;
  int last_rebuild_width_ = -1;
  ftxui::Component container_;
  ftxui::Component tail_spacer_;
  std::shared_ptr<firmius::tui::ScrollableBoxComponent> scrollable_;
  std::unordered_map<std::string, std::shared_ptr<firmius::tui::ToolCallView>>
      tool_views_;
  bool copy_drag_candidate_ = false;
  bool pending_bottom_restore_ = false;
  bool copy_drag_started_ = false;
  bool pending_copy_release_ = false;
  int press_x_ = -1;
  int press_y_ = -1;
  int last_drag_x_ = -1;
  int last_drag_y_ = -1;
  int pending_release_x_ = -1;

  // Cached live rows for cheap redraws where the live stream state hasn't
  // changed.
  bool has_cached_live_rows_ = false;
  std::size_t last_live_rows_signature_ = 0;
  ftxui::Elements cached_live_rows_;
  int pending_release_y_ = -1;
};

} // namespace

ftxui::Component firmius::tui::ChatWindow(
    std::function<const shared::AgentHistory *()> history_getter,
    std::function<std::vector<ftxui::Element>()> live_rows_provider,
    ToolViewProvider tool_view_provider,
    ProcessStateGetter process_state_getter,
    SubagentStateGetter subagent_state_getter,
    AgentFocusHandler agent_focus_handler,
    firmius::tui::HistoryGetter sub_history_getter,
    firmius::tui::StreamGetter sub_stream_getter,
    firmius::tui::LiveQuickSummaryProvider live_quick_summary_provider,
    std::function<std::size_t()> live_measurement_signature_getter,
    std::function<bool()> show_internal_nudges_getter,
    std::function<bool()> hide_errors_getter,
    std::function<bool()> show_turn_footers_getter,
    EditableModeEnabledGetter editable_mode_enabled_getter,
    EditableMessageSelectedGetter editable_message_selected_getter,
    EditableMessageClickHandler editable_message_click_handler) {
  return ftxui::Make<ChatWindowComponent>(
      std::move(history_getter), std::move(live_rows_provider),
      std::move(tool_view_provider), std::move(process_state_getter),
      std::move(subagent_state_getter), std::move(agent_focus_handler),
      std::move(sub_history_getter),
      std::move(sub_stream_getter), std::move(live_quick_summary_provider),
      std::move(live_measurement_signature_getter),
      std::move(show_internal_nudges_getter), std::move(hide_errors_getter),
      std::move(show_turn_footers_getter),
      std::move(editable_mode_enabled_getter),
      std::move(editable_message_selected_getter),
      std::move(editable_message_click_handler));
}
