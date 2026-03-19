#include "components/StatusBar.hpp"
#include "ThemeManager.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace firmius::tui {
namespace {

TEST(StatusBarTest, RendersProcessPillWhenProcessesExist) {
  auto model = std::make_shared<StatusBarModel>();
  model->status_text = "executing_tool";
  model->agent_name = "Firmius";
  model->model_name = "openai/gpt-5";
  model->live_processes = 1;
  model->background_processes = 2;

  auto component = StatusBar(model);
  auto element = component->Render();

  ftxui::Screen screen(160, 1);
  ftxui::Render(screen, element);
  std::string output = screen.ToString();

  EXPECT_NE(output.find("1 live"), std::string::npos);
  EXPECT_NE(output.find("2 bg"), std::string::npos);
}

} // namespace
} // namespace firmius::tui
