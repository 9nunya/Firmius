#include "NotificationManager.hpp"

#include <ftxui/component/component.hpp>
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

TEST(NotificationLayeringTest, NotificationsAppearAboveModalOverlay) {
  ftxui::Terminal::SetFallbackSize({80, 24});

  NotificationManager::instance().dismissAll();
  NotificationManager::instance().notifyInfo("Top", "Visible");

  auto base = ftxui::text("BASE");

  // Simulate a modal that covers the screen with a repeated label.
  auto modal = ftxui::vbox({
                   ftxui::text("MODALMODALMODALMODALMODALMODALMODALMODALMODAL"),
                   ftxui::text("MODALMODALMODALMODALMODALMODALMODALMODALMODAL"),
                   ftxui::text("MODALMODALMODALMODALMODALMODALMODALMODALMODAL"),
               }) |
               ftxui::bgcolor(ftxui::Color::RGB(20, 20, 20));

  auto notifications = NotificationManager::instance().render();

  // The intended stacking order in the app is:
  // base -> modal overlay -> notifications
  auto combined = ftxui::dbox({base, modal, notifications});

  const auto output = RenderToString(combined, 80, 16);
  EXPECT_NE(output.find("Top"), std::string::npos);
  EXPECT_NE(output.find("Visible"), std::string::npos);
}

} // namespace
