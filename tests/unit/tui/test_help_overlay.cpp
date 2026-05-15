#include "components/HelpOverlay.hpp"
#include "components/ScrollableBox.hpp"

#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace {

void StabilizeScrollable(
    const std::shared_ptr<firmius::tui::ScrollableBoxComponent>& scrollable,
    int width, int height) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  Render(screen, scrollable->Render());
  Render(screen, scrollable->Render());
}

ftxui::Event WheelEvent(ftxui::Mouse::Button button, int x = 0, int y = 0) {
  ftxui::Mouse mouse;
  mouse.x = x; mouse.y = y; mouse.button = button; mouse.motion = ftxui::Mouse::Pressed;
  return ftxui::Event::Mouse("", mouse);
}

}  // namespace

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

TEST(ScrollableBoxRegressionTest, WheelMovesOneLinePerTick) {
  auto scrollable = firmius::tui::ScrollableBox(
      ftxui::Renderer([] {
        return ftxui::vbox({
            ftxui::text("line 1"), ftxui::text("line 2"), ftxui::text("line 3"),
            ftxui::text("line 4"), ftxui::text("line 5"), ftxui::text("line 6"),
            ftxui::text("line 7"), ftxui::text("line 8"),
        });
      }));

  StabilizeScrollable(scrollable, 20, 3);
  ASSERT_EQ(scrollable->ScrollOffset(), 0);

  EXPECT_TRUE(scrollable->OnEvent(WheelEvent(ftxui::Mouse::WheelDown, 1, 1)));
  EXPECT_EQ(scrollable->ScrollOffset(), 1);

  EXPECT_TRUE(scrollable->OnEvent(WheelEvent(ftxui::Mouse::WheelUp, 1, 1)));
  EXPECT_EQ(scrollable->ScrollOffset(), 0);
}

TEST(ScrollableBoxRegressionTest, HeightChangesPreserveManualScrollOffset) {
  int synthetic_height = 40;
  std::size_t measurement_signature = 1;

  auto scrollable = firmius::tui::ScrollableBox(
      ftxui::Renderer([] { return ftxui::text("stable body"); }),
      {.measurement_signature_getter = [&] { return measurement_signature; },
       .custom_size_getter = [&](int) { return synthetic_height; }});

  StabilizeScrollable(scrollable, 20, 5);
  EXPECT_TRUE(scrollable->OnEvent(ftxui::Event::PageDown));
  ASSERT_EQ(scrollable->ScrollOffset(), 3);

  synthetic_height = 80;
  StabilizeScrollable(scrollable, 20, 5);
  EXPECT_EQ(scrollable->ScrollOffset(), 3);
}
