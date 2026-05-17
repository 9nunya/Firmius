#include "AccountsOverlay.hpp"

#include <gtest/gtest.h>

using namespace firmius::tui2;

namespace {

MouseEvent leftClick(int row) {
  MouseEvent event;
  event.type = MouseEvent::Type::Press;
  event.button = MouseEvent::Button::Left;
  event.row = row;
  event.col = 1;
  return event;
}

firmius::daemon::AccountSnapshot makeAccount(const std::string& id) {
  firmius::daemon::AccountSnapshot account;
  account.identifier = id;
  return account;
}

} // namespace

TEST(AccountsOverlayTest, ClickSelectsThenTogglesExpansionOnSecondClick) {
  AccountsOverlay overlay;
  overlay.load("openai", {makeAccount("acct-1"), makeAccount("acct-2")}, {}, 100);
  overlay.open();

  auto collapsed = overlay.render(100);
  ASSERT_GE(collapsed.size(), 4u);
  EXPECT_NE(collapsed[2].find("> "), std::string::npos);

  EXPECT_TRUE(overlay.handleMouse(leftClick(4), 2, 1));
  auto selected = overlay.render(100);
  ASSERT_GE(selected.size(), 4u);
  EXPECT_EQ(selected.size(), collapsed.size());

  EXPECT_TRUE(overlay.handleMouse(leftClick(4), 2, 1));
  auto expanded = overlay.render(100);
  EXPECT_GT(expanded.size(), selected.size());
}
