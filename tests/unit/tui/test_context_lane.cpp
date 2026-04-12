#include "components/ContextLane.hpp"
#include "Enums.hpp"
#include "ThemeManager.hpp"

#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace firmius::tui {
namespace {

std::shared_ptr<ContextLaneModel> makeBaseModel() {
  auto model = std::make_shared<ContextLaneModel>();
  model->visible = true;
  model->model_label = "antigravity/gemini-3-flash";
  model->account_label = "scout@example.com";
  model->quota_label = "󱑂 40%";
  model->context_ratio = 0.4f;
  model->context_label = "2.4k/6k";
  model->usage_label = "↑2.4k/1.8k  ↓320";
  model->cost_label = "$0.0137";
  model->bucket_labels.push_back("system_prompt 900");
  model->bucket_labels.push_back("conversation_history 1.5k");
  return model;
}

std::string render(const std::shared_ptr<ContextLaneModel> &model, int width,
                   int height) {
  auto component = ContextLane(model);
  ftxui::Screen screen(width, height);
  ftxui::Render(screen, component->Render());
  ftxui::Render(screen, component->Render());
  return screen.ToString();
}

TEST(ContextLaneTest, RendersCompactSummary) {
  const std::string output = render(makeBaseModel(), 120, 8);
  EXPECT_NE(output.find("scout"), std::string::npos);
  EXPECT_NE(output.find("2.4k"), std::string::npos);
  EXPECT_NE(output.find("40%"), std::string::npos);
  EXPECT_NE(output.find("2.4k/6k"), std::string::npos);
  EXPECT_NE(output.find("system_prompt"), std::string::npos);
}

TEST(ContextLaneTest, RendersStructuredRollingMemorySurface) {
  auto model = makeBaseModel();
  model->rolling_memory.enabled = true;
  model->rolling_memory.mode_label = "rolling_forever";
  model->rolling_memory.preset_label = "aggressive";
  model->rolling_memory.model_label = "codex/gpt-5.4-mini";
  model->rolling_memory.active_observations = 2;
  model->rolling_memory.active_reflections = 1;
  model->rolling_memory.buffered_observations = 3;
  model->rolling_memory.context_occupancy_ratio = 0.58f;
  model->rolling_memory.buffer_threshold_ratio = 0.45f;
  model->rolling_memory.target_threshold_ratio = 0.57f;
  model->rolling_memory.emergency_threshold_ratio = 0.66f;
  model->rolling_memory.source_tokens = 128000;
  model->rolling_memory.summary_tokens = 52000;
  model->rolling_memory.saved_tokens = 76000;
  model->rolling_memory.retained_tail_tokens = 24000;

  const std::string output = render(model, 140, 14);
  EXPECT_NE(output.find("rolling_forever"), std::string::npos);
  EXPECT_NE(output.find("aggressive"), std::string::npos);
  EXPECT_NE(output.find("gpt-5.4-mini"), std::string::npos);
  EXPECT_NE(output.find("obs 2"), std::string::npos);
  EXPECT_NE(output.find("refl 1"), std::string::npos);
  EXPECT_NE(output.find("buf 3"), std::string::npos);
  EXPECT_NE(output.find("rail"), std::string::npos);
  EXPECT_NE(output.find("B45 T57 E66"), std::string::npos);
  EXPECT_NE(output.find("src 128k"), std::string::npos);
  EXPECT_NE(output.find("sum 52k"), std::string::npos);
  EXPECT_NE(output.find("saved 76k"), std::string::npos);
  EXPECT_NE(output.find("tail 24k"), std::string::npos);
}

TEST(ContextLaneTest, RendersInFlightRollingIndicators) {
  auto model = makeBaseModel();
  model->rolling_memory.enabled = true;
  model->rolling_memory.mode_label = "rolling_forever";
  model->rolling_memory.observation_in_flight = true;
  model->rolling_memory.reflection_in_flight = true;

  const std::string output = render(model, 120, 12);
  EXPECT_NE(output.find("obs+refl in-flight"), std::string::npos);
}

TEST(ContextLaneTest, HiddenModelRendersNothing) {
  auto model = makeBaseModel();
  model->visible = false;
  const std::string output = render(model, 120, 8);
  EXPECT_TRUE(output.find("scout") == std::string::npos);
}

TEST(ContextLaneTest, RendersWithoutQuotaLabel) {
  auto model = makeBaseModel();
  model->quota_label.clear();
  const std::string output = render(model, 120, 8);
  EXPECT_NE(output.find("scout"), std::string::npos);
  EXPECT_NE(output.find("2.4k/6k"), std::string::npos);
}

TEST(ContextLaneTest, RendersWithoutModelLabel) {
  auto model = makeBaseModel();
  model->model_label.clear();
  const std::string output = render(model, 120, 8);
  EXPECT_NE(output.find("scout"), std::string::npos);
}

TEST(ContextLaneTest, RendersContextMeterAtLowUsage) {
  auto model = makeBaseModel();
  model->context_ratio = 0.1f;
  const std::string output = render(model, 120, 8);
  EXPECT_NE(output.find("10%"), std::string::npos);
}

TEST(ContextLaneTest, RendersContextMeterAtHighUsage) {
  auto model = makeBaseModel();
  model->context_ratio = 0.92f;
  const std::string output = render(model, 120, 8);
  EXPECT_NE(output.find("92%"), std::string::npos);
}

TEST(ContextLaneTest, IncludesUsageAndCostText) {
  const std::string output = render(makeBaseModel(), 120, 8);
  EXPECT_NE(output.find("↑2.4k"), std::string::npos);
  EXPECT_NE(output.find("↓320"), std::string::npos);
}

TEST(ContextLaneTest, HandlesEmptyBucketList) {
  auto model = makeBaseModel();
  model->bucket_labels.clear();
  const std::string output = render(model, 120, 8);
  EXPECT_NE(output.find("2.4k/6k"), std::string::npos);
}

TEST(ContextLaneTest, FallsBackToLegacyMemoryLabelsWhenStructuredDisabled) {
  auto model = makeBaseModel();
  model->bucket_labels.clear();
  model->memory_labels = {"mode rolling_forever", "active refl 0 · obs 1"};
  const std::string output = render(model, 120, 10);
  EXPECT_NE(output.find("mode rolling_forever"), std::string::npos);
  EXPECT_NE(output.find("obs 1"), std::string::npos);
}

} // namespace
} // namespace firmius::tui
