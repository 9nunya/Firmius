#include "TUIHotkeys.hpp"

#include <gtest/gtest.h>

using namespace firmius::tui;

TEST(TUIHotkeys, PermissionCycleUsesCtrlY) {
  EXPECT_TRUE(IsPermissionCycleInput("\x19"));
  EXPECT_TRUE(IsPermissionCycleEvent(ftxui::Event::Special("\x19")));
}

TEST(TUIHotkeys, OldCtrlShiftPSequenceIsRejected) {
  EXPECT_FALSE(IsPermissionCycleInput("\x1b[1;6P"));
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
