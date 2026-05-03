#include "components/HelpOverlay.hpp"
#include "components/ScrollableBox.hpp"

#include <ftxui/screen/screen.hpp>
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

TEST(HelpOverlayTest, WrappedParagraphMeasurementReachesBottom) {
  auto scrollable = firmius::tui::ScrollableBox(
      ftxui::Renderer([] {
        return ftxui::paragraph(
            "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu");
      }),
      {.startAtBottom = true, .overlayScrollbar = true});

  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(18),
                                      ftxui::Dimension::Fixed(4));
  Render(screen, scrollable->Render());
  Render(screen, scrollable->Render());
  const auto output = screen.ToString();

  EXPECT_NE(output.find("lambda"), std::string::npos);
  EXPECT_NE(output.find("mu"), std::string::npos);
}

TEST(HelpOverlayTest, ResizingViewportKeepsBottomAnchoredWithoutEndKeyRepair) {
  auto scrollable = firmius::tui::ScrollableBox(
      ftxui::Renderer([] {
        return ftxui::paragraph(
                   "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau") |
               ftxui::xflex;
      }),
      {.startAtBottom = true, .overlayScrollbar = true});

  auto tall = ftxui::Screen::Create(ftxui::Dimension::Fixed(22),
                                    ftxui::Dimension::Fixed(7));
  Render(tall, scrollable->Render());
  Render(tall, scrollable->Render());

  auto short_screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(22),
                                            ftxui::Dimension::Fixed(4));
  Render(short_screen, scrollable->Render());
  const auto first_pass = short_screen.ToString();
  Render(short_screen, scrollable->Render());
  const auto second_pass = short_screen.ToString();

  EXPECT_NE(first_pass.find("sigma"), std::string::npos) << first_pass;
  EXPECT_NE(second_pass.find("sigma"), std::string::npos) << second_pass;
}

TEST(HelpOverlayTest, InputUiSectionIncludesCommandPaletteHotkey) {
  const auto items = firmius::tui::BuildHelpItemsForSection("Input + UI");
  const auto it = std::find_if(items.begin(), items.end(), [](const auto &item) {
    return item.description == "Open command palette / launcher";
  });
  ASSERT_NE(it, items.end());
}
