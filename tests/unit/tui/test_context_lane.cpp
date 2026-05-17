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

} // namespace
} // namespace firmius::tui
