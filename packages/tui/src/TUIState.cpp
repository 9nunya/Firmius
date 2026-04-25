#include "TUIState.hpp"
#include "controllers/AppController.hpp"
#include "controllers/InputController.hpp"
#include "controllers/TranscriptController.hpp"
#include "controllers/BackgroundController.hpp"
#include "harness/Harness.hpp"
#include "AgentRegistry.hpp"
#include "ThemeManager.hpp"
#include "NotificationManager.hpp"
#include "UserPreferences.hpp"
#include "modals/ModalRegistry.hpp"
#include "providers/ProviderRegistry.hpp"
#include <iomanip>
#include <sstream>

namespace firmius::tui {

namespace detail {
bool shouldNotifyHiddenChatError(const std::string &focused_id, const std::string &error_id, bool hide_errors) {
  if (error_id.empty()) return false;
  if (focused_id == error_id) return true;
  if (hide_errors) return false;
  return false;
}
}

std::size_t BuildFocusedChatLiveMeasurementSignature(
    const StreamStateManager &stream_state, const std::string &focused_agent_id,
    const std::string &thread_id, const std::unordered_set<std::string> &persisted_tool_call_ids) {
  (void)stream_state; (void)focused_agent_id; (void)thread_id; (void)persisted_tool_call_ids;
  return 0;
}

std::vector<shared::AgentTurn> expandCompactionTranscriptForDisplay(
    const std::vector<shared::AgentTurn>& turns,
    const std::unordered_map<std::string, firmius::core::CompactionSnapshot>& snapshots) {
  (void)snapshots;
  return turns;
}

void noteTuiModalOpenRequested(const std::string &name) { (void)name; }

TuiState &TuiState::instance() {
  static TuiState inst;
  return inst;
}

TuiState::TuiState() { loadUserPreferences(); }

void TuiState::initModels() {
  auto& store = TUIStore::instance();
  title_model_ = store.title_model;
  status_model_ = store.status_model;
  input_model_ = store.input_model;
  agent_strip_model_ = store.agent_strip_model;
  plan_lane_model_ = store.plan_lane_model;
  todo_lane_model_ = store.todo_lane_model;
  context_lane_model_ = store.context_lane_model;
}

void TuiState::loadUserPreferences() {
  const auto prefs = firmius::tui::loadUserPreferences();
  if (prefs.preferred_permission_mode.has_value()) {
    thread_.permissionMode = *prefs.preferred_permission_mode;
  }
}

void TuiState::persistUserPreferences() const {
  UserPreferences prefs;
  prefs.preferred_permission_mode = thread_.permissionMode;
  saveUserPreferences(prefs);
}

void TuiState::init(firmius::core::Harness &harness,
                    const shared::ThreadMetadata &thread,
                    const std::string &focused_agent_id) {
  harness_ = &harness;
  thread_ = thread;
  focused_agent_id_ = focused_agent_id;
  initModels();
  TUIStore::instance().thread_metadata = thread;
  TUIStore::instance().focused_agent_id = focused_agent_id;
  
  if (title_model_) {
    title_model_->title = thread.title;
    title_model_->thread_id = thread.threadId;
  }

  subscription_id_ =
      harness_->subscribe([this](const firmius::shared::AppEvent &ev) {
        event_queue_.push(ev);
        if (screen_) {
          postEvent(ftxui::Event::Custom);
        }
      });

  refreshFocusedHistory();
  requestRefresh(RefreshFlags::Status);
  requestRefresh(RefreshFlags::AgentStrip);
}

void TuiState::syncCurrentThreadMetadataFromHarness(bool preserve_live_state) {
  if (!harness_) return;
  const std::string curId = harness_->currentThreadId();
  if (curId.empty()) return;
  
  for (const auto &metadata : harness_->listThreads()) {
    if (metadata.threadId != curId) continue;
    if (!preserve_live_state) {
      handleAppEvent(shared::ThreadChanged{curId, metadata});
      return;
    }
    thread_ = metadata;
    break;
  }
  refreshFocusedHistory();
}

void TuiState::refreshFocusedHistory() {
  if (!harness_ || focused_agent_id_.empty()) {
    history_.reset();
    return;
  }
  history_ = harness_->getAgentHistoryPtr(focused_agent_id_);
  TranscriptModel::instance().active_history = history_;
}

void TuiState::attachScreen(ftxui::ScreenInteractive *screen) {
  screen_ = screen;
}

void TuiState::shutdown() {
  if (harness_ && subscription_id_ >= 0) {
    harness_->unsubscribe(subscription_id_);
    subscription_id_ = -1;
  }
  screen_ = nullptr;
}

void TuiState::requestQuit() {
  if (screen_) screen_->ExitLoopClosure()();
}

bool TuiState::isQuitArmed() const {
  return false;
}

std::string TuiState::exitSummaryText() const {
  return "Session Finished";
}

void TuiState::drainEvents() {
  custom_event_pending_ = false;
  auto drained = event_queue_.drainAll();
  for (const auto &ev : drained) {
    onEvent(ev);
  }
  applyPendingRefreshes();
}

void TuiState::clearInputBuffer() {
  if (input_model_ && input_model_->buffer) input_model_->buffer->clear();
  if (input_model_ && input_model_->cursor) *input_model_->cursor = 0;
}

bool TuiState::handleCtrlC() {
  return false; 
}

bool TuiState::cycleThreadPermissionMode() { return false; }
bool TuiState::hasActiveThread() const { return false; }
std::string TuiState::currentThreadId() const { return thread_.threadId; }
shared::ThreadPermissionMode TuiState::currentThreadPermissionMode() const {
  return shared::ThreadPermissionMode::Request;
}

void TuiState::setViewMode(ViewMode mode) {
  view_mode_ = mode;
  TUIStore::instance().view_mode = static_cast<TUIStore::ViewMode>(mode);
}

TuiState::ViewMode TuiState::getViewMode() const {
  return view_mode_;
}

void TuiState::openModal(const std::string &name) {
  ModalRegistry::instance().openModal(name, *this, true);
}

void TuiState::openModalDirect(ftxui::Component modal, const std::string &modal_name) {
  (void)modal_name;
  modals_.push_back(std::move(modal));
  if (modal) modal->TakeFocus();
  if (screen_) postEvent(ftxui::Event::Custom);
}

void TuiState::popModal() {
  if (!modals_.empty()) modals_.pop_back();
  if (modals_.empty()) {
     if (chat_component_) chat_component_->TakeFocus();
  } else {
     modals_.back()->TakeFocus();
  }
  if (screen_) postEvent(ftxui::Event::Custom);
}

void TuiState::popModalImmediate() {
  popModal();
}

void TuiState::deferUiMutation(std::function<void()> action) {
  deferred_ui_mutations_.push_back(std::move(action));
  postEvent(ftxui::Event::Custom);
}

SkinKind TuiState::currentSkinKind() const {
  return skin_config_.kind;
}

void TuiState::setSkinKind(SkinKind kind) {
  skin_config_ = defaultSkinConfig(kind);
  ThemeManager::instance().setTheme(kind == SkinKind::Claudex ? "Claudex" : "Firmius");
  TUIStore::instance().skin_kind = kind;
  TUIStore::instance().skin_config = skin_config_;
  if (chat_component_) chat_component_->OnEvent(ftxui::Event::Special("ThemeChanged"));
  if (screen_) postEvent(ftxui::Event::Custom);
}

void TuiState::requestRefresh(RefreshFlags flags) {
  pending_refresh_flags_ |= static_cast<unsigned int>(flags);
}

void TuiState::notifyChatTranscriptChanged() {
  requestRefresh(RefreshFlags::ChatTranscript);
}

void TuiState::applyPendingRefreshes() {
  const unsigned int flags = pending_refresh_flags_;
  if (flags == 0) return;
  pending_refresh_flags_ = 0;

  if (flags & static_cast<unsigned int>(RefreshFlags::Status)) updateStatusModel();
  if (flags & static_cast<unsigned int>(RefreshFlags::AgentStrip)) updateAgentStripModel();
  if (flags & static_cast<unsigned int>(RefreshFlags::PlanLane)) updatePlanLaneModel();
  if (flags & static_cast<unsigned int>(RefreshFlags::ContextLane)) updateContextLaneModel();
  if ((flags & static_cast<unsigned int>(RefreshFlags::ChatTranscript)) && chat_component_) {
     chat_component_->OnEvent(ftxui::Event::Special("TranscriptChanged"));
  }
}

void TuiState::updateStatusModel() {
  if (!status_model_) return;
  
  if (!focused_agent_id_.empty()) {
    auto agent = firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
    if (agent) {
      status_model_->status_text = "idle";
      status_model_->is_active = agent->isRunning();
    }
  }
}

void TuiState::updatePlanLaneModel() {
  if (!plan_lane_model_) return;
  *plan_lane_model_ = active_plan_state_.model();
}

void TuiState::updateContextLaneModel() {
  if (!context_lane_model_) return;
  if (focused_agent_id_.empty()) return;
  auto agent = firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
  if (!agent) return;
  const auto &ctx = agent->getContext();
  context_lane_model_->visible = true;
  context_lane_model_->owner_label = ctx.identity.friendlyName;
  context_lane_model_->model_label = ctx.config.providerId + "/" + ctx.config.modelId;
}

void TuiState::updateAgentStripModel() {
  if (!agent_strip_model_) return;
  auto all_ids = firmius::core::AgentRegistry::instance().listAll();
  agent_strip_model_->items.clear();
  for (const auto &id : all_ids) {
    auto agent = firmius::core::AgentRegistry::instance().getAgent(id);
    if (!agent) continue;
    const auto &ctx = agent->getContext();
    AgentStripItem item;
    item.id = id;
    item.title = ctx.identity.role.empty() ? ctx.identity.name : ctx.identity.role;
    item.is_busy = agent->isRunning();
    item.is_focused = (id == focused_agent_id_);
    agent_strip_model_->items.push_back(std::move(item));
  }
}

void TuiState::onEvent(const shared::AppEvent &ev) {
  AppController::instance().dispatch(ev, harness_);
  if (screen_) {
    postEvent(ftxui::Event::Custom);
  }
}

void TuiState::postEvent(ftxui::Event event) {
  if (!screen_) return;
  if (custom_event_pending_ && event == ftxui::Event::Custom) return;
  if (event == ftxui::Event::Custom) custom_event_pending_ = true;
  screen_->PostEvent(event);
}

ftxui::Component TuiState::root() {
  if (!root_component_) {
    TUIStore::instance().thread_id = thread_.threadId;
    TUIStore::instance().focused_agent_id = focused_agent_id_;
    initModels();
    main_view_ = MakeMainView();
    
    auto modal_renderer = ftxui::Renderer(main_view_, [this]() {
      ftxui::Element current = main_view_->Render();
      for (const auto &modal : modals_) {
        current = ftxui::dbox(
            {current, modal->Render() | ftxui::center});
      }
      auto notifications = NotificationManager::instance().render();
      current = ftxui::dbox({current, notifications});
      return current;
    });
    
    auto routed_component = ftxui::CatchEvent(modal_renderer, [this](ftxui::Event event) {
      if (event == ftxui::Event::Custom) {
        if (!deferred_ui_mutations_.empty()) {
          auto deferred = std::move(deferred_ui_mutations_);
          deferred_ui_mutations_.clear();
          for (auto &mutation : deferred) {
            mutation();
          }
        }
        custom_event_pending_ = false;
        drainEvents();
        return true;
      }
      if (event == ftxui::Event::Special("PopModal")) {
        if (!modals_.empty()) {
          modals_.pop_back();
        }
        if (modals_.empty()) {
          postEvent(ftxui::Event::Custom);
        } else {
          modals_.back()->TakeFocus();
        }
        return true;
      }
      if (event == ftxui::Event::Escape && !modals_.empty()) {
        postEvent(ftxui::Event::Special("PopModal"));
        return true;
      }
      return main_view_->OnEvent(event);
    });
    
    root_component_ = routed_component;
  }
  return root_component_;
}

void TuiState::handleAppEvent(const shared::AppEvent &ev) {
  onEvent(ev);
}

} // namespace firmius::tui
