#include "TUIHotkeys.hpp"

#include <algorithm>
#include <gtest/gtest.h>

using namespace firmius::tui;

TEST(TUIHotkeys, PermissionCycleUsesCtrlL) {
  EXPECT_TRUE(IsPermissionCycleInput("\x0C"));
  EXPECT_TRUE(IsPermissionCycleEvent(ftxui::Event::Special("\x0C")));
}

TEST(TUIHotkeys, OldCtrlYSequenceIsRejected) {
  EXPECT_FALSE(IsPermissionCycleInput("\x19"));
  EXPECT_FALSE(IsPermissionCycleEvent(ftxui::Event::Special("\x19")));
}

TEST(TUIHotkeys, RetryLastRequestUsesCtrlR) {
  EXPECT_TRUE(IsRetryLastRequestInput("\x12"));
  EXPECT_TRUE(IsRetryLastRequestEvent(ftxui::Event::Special("\x12")));
}

TEST(TUIHotkeys, VariantCycleUsesCtrlK) {
  EXPECT_TRUE(IsVariantCycleInput("\x0B"));
  EXPECT_TRUE(IsVariantCycleEvent(ftxui::Event::Special("\x0B")));
}

TEST(TUIHotkeys, CtrlVIsNoLongerVariantCycle) {
  EXPECT_FALSE(IsVariantCycleInput("\x16"));
  EXPECT_FALSE(IsVariantCycleEvent(ftxui::Event::Special("\x16")));
}

TEST(TUIHotkeys, CtrlWIsNoLongerVariantCycle) {
  EXPECT_FALSE(IsVariantCycleInput("\x17"));
  EXPECT_FALSE(IsVariantCycleEvent(ftxui::Event::Special("\x17")));
}

TEST(TUIHotkeys, DefaultConfigIncludesHelpAndPaletteBindings) {
  const auto bindings = DefaultHotkeyBindings();
  auto has_action = [&](HotkeyAction action) {
    return std::any_of(bindings.begin(), bindings.end(),
                       [&](const HotkeyBinding &binding) {
                         return binding.action == action;
                       });
  };
  EXPECT_TRUE(has_action(HotkeyAction::OpenHelp));
  EXPECT_TRUE(has_action(HotkeyAction::OpenCommandPalette));
}

TEST(TUIHotkeys, ParsesNaturalLabels) {
  EXPECT_EQ(ParseHotkeyLabel("ctrl + shift + p"),
            std::optional<std::string>("CTRL+SHIFT+P"));
  EXPECT_EQ(ParseHotkeyLabel("Alt-Backspace"),
            std::optional<std::string>("ALT+BACKSPACE"));
  EXPECT_EQ(ParseHotkeyLabel("nonsense"), std::nullopt);
}

TEST(TUIHotkeys, DetectsDuplicateBindingConflicts) {
  const std::vector<HotkeyBinding> bindings = {
      {HotkeyAction::OpenHelp, "F1"},
      {HotkeyAction::OpenCommandPalette, "F1"}};
  const auto conflicts = FindHotkeyConflicts(bindings);
  ASSERT_EQ(conflicts.size(), 1u);
  EXPECT_EQ(conflicts.front().label, "F1");
  EXPECT_EQ(conflicts.front().actions.size(), 2u);
}

TEST(TUIHotkeys, ActionDescriptionUsesMetadata) {
  EXPECT_EQ(HotkeyActionDescription(HotkeyAction::OpenCommandPalette),
            "Open command palette / launcher");
}

TEST(TUIHotkeys, ReloadHotkeyConfigReturnsWarningsVector) {
  std::vector<std::string> warnings;
  EXPECT_TRUE(ReloadHotkeyConfig(&warnings));
}
