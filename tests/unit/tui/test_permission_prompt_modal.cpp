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
  request.toolName = "Files.Read";
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
  EXPECT_NE(output.find("Allow this exact location for this session"),
            std::string::npos);
  EXPECT_NE(output.find("Allow every read this session"), std::string::npos);
  EXPECT_NE(output.find("Tool: Files.Read"), std::string::npos);
  EXPECT_NE(output.find("Path: /opt/project/.venv"), std::string::npos);
  EXPECT_EQ(chosen, firmius::shared::PermissionResponse::Deny);
}

TEST(PermissionPromptModalTest,
     CommandPermissionNamesExactCommandToolAndSelection) {
  firmius::tui::TuiState &state = firmius::tui::TuiState::instance();
  state.clearModals();

  firmius::shared::PermissionEscalationRequest request;
  request.requestType = firmius::shared::PermissionRequestType::Command;
  request.title = "Command permission required";
  request.message = "Approve this command request.";
  request.command = "git status --short";
  request.commandPrimary = "git";
  request.toolName = "Process";
  request.toolCallId = "call-42";
  request.severity = firmius::shared::CommandSeverity::LOW;

  firmius::shared::PermissionResponse chosen =
      firmius::shared::PermissionResponse::Deny;
  firmius::tui::PermissionPromptModal modal(
      request, [&](firmius::shared::PermissionResponse response) {
        chosen = response;
      });

  auto component = modal.create(state);
  const std::string output = RenderModal(component);
  EXPECT_NE(output.find("Tool: Process"), std::string::npos);
  EXPECT_NE(output.find("Command: git status --short"), std::string::npos);
  EXPECT_NE(output.find("Primary command: git"), std::string::npos);
  EXPECT_NE(output.find("Run once: git status --short"), std::string::npos);

  EXPECT_TRUE(component->OnEvent(ftxui::Event::ArrowDown));
  EXPECT_TRUE(component->OnEvent(ftxui::Event::Return));
  EXPECT_EQ(chosen,
            firmius::shared::PermissionResponse::AllowCommandSession);
}

} // namespace
