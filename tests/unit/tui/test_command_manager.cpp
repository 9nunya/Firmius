#include "TUIState.hpp"
#include "NotificationManager.hpp"
#include "commands/CommandManager.hpp"
#include "commands/ConfigCommand.hpp"
#include "commands/McpCommand.hpp"
#include "modals/IModal.hpp"
#include "modals/ModalRegistry.hpp"
#include "TUIHotkeys.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <gtest/gtest.h>
#include <stdexcept>

namespace firmius::tui {

TuiState &TuiState::instance() {
  static TuiState state;
  return state;
}

TuiState::TuiState() = default;

void noteTuiModalOpenRequested(const std::string &name) {
  (void)name;
}

void TuiState::openModal(const std::string &name) {
  ModalRegistry::instance().openModal(name, *this);
}

void TuiState::openModalDirect(ftxui::Component modal,
                               const std::string &profileKey) {
  (void)profileKey;
  modals_.push_back(std::move(modal));
}

void TuiState::clearModals() { modals_.clear(); }

NotificationManager &NotificationManager::instance() {
  static NotificationManager manager;
  return manager;
}

std::string NotificationManager::notifyError(const std::string &, const std::string &,
                                            bool) {
  return {};
}

} // namespace firmius::tui

namespace {

using firmius::tui::CommandCtx;
using firmius::tui::CommandManager;
using firmius::tui::McpCommand;
using firmius::tui::ConfigCommand;
using firmius::tui::HotkeyAction;
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

class ThrowingCommand final : public firmius::tui::ICommand {
public:
  std::string name() const override { return "explode"; }
  std::string description() const override { return "throws"; }
  std::vector<firmius::tui::CommandArg> args() const override { return {}; }
  void execute(firmius::tui::CommandCtx &,
               const std::vector<firmius::tui::ParsedArg> &) override {
    throw std::runtime_error("boom");
  }
};

TEST(CommandManagerTest, ConfigCommandDispatchInvokesRegisteredConfigModal) {
  auto &manager = CommandManager::instance();
  manager.registerCommand(std::make_shared<ConfigCommand>());

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

TEST(CommandManagerTest, ConfigCommandBindingHintsExposePaletteAndEditor) {
  ConfigCommand command;
  const auto hints = command.bindingHints();
  ASSERT_GE(hints.size(), 3u);

  EXPECT_EQ(hints[0].label, GetHotkeyLabel(HotkeyAction::PermissionCycle));
  EXPECT_EQ(hints[1].label,
            GetHotkeyLabel(HotkeyAction::OpenCommandPalette));
  EXPECT_EQ(hints[2].label, "/config → keybindings");
}

TEST(CommandManagerTest, CommandPaletteEntriesExposeBindingHints) {
  auto &manager = CommandManager::instance();
  manager.registerCommand(std::make_shared<ConfigCommand>());
  const auto entries = manager.listCommands();
  const auto it = std::find_if(entries.begin(), entries.end(), [](const auto &entry) {
    return entry.name == "config";
  });
  ASSERT_NE(it, entries.end());
  EXPECT_GE(it->binding_hints.size(), 3u);
}

TEST(CommandManagerTest, ExecuteCommandCatchesCommandExceptions) {
  auto &manager = CommandManager::instance();
  manager.registerCommand(std::make_shared<ThrowingCommand>());

  auto &state = TuiState::instance();
  CommandCtx ctx{&state};
  EXPECT_TRUE(manager.executeCommand(ctx, "/explode"));
}

} // namespace
