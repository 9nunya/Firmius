#include "components/AgentStrip.hpp"

#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace firmius::tui {
namespace {

TEST(AgentStripTest, RendersModelVariantWhenPresent) {
  auto model = std::make_shared<AgentStripModel>();
  AgentStripItem item;
  item.id = "a-1";
  item.title = "Lead";
  item.purpose = "lead";
  item.model_name = "openai/gpt-5";
  item.model_variant = "thinking";
  item.status_text = "idle";
  item.context_percent = 0.25f;
  item.is_busy = false;
  item.is_focused = true;
  item.tool_call_count = 1;
  model->items.push_back(item);

  auto component = AgentStrip(model);
  auto element = component->Render();

  ftxui::Screen screen(180, 3);
  ftxui::Render(screen, element);
  std::string output = screen.ToString();

  EXPECT_NE(output.find("Thinking"), std::string::npos);
}

} // namespace
} // namespace firmius::tui

