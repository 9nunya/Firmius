#include "TUIState.hpp"
#include "commands/CommandManager.hpp"
#include "commands/McpCommand.hpp"
#include "modals/IModal.hpp"
#include "modals/ModalRegistry.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <gtest/gtest.h>

namespace firmius::tui {

TuiState &TuiState::instance() {
  static TuiState state;
  return state;
}

TuiState::TuiState() = default;

void TuiState::openModal(const std::string &name) {
  ModalRegistry::instance().openModal(name, *this);
}

void TuiState::openModalDirect(ftxui::Component modal) {
  modals_.push_back(std::move(modal));
}

void TuiState::clearModals() { modals_.clear(); }

} // namespace firmius::tui

namespace {

using firmius::tui::CommandCtx;
using firmius::tui::CommandManager;
using firmius::tui::McpCommand;
using firmius::tui::TuiState;

class ProbeMcpModal final : public firmius::tui::IModal {
public:
  explicit ProbeMcpModal(bool &created) : created_(created) {}

  std::string name() const override { return "mcp"; }

  ftxui::Component create(firmius::tui::TuiState &state) override {
    (void)state;
    created_ = true;
    return ftxui::Renderer([] { return ftxui::text("probe"); });
  }

private:
  bool &created_;
};

TEST(CommandManagerTest, McpAutocompleteIncludesExactCommandMatch) {
  auto &manager = CommandManager::instance();
  manager.registerCommand(std::make_shared<McpCommand>());

  const auto partial = manager.getAutocomplete("/mc");
  ASSERT_TRUE(partial.has_value());
  ASSERT_TRUE(partial->is_typing_command_name);

  bool found_partial = false;
  for (const auto &match : partial->command_matches) {
    if (match.name == "mcp") {
      found_partial = true;
      EXPECT_FALSE(match.is_exact);
    }
  }
  EXPECT_TRUE(found_partial);

  const auto exact = manager.getAutocomplete("/mcp");
  ASSERT_TRUE(exact.has_value());
  ASSERT_TRUE(exact->is_typing_command_name);

  bool found_exact = false;
  for (const auto &match : exact->command_matches) {
    if (match.name == "mcp") {
      found_exact = true;
      EXPECT_TRUE(match.is_exact);
    }
  }
  EXPECT_TRUE(found_exact);
}

TEST(CommandManagerTest, McpCommandDispatchInvokesRegisteredMcpModalCreate) {
  auto &manager = CommandManager::instance();
  manager.registerCommand(std::make_shared<McpCommand>());

  bool modal_created = false;
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<ProbeMcpModal>(modal_created));

  auto &state = TuiState::instance();
  state.clearModals();

  CommandCtx ctx{&state};
  EXPECT_TRUE(manager.executeCommand(ctx, "/mcp"));
  EXPECT_TRUE(modal_created);

  state.clearModals();
}

class ProbeConfigModal final : public firmius::tui::IModal {
public:
  explicit ProbeConfigModal(bool &created) : created_(created) {}

  std::string name() const override { return "config_display"; }

  ftxui::Component create(firmius::tui::TuiState &state) override {
    (void)state;
    created_ = true;
    return ftxui::Renderer([] { return ftxui::text("config-probe"); });
  }

private:
  bool &created_;
};

TEST(CommandManagerTest, ConfigCommandDispatchInvokesRegisteredConfigModal) {
  auto &manager = CommandManager::instance();
  manager.registerCommand(std::make_shared<firmius::tui::ConfigCommand>());

  bool modal_created = false;
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<ProbeConfigModal>(modal_created));

  auto &state = TuiState::instance();
  state.clearModals();

  CommandCtx ctx{&state};
  EXPECT_TRUE(manager.executeCommand(ctx, "/config"));
  EXPECT_TRUE(modal_created);

  state.clearModals();
}

} // namespace
