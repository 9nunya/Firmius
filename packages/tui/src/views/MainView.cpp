#include "views/MainView.hpp"
#include "ClaudexPhrases.hpp"
#include "ConfigLoader.hpp"
#include "NotificationManager.hpp"
#include "TUIHotkeys.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "commands/CommandManager.hpp"
#include "components/AgentStrip.hpp"
#include "components/ChatWindow.hpp"
#include "components/ContextLane.hpp"
#include "components/InputBar.hpp"
#include "components/LiveStatusRow.hpp"
#include "components/PlanLane.hpp"
#include "components/HelpOverlay.hpp"
#include "components/StatusBar.hpp"
#include "components/TitleBar.hpp"
#include "components/TodoLane.hpp"
#include "components/WelcomeScreen.hpp"
#include "controllers/InputController.hpp"
#include "controllers/PermissionController.hpp"
#include "controllers/TranscriptController.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/ProviderRegistry.hpp"
#include "utils/ClaudexActivity.hpp"
#include "utils/ModeCycle.hpp"
#include "utils/PlatformPaths.hpp"
#include <AgentRegistry.hpp>
#include <agents/PurposeLoader.hpp>
#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <harness/Harness.hpp>

void noteTuiRequestAnimationFrameFromLiveStatusRow() __attribute__((weak));

namespace firmius::tui {

namespace {

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

std::chrono::milliseconds
liveRowTransitionDurationForPhrases(const std::string &previous_phrase,
                                    const std::string &next_phrase) {
  const auto max_len = std::max(previous_phrase.size(), next_phrase.size());
  return std::chrono::milliseconds(
      450 + static_cast<int>(std::min<std::size_t>(max_len, 80) * 22));
}

std::string pickLiveRowPhrase(const std::vector<std::string> &phrases,
                              const std::string &current, uint64_t &state) {
  if (phrases.empty())
    return "Standing by.";
  if (phrases.size() == 1)
    return phrases.front();
  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto index =
        static_cast<std::size_t>(nextLiveRowRng(state) % phrases.size());
    if (phrases[index] != current)
      return phrases[index];
  }
  return phrases.front();
}

} // namespace

std::vector<std::string> getSwitchableLeadPersonas() {
  auto purposes = firmius::core::PurposeLoader::listSwitchablePurposes();
  if (purposes.empty()) {
    purposes.push_back("aster");
  }
  return purposes;
}

std::string resolvePersonaTitle(const std::string &personaName) {
  try {
    return firmius::core::PurposeLoader::load(personaName).title;
  } catch (...) {
    return personaName;
  }
}

void UpdateLiveStatusModel(LiveStatusRowModel &model, TuiState &state) {
  if (!state.skin_config_.show_persistent_live_row)
    return;

  auto agentId =
      state.focused_agent_id_.empty() ? "aster" : state.focused_agent_id_;
  auto agent = firmius::core::AgentRegistry::instance().getAgent(agentId);
  const auto *stream = state.stream_state_.getStream(agentId);

  const bool is_verifying =
      agent && (!agent->getContext().state.blockingProcessIds.empty() ||
                !agent->getContext().state.ownedProcesses.empty());
  const bool busy =
      (agent && (agent->isRunning() || agent->isBooting())) ||
      (stream && (stream->provider_waiting || stream->is_thinking)) ||
      is_verifying ||
      (state.status_model_ &&
       (state.status_model_->status_text == "streaming" ||
        state.status_model_->status_text == "executing_tool" ||
        state.status_model_->status_text == "compacting"));
  model.busy = busy;
  model.focused_agent_id = agentId;

  const auto now = std::chrono::steady_clock::now();
  const auto nowMs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count());

  auto it_work = state.agent_work_start_ms_.find(agentId);
  if (it_work == state.agent_work_start_ms_.end()) {
    it_work = state.agent_work_start_ms_.emplace(agentId, nowMs).first;
  }

  auto formatDurationFromMs = [](uint64_t durationMs) {
    const uint64_t totalSeconds = durationMs / 1000;
    const uint64_t minutes = totalSeconds / 60;
    const uint64_t seconds = totalSeconds % 60;
    std::ostringstream out;
    if (minutes > 0)
      out << minutes << "m" << seconds << "s";
    else
      out << seconds << "s";
    return out.str();
  };
  model.elapsed = (nowMs > it_work->second)
                      ? formatDurationFromMs(nowMs - it_work->second)
                      : "0s";

  firmius::shared::AgentTodoList todoList;
  if (!state.thread_.threadId.empty()) {
    firmius::core::ThreadManager tm(
        (firmius::shared::PlatformPaths::firmiusHomeDir() / "threads")
            .string());
    todoList = tm.getAgentTodo(state.thread_.threadId, agentId);
  }

  if (agent) {
    model.activity = inferClaudexActivity(
        agent->getContext(), stream,
        state.status_model_ ? state.status_model_->status_text : "", &todoList);
  } else {
    model.activity = "idle";
  }
  model.phrase_mode = model.activity;

  const auto &phrase_bank = claudexLivePhrasesForMode(model.phrase_mode);
  const std::string phrase_mode_key =
      model.phrase_mode.empty() ? "idle" : model.phrase_mode;

  auto transition_duration = liveRowTransitionDurationForPhrases(
      state.live_row_previous_phrase_, state.live_row_current_phrase_);
  auto transition_elapsed = now - state.live_row_phrase_transition_started_at_;
  bool transition_in_progress = !state.live_row_previous_phrase_.empty() &&
                                transition_elapsed < transition_duration;
  const bool mode_changed =
      state.live_row_last_phrase_key_.empty() ||
      state.live_row_last_phrase_key_.rfind(phrase_mode_key + ":", 0) != 0;

  auto begin_transition = [&](std::string next_key, std::string next_phrase) {
    state.live_row_previous_phrase_ = state.live_row_current_phrase_;
    state.live_row_current_phrase_ = std::move(next_phrase);
    state.live_row_last_phrase_key_ = std::move(next_key);
    state.live_row_phrase_transition_started_at_ = now;
    state.live_row_phrase_next_switch_at_ =
        now + nextLiveRowPhraseDelay(state.live_row_phrase_rng_state_);
    state.live_row_phrase_min_visible_until_ =
        now + minimumLiveRowPhraseVisibleDuration();
  };

  if (state.live_row_current_phrase_.empty()) {
    begin_transition(
        phrase_mode_key + ":initial",
        pickLiveRowPhrase(phrase_bank, "", state.live_row_phrase_rng_state_));
  } else if (mode_changed || now >= state.live_row_phrase_next_switch_at_) {
    std::string next_phrase =
        pickLiveRowPhrase(phrase_bank, state.live_row_current_phrase_,
                          state.live_row_phrase_rng_state_);
    std::string next_key = phrase_mode_key + ":" + next_phrase;
    if (!transition_in_progress &&
        now >= state.live_row_phrase_min_visible_until_) {
      begin_transition(std::move(next_key), std::move(next_phrase));
    }
  }

  transition_duration = liveRowTransitionDurationForPhrases(
      state.live_row_previous_phrase_, state.live_row_current_phrase_);
  transition_elapsed = now - state.live_row_phrase_transition_started_at_;

  if (!state.live_row_previous_phrase_.empty() &&
      transition_elapsed < transition_duration) {
    model.phrase_transition_active = true;
    model.phrase_transition_t =
        std::clamp(static_cast<float>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           transition_elapsed)
                           .count()) /
                       static_cast<float>(transition_duration.count()),
                   0.0f, 1.0f);
    model.phrase_prev = state.live_row_previous_phrase_;
    model.phrase_next = state.live_row_current_phrase_;
    model.phrase.clear();
    ftxui::animation::RequestAnimationFrame();
    if (noteTuiRequestAnimationFrameFromLiveStatusRow) {
      noteTuiRequestAnimationFrameFromLiveStatusRow();
    }
  } else {
    state.live_row_previous_phrase_.clear();
    model.phrase = state.live_row_current_phrase_;
    model.phrase_transition_active = false;
    model.phrase_transition_t = 1.0f;
  }

  const bool should_animate = busy || model.phrase_transition_active ||
                              (state.skin_config_.show_persistent_live_row &&
                               !state.focused_agent_id_.empty());
  if (should_animate &&
      now - state.last_live_raf_request_ >= std::chrono::milliseconds(33)) {
    state.last_live_raf_request_ = now;
    ftxui::animation::RequestAnimationFrame();
    if (noteTuiRequestAnimationFrameFromLiveStatusRow) {
      noteTuiRequestAnimationFrameFromLiveStatusRow();
    }
  }

  model.skin = state.skin_config_;

}

MainView::MainView() {
  auto &store = TUIStore::instance();
  auto &state = TuiState::instance();

  title_bar_ = TitleBar(store.title_model);
  status_bar_ = StatusBar(store.status_model);
  agent_strip_ = AgentStrip(store.agent_strip_model);
  plan_lane_ = PlanLane(store.plan_lane_model);
  todo_lane_ = TodoLane(store.todo_lane_model);
  context_lane_ = ContextLane(store.context_lane_model);

  auto history_getter = []() -> const firmius::shared::AgentHistory * {
    return TuiState::instance().history_.get();
  };
  auto sub_history_getter =
      [](const std::string &id) -> const firmius::shared::AgentHistory * {
    auto it = TuiState::instance().agent_history_cache_.find(id);
    return it != TuiState::instance().agent_history_cache_.end()
               ? it->second.get()
               : nullptr;
  };
  auto stream_getter = [](const std::string &id) {
    return TuiState::instance().stream_state_.getStream(id);
  };
  auto tool_call_getter = [](const std::string &id) {
    return TuiState::instance().stream_state_.getToolView(id);
  };
  auto proc_getter = [](const std::string &id) {
    return TuiState::instance().stream_state_.getProcessStateForToolCall(id);
  };
  auto sub_getter = [](const std::string &id) {
    return TuiState::instance().stream_state_.getSubagentStateForToolCall(id);
  };
  auto focus_setter = [](const std::string &id) {
    TuiState::instance().focusAgent(id);
  };

  auto summary_cluster_getter = []() {
    std::unordered_map<int, LiveQuickSummaryCluster> clusters;
    std::vector<int> cluster_order;
    auto &ss = TuiState::instance().stream_state_;
    auto &store = TUIStore::instance();

    for (const auto &entry : ss.getTimeline()) {
      if (entry.kind != TimelineEntry::Kind::ToolCall ||
          entry.agentId != store.focused_agent_id)
        continue;
      auto view = ss.getToolView(entry.id);
      if (!view || MainView::persistedToolCallIds().count(entry.id))
        continue;
      if (!ShouldRenderToolCallView(*view))
        continue;

      const auto desc = DescribeQuickToolCall(*view);
      if (!IsQuickToolCategory(desc.category))
        continue;

      int cid = ss.getToolCallClusterId(entry.id);
      if (cid < 0)
        cid = 0;
      if (!clusters.count(cid))
        cluster_order.push_back(cid);
      auto &cluster = clusters[cid];
      cluster.merge_with_history = (cid == 0);

      auto key = static_cast<int>(desc.category);
      auto &summary = cluster.summaries[key];
      if (summary.category == QuickToolCategory::None) {
        summary.category = desc.category;
        cluster.category_order.push_back(desc.category);
      }
      if (!desc.target.empty())
        summary.targets.push_back(desc.target);

      using firmius::shared::ToolPhase;
      if (view->phase == ToolPhase::Preparing) {
        summary.has_preparing = true;
        summary.preparing_count++;
      } else if (view->phase == ToolPhase::Called) {
        summary.has_live = true;
        summary.live_count++;
      } else if (view->phase == ToolPhase::Error)
        summary.has_error = true;
    }
    std::vector<LiveQuickSummaryCluster> res;
    for (int id : cluster_order)
      res.push_back(std::move(clusters[id]));
    return res;
  };

  auto signature_getter = []() {
    auto &state = TuiState::instance();
    return BuildFocusedChatLiveMeasurementSignature(
        state.stream_state_, state.focused_agent_id_, state.thread_.threadId,
        MainView::persistedToolCallIds());
  };

  chat_component_ = ChatWindow(
      history_getter,
      []() { return TranscriptController::instance().generateLiveRows(); },
      tool_call_getter, proc_getter, sub_getter, focus_setter,
      sub_history_getter, stream_getter, summary_cluster_getter,
      signature_getter,
      []() {
        return TuiState::instance().harness_ ? TuiState::instance()
                                                   .harness_->getConfig()
                                                   .showInternalNudges
                                             : false;
      },
      []() {
        return TuiState::instance().harness_
                   ? TuiState::instance().harness_->getConfig().hideErrors
                   : false;
      },
      []() { return TUIStore::instance().skin_config.show_turn_footers; },
      []() { return TranscriptModel::instance().edit_mode_active; },
      [](uint64_t ts) {
        auto &model = TranscriptModel::instance();
        if (model.selected_editable_message_index < 0)
          return false;
        return (size_t)model.selected_editable_message_index <
                   model.editable_user_messages.size() &&
               model.editable_user_messages
                       [model.selected_editable_message_index]
                           .timestamp == ts;
      },
      [](uint64_t ts) {
        TranscriptController::instance().selectEditableMessageByTimestamp(ts);
      });

  input_component_ = InputBar(
      store.input_model,
      [&state, &store](const std::string &text,
                       const std::vector<PastedBlock> &images) {
        auto applyPendingRewrite = [&]() -> bool {
          auto &t_model = TranscriptModel::instance();
          if (!t_model.pending_edit_message || !state.harness_)
            return true;
          const uint64_t cutoff =
              t_model.pending_edit_message->timestamp > 0
                  ? t_model.pending_edit_message->timestamp - 1
                  : 0;
          state.suppress_next_history_undone_refresh_ = true;
          firmius::core::UndoResult result;
          try {
            result = state.harness_->undoAfterTimestamp(cutoff);
          } catch (const std::exception &ex) {
            state.suppress_next_history_undone_refresh_ = false;
            NotificationManager::instance().notifyError(
                "Rewrite Not Applied", ex.what(), false);
            return false;
          }
          state.suppress_next_history_undone_refresh_ = false;
          if (result.turnsRemoved == 0) {
            NotificationManager::instance().notifyWarning(
                "Rewrite Not Applied", "Could not prune turn.",
                std::chrono::milliseconds(2000));
            return false;
          }
          t_model.pending_edit_message.reset();
          return true;
        };

        std::vector<shared::ImageContent> image_contents;
        for (const auto &img : images) {
          shared::ImageContent content;
          content.url =
              "data:" + (img.mime_type.empty() ? "image/png" : img.mime_type) +
              ";base64," + img.content;
          content.mediaType =
              img.mime_type.empty() ? "image/png" : img.mime_type;
          image_contents.push_back(content);
        }

        if (!text.empty() && text[0] == '/') {
          CommandCtx ctx{&state};
          if (CommandManager::instance().executeCommand(ctx, text))
            return;
        }

        if (state.getViewMode() != TuiState::ViewMode::Welcome) {
          if (!applyPendingRewrite())
            return;
        }
        state.submitPrompt(text, image_contents);

        if (store.input_model && store.input_model->buffer)
          store.input_model->buffer->clear();
        if (store.input_model && store.input_model->cursor)
          *store.input_model->cursor = 0;
        return;
      });

  welcome_screen_ = WelcomeScreen();

  auto container = ftxui::Container::Vertical({
      chat_component_,
      input_component_,
  });
  store.input_model->help_opened_from_empty_query =
      &help_opened_from_empty_query_;
  store.input_model->command_palette_requested =
      &command_palette_requested_;

  Add(container);
}

ftxui::Element MainView::OnRender() {
  auto &store = TUIStore::instance();
  auto &state = TuiState::instance();
  const auto &theme = ThemeManager::instance().getCurrentTheme();

  // 1. Focus Management
  const bool has_modals = !state.modals_.empty();
  const bool edit_mode_active = TranscriptModel::instance().edit_mode_active;

  if (store.input_model) {
    store.input_model->is_focused = !edit_mode_active && !has_modals;
  }

  if (has_modals) {
    // Top modal gets focus (restored from old behavior)
    if (!state.modals_.empty() && state.modals_.back()) {
      state.modals_.back()->TakeFocus();
    }
  } else if (input_component_ && !edit_mode_active) {
    input_component_->TakeFocus();
  }

  const bool is_claudex = store.skin_kind == SkinKind::Claudex;

  // 2. Chat Area
  ftxui::Element chat_area;
  if (state.getViewMode() == TuiState::ViewMode::Chat)
    chat_area = chat_component_->Render();
  else
    chat_area = welcome_screen_->Render();
  chat_area = chat_area | ftxui::flex;

  // 3. Bottom Bar Construction
  ftxui::Elements bottom_bar_children;

  const bool show_strip =
      show_agent_strip_ && store.skin_config.show_agent_strip &&
      store.agent_strip_model && store.agent_strip_model->items.size() > 1;
  if (show_strip) {
    bottom_bar_children.push_back(agent_strip_->Render());
  }

  // Live status row (Claudex) - always render if persistent_live_row is enabled
  if (is_claudex && store.skin_config.show_persistent_live_row) {
    LiveStatusRowModel live_status_model;
    UpdateLiveStatusModel(live_status_model, state);
    auto live_el = RenderLiveStatusRow(live_status_model);
    if (live_el) {
      if (!bottom_bar_children.empty()) {
        bottom_bar_children.push_back(ftxui::separator() |
                                      ftxui::color(theme.base.border));
      }
      bottom_bar_children.push_back(live_el);
    }
  }

  bottom_bar_children.push_back(ftxui::separator() |
                                (state.isQuitArmed()
                                     ? ftxui::color(ftxui::Color::Red)
                                     : ftxui::color(theme.base.border)));
  bottom_bar_children.push_back(input_component_->Render());
  bottom_bar_children.push_back(ftxui::separator() |
                                (state.isQuitArmed()
                                     ? ftxui::color(ftxui::Color::Red)
                                     : ftxui::color(theme.base.border)));

  const bool show_status_bar =
      store.skin_config.status_bar_mode != SkinStatusBarMode::Hidden;

  if (show_status_bar) {
    bottom_bar_children.push_back(status_bar_->Render());
  }

  auto bottom_bar = ftxui::vbox(std::move(bottom_bar_children)) | ftxui::xflex;

  // 4. Layout
  ftxui::Elements main_vbox;
  if (store.skin_config.show_title_bar)
    main_vbox.push_back(title_bar_->Render());
  main_vbox.push_back(chat_area);

  // Separator between chat and bottom bar
  // main_vbox.push_back(ftxui::separator() | ftxui::color(theme.base.border));

  const bool should_show_work_panel =
      show_work_panel_ && store.skin_config.show_work_panel &&
      state.getViewMode() != TuiState::ViewMode::Welcome;
  if (should_show_work_panel) {
    ftxui::Elements tab_elements;
    auto add_tab = [&](WorkPanelTab tab, const std::string &label) {
      bool is_selected = (selected_work_panel_tab_ == tab);
      auto el = ftxui::text(" " + label + " ") | ftxui::bold;
      if (is_selected)
        el = el | ftxui::color(theme.base.bg) |
             ftxui::bgcolor(theme.base.highlight);
      else
        el = el | ftxui::color(theme.base.dim);

      // Make tab clickable
      auto tab_comp = ftxui::Renderer([el, is_selected, &theme](bool focused) {
        auto final_el = el;
        if (focused && !is_selected) {
          final_el = final_el | ftxui::bgcolor(theme.base.dim) |
                     ftxui::color(theme.base.bg);
        }
        return final_el;
      });

      tab_elements.push_back(
          ftxui::CatchEvent(tab_comp, [this, tab](ftxui::Event event) {
            if (event.is_mouse() &&
                event.mouse().button == ftxui::Mouse::Left &&
                event.mouse().motion == ftxui::Mouse::Pressed) {
              selected_work_panel_tab_ = tab;
              return true;
            }
            return false;
          })->Render());
      tab_elements.push_back(ftxui::text(" "));
    };
    add_tab(WorkPanelTab::Plan, "PLAN");
    add_tab(WorkPanelTab::Todo, "TODO");
    add_tab(WorkPanelTab::Context, "CONTEXT");
    auto tab_bar = ftxui::hbox(std::move(tab_elements));

    ftxui::Element active_lane;
    if (selected_work_panel_tab_ == WorkPanelTab::Plan)
      active_lane = plan_lane_->Render();
    else if (selected_work_panel_tab_ == WorkPanelTab::Todo)
      active_lane = todo_lane_->Render();
    else
      active_lane = context_lane_->Render();

    main_vbox.push_back((ftxui::separator() | ftxui::color(theme.base.border)) |
                        ftxui::reflect(work_panel_separator_box_));
    main_vbox.push_back(ftxui::vbox({tab_bar, active_lane | ftxui::flex}) |
                        ftxui::size(ftxui::HEIGHT, ftxui::EQUAL,
                                    work_panel_height_override_ > 0
                                        ? work_panel_height_override_
                                        : 12));
  }

  main_vbox.push_back(bottom_bar);

  // 5. Create main content from layout
  return ftxui::vbox(std::move(main_vbox)) | ftxui::flex |
         ftxui::bgcolor(theme.base.bg);
}

bool MainView::OnEvent(ftxui::Event event) {
  if (ftxui::ComponentBase::OnEvent(event))
    return true;

  auto &store = TUIStore::instance();
  auto &state = TuiState::instance();

  if (help_opened_from_empty_query_) {
    help_opened_from_empty_query_ = false;
    state.openModalDirect(HelpOverlay(state), "help_overlay");
    return true;
  }
  if (command_palette_requested_) {
    command_palette_requested_ = false;
    state.openModal("command_palette");
    return true;
  }

  if (event.is_mouse()) {
    const auto mouse = event.mouse();
    if (mouse.button == ftxui::Mouse::Left &&
        mouse.motion == ftxui::Mouse::Pressed) {
      if (show_work_panel_ &&
          work_panel_separator_box_.Contain(mouse.x, mouse.y)) {
        dragging_work_panel_ = true;
        drag_origin_y_ = mouse.y;
        drag_origin_value_ =
            work_panel_height_override_ > 0 ? work_panel_height_override_ : 10;
        return true;
      }
      if (show_agent_strip_ &&
          agent_strip_separator_box_.Contain(mouse.x, mouse.y)) {
        dragging_agent_strip_ = true;
        drag_origin_y_ = mouse.y;
        drag_origin_value_ = agent_strip_visible_rows_;
        return true;
      }
    }

    if (mouse.motion == ftxui::Mouse::Moved) {
      if (dragging_work_panel_) {
        const int delta = drag_origin_y_ - mouse.y;
        work_panel_height_override_ =
            std::clamp(drag_origin_value_ + delta, 4,
                       std::max(6, last_terminal_height_ - 8));
        return true;
      }
      if (dragging_agent_strip_) {
        const int delta = drag_origin_y_ - mouse.y;
        agent_strip_visible_rows_ =
            std::clamp(drag_origin_value_ + delta, 1, 12);
        store.agent_strip_model->visible_rows = agent_strip_visible_rows_;
        return true;
      }
    }

    if (mouse.motion == ftxui::Mouse::Released) {
      dragging_work_panel_ = false;
      dragging_agent_strip_ = false;
    }
  }

  if (event == ftxui::Event::F6) {
    show_agent_strip_ = !show_agent_strip_;
    return true;
  }
  if (event == ftxui::Event::F7) {
    show_work_panel_ = !show_work_panel_;
    return true;
  }

  return false;
}

bool MainView::HandleGlobalHotkeys(ftxui::Event event) {
  auto &store = TUIStore::instance();
  auto &state = TuiState::instance();

  if (IsOpenHelpEvent(event)) {
    state.openModalDirect(HelpOverlay(state), "help_overlay");
    return true;
  }
  // Ctrl+O - Cycle work-lane tabs
  if (event == ftxui::Event::Special("\x0F")) {
    const bool hasPlan =
        store.plan_lane_model && store.plan_lane_model->visible;
    const bool hasTodo =
        store.todo_lane_model && store.todo_lane_model->visible;
    const bool hasContext =
        store.context_lane_model && store.context_lane_model->visible;
    selected_work_panel_tab_ = nextWorkPanelTab(selected_work_panel_tab_,
                                                hasPlan, hasTodo, hasContext);
    state.postEvent(ftxui::Event::Custom);
    return true;
  }

  // Ctrl+T - Cycle Theme
  if (event == ftxui::Event::Special("\x14")) {
    ThemeManager::instance().cycleTheme();
    if (chat_component_)
      chat_component_->OnEvent(ftxui::Event::Special("ThemeChanged"));
    state.postEvent(ftxui::Event::Custom);
    return true;
  }

  // Ctrl+H - Toggle notifications
  if (event == ftxui::Event::Special("\x08")) {
    NotificationManager::instance().toggleVisibility();
    return true;
  }

  // Ctrl+Y - Cycle the active mode for the focused agent (or, on the
  // welcome screen, the pre-thread initial mode). The cycle includes a
  // "no mode" entry plus the persona's sub-modes plus system-level modes.
  // Visible immediately in the status-bar mode pill.
  if (event == ftxui::Event::Special("\x19")) {
    if (state.getViewMode() == TuiState::ViewMode::Welcome) {
      const std::string persona = state.thread_.leadPersona.empty()
                                      ? std::string("aster")
                                      : state.thread_.leadPersona;
      const std::string current = state.thread_.initialMode;
      const std::string next = cycleMode(current, persona, +1);
      state.thread_.initialMode = next;
      store.thread_metadata.initialMode = next;
      state.requestRefresh(RefreshFlags::Status);
      NotificationManager::instance().notifyInfo(
          "Mode",
          next.empty() ? "Initial mode cleared (no overlay)."
                       : "Initial mode: " + next,
          std::chrono::milliseconds(1500));
      state.postEvent(ftxui::Event::Custom);
    } else {
      auto agent = firmius::core::AgentRegistry::instance().getAgent(
          state.focused_agent_id_.empty() ? store.focused_agent_id
                                          : state.focused_agent_id_);
      if (agent) {
        auto &ctx = agent->getMutableContext();
        const std::string persona = ctx.config.personaName;
        const std::string current = ctx.state.activeMode;
        const std::string next = cycleMode(current, persona, +1);
        ctx.state.activeMode = next;
        state.requestRefresh(RefreshFlags::Status);
        NotificationManager::instance().notifyInfo(
            "Mode",
            next.empty() ? "Mode cleared on " + ctx.identity.friendlyName
                         : "Mode: " + next + " (" + ctx.identity.friendlyName +
                               ")",
            std::chrono::milliseconds(1500));
        state.postEvent(ftxui::Event::Custom);
      }
    }
    return true;
  }

  // Ctrl+P - Focus Parent
  if (event == ftxui::Event::Special("\x10")) {
    auto agent = firmius::core::AgentRegistry::instance().getAgent(
        state.focused_agent_id_.empty() ? store.focused_agent_id
                                        : state.focused_agent_id_);
    if (agent && !agent->getContext().identity.parentId.empty())
      state.focusAgent(agent->getContext().identity.parentId);
    return true;
  }

  if (event == ftxui::Event::Special("\x0E") ||
      event == ftxui::Event::Special("\x02")) {
    const std::string focusedId =
        state.focused_agent_id_.empty() ? store.focused_agent_id
                                        : state.focused_agent_id_;
    auto agents = focusCycleCandidates(focusedId);
    if (!agents.empty()) {
      auto it = std::find(agents.begin(), agents.end(), focusedId);
      if (it == agents.end())
        it = agents.begin();
      else if (event == ftxui::Event::Special("\x0E")) { // Next
        if (++it == agents.end())
          it = agents.begin();
      } else { // Prev
        if (it == agents.begin())
          it = agents.end();
        --it;
      }
      state.focusAgent(*it);
    }
    return true;
  }

  // Shift+Tab - Cycle Lead Persona
  if (event.input() == "\x1b[Z") {
    auto agent = firmius::core::AgentRegistry::instance().getAgent(
        state.focused_agent_id_.empty() ? store.focused_agent_id
                                        : state.focused_agent_id_);
    if (agent && !agent->getContext().identity.parentId.empty()) {
      NotificationManager::instance().notifyWarning(
          "Lead Mode", "Subagent focused.", std::chrono::milliseconds(1500));
      return true;
    }

    auto modes = getSwitchableLeadPersonas();
    std::string current = agent ? agent->getContext().identity.name
                                : state.thread_.leadPersona;
    if (current.empty())
      current = "aster";

    size_t next_idx = 0;
    auto it = std::find(modes.begin(), modes.end(), current);
    if (it != modes.end())
      next_idx = (static_cast<size_t>(it - modes.begin()) + 1) % modes.size();
    std::string next = modes[next_idx];

    if (state.harness_) {
      try {
        if (state.harness_->switchLeadPersona(next)) {
          state.focusAgent(state.harness_->focusedAgentId());
          NotificationManager::instance().notifyInfo(
              "Lead Mode", "Lead persona: " + resolvePersonaTitle(next),
              std::chrono::milliseconds(1500));
        }
      } catch (const std::exception &ex) {
        NotificationManager::instance().notifyError("Lead Mode", ex.what(),
                                                    false);
      }
    }
    return true;
  }

  // Ctrl+E - Toggle/Commit Edit Mode
  if (event == ftxui::Event::Special("\x05")) {
    auto &model = TranscriptModel::instance();
    if (!model.edit_mode_active) {
      TranscriptController::instance().rebuildEditableUserMessages();
      model.edit_mode_active = !model.editable_user_messages.empty();
    } else if (model.selected_editable_message_index >= 0) {
      TranscriptController::instance().commitSelectedEditableMessageToInput();
    } else {
      model.edit_mode_active = false;
    }
    if (chat_component_)
      chat_component_->OnEvent(ftxui::Event::Special("TranscriptChanged"));
    state.postEvent(ftxui::Event::Custom);
    state.requestRefresh(RefreshFlags::ChatTranscript);
    return true;
  }

  if (IsPermissionCycleEvent(event)) {
    state.cycleThreadPermissionMode();
    return true;
  }

  if (IsTranscriptUndoEvent(event)) {
    state.triggerTranscriptUndoFromHotkey();
    return true;
  }
  if (IsTranscriptRedoEvent(event)) {
    state.triggerTranscriptRedoFromHotkey();
    return true;
  }
  if (IsTranscriptUndoToUserBoundaryEvent(event)) {
    state.triggerTranscriptUndoToUserBoundaryFromHotkey();
    return true;
  }
  if (IsEditUndoEvent(event)) {
    state.triggerEditUndoFromHotkey();
    return true;
  }
  if (IsEditRedoEvent(event)) {
    state.triggerEditRedoFromHotkey();
    return true;
  }

  if (IsVariantCycleEvent(event)) {
    if (state.harness_ && !store.focused_agent_id.empty()) {
      auto agent = firmius::core::AgentRegistry::instance().getAgent(
          store.focused_agent_id);
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
            if (current.empty())
              next = info.variants.front().variantName;
            else {
              auto it_v = std::find_if(
                  info.variants.begin(), info.variants.end(),
                  [&](const auto &v) { return v.variantName == current; });
              if (it_v != info.variants.end() &&
                  std::next(it_v) != info.variants.end())
                next = std::next(it_v)->variantName;
            }
            try {
              state.harness_->switchModel(ctx.config.providerId,
                                          ctx.config.modelId, next);
            } catch (const std::exception &ex) {
              NotificationManager::instance().notifyError("Variant", ex.what(),
                                                          false);
            }
          }
        }
      }
    }
    return true;
  }

  if (event == ftxui::Event::F7) {
    if (show_work_panel_) {
      int next = static_cast<int>(selected_work_panel_tab_) + 1;
      if (next > 2)
        next = 0;
      selected_work_panel_tab_ = static_cast<WorkPanelTab>(next);
    } else {
      show_work_panel_ = true;
    }
    return true;
  }

  if (event.input() == std::string("\x1b") + "1") {
    selected_work_panel_tab_ = WorkPanelTab::Plan;
    show_work_panel_ = true;
    return true;
  }
  if (event.input() == std::string("\x1b") + "2") {
    selected_work_panel_tab_ = WorkPanelTab::Todo;
    show_work_panel_ = true;
    return true;
  }
  if (event.input() == std::string("\x1b") + "3") {
    selected_work_panel_tab_ = WorkPanelTab::Context;
    show_work_panel_ = true;
    return true;
  }

  if (event == ftxui::Event::Escape) {
    auto &t_model = TranscriptModel::instance();
    if (t_model.edit_mode_active) {
      t_model.edit_mode_active = false;
      chat_component_->OnEvent(ftxui::Event::Special("TranscriptChanged"));
      return true;
    }
    if (!TuiState::instance().modals_.empty()) {
      TuiState::instance().popModalImmediate();
      return true;
    }
    if (state.harness_)
      state.harness_->abortAndFlushQueuedMessages();
    return true;
  }

  return false;
}

const std::unordered_set<std::string> &MainView::persistedToolCallIds() {
  auto &state = TuiState::instance();
  auto &store = TUIStore::instance();
  static const std::unordered_set<std::string> empty;

  if (store.focused_agent_id.empty())
    return empty;

  auto it =
      state.agent_persisted_tool_call_ids_cache_.find(store.focused_agent_id);
  if (it != state.agent_persisted_tool_call_ids_cache_.end()) {
    return it->second;
  }
  return empty;
}

std::shared_ptr<MainView> MakeMainView() {
  return std::make_shared<MainView>();
}

} // namespace firmius::tui
