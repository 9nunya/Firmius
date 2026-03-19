#include "components/HelpOverlay.hpp"
#include "components/ScrollableBox.hpp"

#include <gtest/gtest.h>

TEST(HelpOverlayTest, AcceptsKeyboardScrollEvents) {
  auto scrollable = firmius::tui::ScrollableBox(
      ftxui::Renderer([] { return ftxui::text("line 1\nline 2\nline 3\nline 4"); }));

  EXPECT_TRUE(scrollable->OnEvent(ftxui::Event::ArrowDown));
  EXPECT_TRUE(scrollable->OnEvent(ftxui::Event::ArrowUp));
  EXPECT_TRUE(scrollable->OnEvent(ftxui::Event::PageDown));
  EXPECT_TRUE(scrollable->OnEvent(ftxui::Event::PageUp));
}

TEST(HelpOverlayTest, ComputesLargeUsableLayout) {
  const auto layout = firmius::tui::ComputeHelpOverlayLayout(140, 50);
  EXPECT_GE(layout.width, 80);
  EXPECT_GE(layout.height, 22);
  EXPECT_EQ(layout.width, 132);
  EXPECT_EQ(layout.height, 44);
}
