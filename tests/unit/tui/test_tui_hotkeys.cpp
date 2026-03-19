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
