#include "components/AgentStrip.hpp"

#include <atomic>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace firmius::tui {
namespace {

std::atomic<int> g_spinner_raf_requests{0};
}

void noteTuiRequestAnimationFrameFromAgentStripSpinner() {
  g_spinner_raf_requests.fetch_add(1, std::memory_order_relaxed);
}

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
  ftxui::Screen screen(180, 3);
  ftxui::Render(screen, component->Render());
  ftxui::Render(screen, component->Render());
  std::string output = screen.ToString();

  EXPECT_NE(output.find("Thinking"), std::string::npos);
}

TEST(AgentStripTest, BusySpinnerRequestsAnimationAtMostOncePerFrameBucket) {
  g_spinner_raf_requests.store(0, std::memory_order_relaxed);

  auto model = std::make_shared<AgentStripModel>();
  AgentStripItem item;
  item.id = "a-1";
  item.title = "Lead";
  item.purpose = "lead";
  item.model_name = "openai/gpt-5";
  item.status_text = "streaming";
  item.is_busy = true;
  item.is_focused = true;
  model->items.push_back(item);

  auto component = AgentStrip(model);
  auto element = component->Render();

  ftxui::Screen screen(180, 3);
  ftxui::Render(screen, element);
  ftxui::Render(screen, component->Render());

  EXPECT_LE(g_spinner_raf_requests.load(std::memory_order_relaxed), 1);
}

} // namespace
} // namespace firmius::tui
