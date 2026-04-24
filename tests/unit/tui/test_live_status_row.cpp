#include "components/LiveStatusRow.hpp"
#include "ThemeManager.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace firmius::tui {
namespace {

ftxui::Screen RenderLiveRowScreen(const LiveStatusRowModel &model,
                                  int width = 120, int height = 4) {
  ftxui::Screen screen(width, height);
  ftxui::Render(screen, RenderLiveStatusRow(model));
  return screen;
}

std::pair<int, int> FindText(const ftxui::Screen &screen,
                             const std::string &needle) {
  for (int y = 0; y < screen.dimy(); ++y) {
    std::string line;
    for (int x = 0; x < screen.dimx(); ++x) {
      line += screen.PixelAt(x, y).character;
    }
    const auto pos = line.find(needle);
    if (pos == std::string::npos) {
      continue;
    }
    int x_pos = 0;
    int current_pos = 0;
    while (current_pos < static_cast<int>(pos) && x_pos < screen.dimx()) {
      current_pos += screen.PixelAt(x_pos, y).character.length();
      x_pos++;
    }
    return {x_pos, y};
  }
  return {-1, -1};
}

TEST(LiveStatusRowTest, SpinnerRemainsVisibleDuringPhraseTransition) {
  LiveStatusRowModel model;
  model.skin = defaultSkinConfig(SkinKind::Claudex);
  model.focused_agent_id = "aster";
  model.busy = true;
  model.activity = "thinking";
  model.phrase_transition_active = true;
  model.phrase_transition_t = 0.5f;
  model.phrase_prev = "previous phrase";
  model.phrase_next = "next phrase";

  auto screen = RenderLiveRowScreen(model);
  const std::string output = screen.ToString();
  EXPECT_TRUE(output.find("✦") != std::string::npos ||
              output.find("✧") != std::string::npos ||
              output.find("⋆") != std::string::npos);
}

TEST(LiveStatusRowTest, FadeInUsesPerCharacterColorProgression) {
  LiveStatusRowModel model;
  model.skin = defaultSkinConfig(SkinKind::Claudex);
  model.focused_agent_id = "aster";
  model.busy = true;
  model.activity = "thinking";
  model.phrase_transition_active = true;
  model.phrase_transition_t = 0.70f;
  model.phrase_prev = "old";
  model.phrase_next = "ABCDE";

  auto screen = RenderLiveRowScreen(model);
  auto [x, y] = FindText(screen, "ABCDE");
  ASSERT_GE(x, 0);
  const auto &theme = ThemeManager::instance().getCurrentTheme();

  EXPECT_NE(screen.PixelAt(x, y).foreground_color, theme.base.bg);
  EXPECT_EQ(screen.PixelAt(x + 4, y).foreground_color, theme.base.bg);
}

} // namespace
} // namespace firmius::tui
