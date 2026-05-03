#include "controllers/PermissionController.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include "modals/PermissionPromptModal.hpp"

namespace firmius::tui {

PermissionController& PermissionController::instance() {
  static PermissionController inst;
  return inst;
}

void PermissionController::activateRequest(const firmius::shared::PermissionEscalationRequest& req) {
  auto &model = PermissionModel::instance();
  if (model.pending_request.has_value()) {
    model.request_queue.push_back(req);
    return;
  }

  model.pending_request = req;
  model.selected_index = 0;
  if (model.on_request_activated) {
    model.on_request_activated();
  }

  auto modal = std::make_shared<PermissionPromptModal>(
      req, [request_id = req.requestId](firmius::shared::PermissionResponse response) {
        auto &model = PermissionModel::instance();
        if (auto *harness = TuiState::instance().harness_) {
          harness->resolvePermissionEscalation(request_id, response);
        }
        model.pending_request.reset();
        model.selected_index = 0;
        if (model.on_request_cleared) {
          model.on_request_cleared();
        }
      });
  TuiState::instance().openModalDirect(modal->create(TuiState::instance()),
                                       modal->name());
}

void PermissionController::promoteNextRequest() {
  auto &model = PermissionModel::instance();
  if (model.pending_request.has_value()) {
    return;
  }
  if (model.request_queue.empty()) {
    if (model.on_request_cleared) {
      model.on_request_cleared();
    }
    return;
  }

  const auto next = model.request_queue.front();
  model.request_queue.erase(model.request_queue.begin());
  activateRequest(next);
}

} // namespace firmius::tui
