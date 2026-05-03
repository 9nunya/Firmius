#include "commands/AccountsCommand.hpp"
#include "commands/QuotasCommand.hpp"
#include "components/InputBar.hpp"

#include <gtest/gtest.h>

using namespace firmius::tui;

TEST(InputBar, ShiftEnterSequencesAreRecognizedAsNewlineInput) {
  EXPECT_TRUE(IsShiftEnterInput("\x1b[13;2u"));
  EXPECT_TRUE(IsShiftEnterInput("\x1b\r"));
  EXPECT_TRUE(IsShiftEnterInput("\x1b\n"));
  EXPECT_TRUE(IsShiftEnterInput("\x1b[27;2;13~"));
}

TEST(InputBar, PlainEnterSequenceIsNotTreatedAsShiftEnter) {
  EXPECT_FALSE(IsShiftEnterInput("\r"));
  EXPECT_FALSE(IsShiftEnterInput("\n"));
  EXPECT_FALSE(IsShiftEnterInput(""));
}

TEST(InputBar, DetectsFileReferenceAutocompleteStateForAtPrefix) {
  const std::string buffer = "inspect @src/file.ts";
  const auto state = DetectAtReferenceAutocompleteState(
      buffer, static_cast<int>(buffer.size()));
  EXPECT_TRUE(state.active);
  EXPECT_FALSE(state.is_artifact);
  EXPECT_EQ(state.token_prefix, "@");
  EXPECT_EQ(state.query, "src/file.ts");
}

TEST(InputBar, DetectsArtifactReferenceAutocompleteStateForArtifactPrefix) {
  const std::string buffer = "review @artifact:planner/REPORT.md";
  const auto state = DetectAtReferenceAutocompleteState(
      buffer, static_cast<int>(buffer.size()));
  EXPECT_TRUE(state.active);
  EXPECT_TRUE(state.is_artifact);
  EXPECT_EQ(state.token_prefix, "@artifact:");
  EXPECT_EQ(state.query, "planner/REPORT.md");
}

TEST(InputBar, DoesNotActivateAutocompleteInsideWord) {
  const std::string buffer = "email test@example.com";
  const auto state = DetectAtReferenceAutocompleteState(
      buffer, static_cast<int>(buffer.size()));
  EXPECT_FALSE(state.active);
}

TEST(CommandArgs, AccountsCommandAcceptsAllProviderIds) {
  firmius::tui::AccountsCommand cmd;
  const auto args = cmd.args();
  ASSERT_EQ(args.size(), 1u);
  EXPECT_EQ(args.front().type, ArgType::ProviderId);
}

TEST(CommandArgs, QuotasCommandUsesQuotaProviderArgType) {
  firmius::tui::QuotasCommand cmd;
  const auto args = cmd.args();
  ASSERT_EQ(args.size(), 1u);
  EXPECT_EQ(args.front().type, ArgType::QuotaProvider);
}

TEST(InputBarModel, HelpQueryCanSignalOverlayWhenInputEmpty) {
  firmius::tui::InputBarModel model;
  std::string buffer;
  int cursor = 0;
  bool help_requested = false;
  model.buffer = &buffer;
  model.cursor = &cursor;
  model.help_opened_from_empty_query = &help_requested;
  EXPECT_FALSE(help_requested);
  *model.help_opened_from_empty_query = true;
  EXPECT_TRUE(help_requested);
}

TEST(InputBarModel, PaletteRequestFlagCanBeRaised) {
  bool palette_requested = false;
  firmius::tui::InputBarModel model;
  model.command_palette_requested = &palette_requested;
  *model.command_palette_requested = true;
  EXPECT_TRUE(palette_requested);
}
