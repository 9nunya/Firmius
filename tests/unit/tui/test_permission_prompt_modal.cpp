#include "modals/PermissionPromptModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace firmius::tui {

TuiState &TuiState::instance() {
  static TuiState state;
  return state;
}

TuiState::TuiState() = default;
void TuiState::loadUserPreferences() {}
void TuiState::persistUserPreferences() const {}
void TuiState::openModal(const std::string &) {}
void TuiState::openModalDirect(ftxui::Component modal, const std::string &) {
  modals_.push_back(std::move(modal));
}
void TuiState::popModal() {}
void TuiState::popModalImmediate() {}
void TuiState::replaceModalDirect(ftxui::Component) {}
void TuiState::clearModals() { modals_.clear(); }
void TuiState::deferUiMutation(std::function<void()>) {}
void TuiState::postEvent(ftxui::Event) {}
bool TuiState::cycleThreadPermissionMode() { return false; }
bool TuiState::hasActiveThread() const { return false; }
std::string TuiState::currentThreadId() const { return {}; }
shared::ThreadPermissionMode TuiState::currentThreadPermissionMode() const {
  return shared::ThreadPermissionMode::Request;
}
bool TuiState::needsAnimationTick() const { return false; }
bool TuiState::focusAgent(const std::string &) { return false; }

} // namespace firmius::tui

namespace {

std::string RenderModal(ftxui::Component component, int width = 90,
                        int height = 24) {
  auto element = component->Render();
  ftxui::Screen screen(width, height);
  ftxui::Render(screen, element);
  return screen.ToString();
}

TEST(PermissionPromptModalTest, ReadPermissionUsesLocationLanguage) {
  firmius::tui::TuiState &state = firmius::tui::TuiState::instance();
  state.clearModals();
  const auto &theme = firmius::tui::ThemeManager::instance().getCurrentTheme();
  (void)theme;

  firmius::shared::PermissionEscalationRequest request;
  request.requestType = firmius::shared::PermissionRequestType::Read;
  request.title = "Read permission required";
  request.message = "Approve this file or directory read request.";
  request.targetPath = "/opt/project/.venv";
  request.severity = firmius::shared::CommandSeverity::LOW;
  request.allowAlways = true;

  firmius::shared::PermissionResponse chosen =
      firmius::shared::PermissionResponse::Deny;
  firmius::tui::PermissionPromptModal modal(
      request, [&](firmius::shared::PermissionResponse response) {
        chosen = response;
      });

  const std::string output = RenderModal(modal.create(state));
  EXPECT_NE(output.find("Allow once"), std::string::npos);
  EXPECT_NE(output.find("Always allow this location for this session"),
            std::string::npos);
  EXPECT_NE(output.find("Allow all directory reads this session"), std::string::npos);
  EXPECT_NE(output.find("/opt/project/.venv"), std::string::npos);
  EXPECT_EQ(chosen, firmius::shared::PermissionResponse::Deny);
}

} // namespace
