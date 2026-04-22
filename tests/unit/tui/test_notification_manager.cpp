#include "NotificationManager.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <gtest/gtest.h>

namespace {

using firmius::tui::NotificationManager;

std::string RenderToString(ftxui::Element element, int width, int height) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, element);
  return screen.ToString();
}

TEST(NotificationManagerTest, RenderClearsUnderlaySoUnderlyingTextDoesNotSeep) {
  // Force a deterministic terminal size for NotificationManager's width logic.
  ftxui::Terminal::SetFallbackSize({80, 24});

  // Build a background with a recognizable pattern.
  auto background = ftxui::vbox({
      ftxui::text("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
      ftxui::text("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"),
      ftxui::text("CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"),
      ftxui::text("DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD"),
      ftxui::text("EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE"),
      ftxui::filler(),
  });

  // Ensure we start from a clean slate.
  NotificationManager::instance().dismissAll();
  NotificationManager::instance().notifyInfo("Notice", "");

  auto overlay = NotificationManager::instance().render();
  auto combined = ftxui::dbox({background, overlay});

  // If clear_under is missing, blank lines inside the notification card can
  // reveal characters from background (A/B/C/...) where the card should be blank.
  const auto output = RenderToString(combined, 80, 16);
  EXPECT_EQ(output.find("AAAAAAAA"), std::string::npos);
  EXPECT_EQ(output.find("BBBBBBBB"), std::string::npos);
  EXPECT_EQ(output.find("CCCCCCCC"), std::string::npos);
}

} // namespace
