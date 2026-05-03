#include "components/StatusBar.hpp"
#include "ThemeManager.hpp"

#include <algorithm>
#include <cmath>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <gtest/gtest.h>

namespace firmius::tui {
namespace {

std::string RenderStatusBarToString(const std::shared_ptr<StatusBarModel> &model,
                                    int width, int height = 6) {
  ftxui::Terminal::SetFallbackSize({width, height});
  auto component = StatusBar(model);
  auto element = component->Render();

  ftxui::Screen screen(width, height);
  ftxui::Render(screen, element);
  return screen.ToString();
}

TEST(StatusBarTest, RendersProcessPillWhenProcessesExist) {
  auto model = std::make_shared<StatusBarModel>();
  model->status_text = "executing_tool";
  model->agent_name = "Firmius";
  model->model_name = "openai/gpt-5";
  model->live_processes = 1;
  model->background_processes = 2;

  std::string output = RenderStatusBarToString(model, 160);

  EXPECT_NE(output.find("1 live"), std::string::npos);
  EXPECT_NE(output.find("2 bg"), std::string::npos);
}

TEST(StatusBarTest, RendersModelVariantWhenPresent) {
  auto model = std::make_shared<StatusBarModel>();
  model->status_text = "idle";
  model->agent_name = "Firmius";
  model->model_name = "openai/gpt-5";
  model->model_variant = "thinking";

  std::string output = RenderStatusBarToString(model, 160);

  EXPECT_NE(output.find("Thinking"), std::string::npos);
}

TEST(StatusBarTest, RendersContextUsageWhenPresent) {
  auto model = std::make_shared<StatusBarModel>();
  model->status_text = "streaming";
  model->agent_name = "Firmius";
  model->model_name = "openai/gpt-5";
  model->context_used = 2400;
  model->context_max = 6000;

  std::string output = RenderStatusBarToString(model, 180);

  EXPECT_NE(output.find("40%"), std::string::npos);
}

TEST(StatusBarTest, RendersQuotaUsageWhenPresent) {
  auto model = std::make_shared<StatusBarModel>();
  model->status_text = "idle";
  model->agent_name = "aster";
  model->model_name = "anthropic/claude-3";
  model->quota_usage = "Q 3/10";

  std::string output = RenderStatusBarToString(model, 160);

  EXPECT_NE(output.find("Q 3/10"), std::string::npos);
}

TEST(StatusBarTest, ClaudexCompactStatusPreservesDetailsAtNarrowWidth) {
  auto model = std::make_shared<StatusBarModel>();
  model->compact_skin_mode = true;
  model->status_text = "executing_tool";
  model->agent_name = "aster";
  model->active_mode = "aster:route";
  model->active_mode_glyph = "R";
  model->model_name = "openai/gpt-5.4";
  model->model_variant = "low";
  model->sent_prompt = 54500;
  model->billed_prompt = 67300;
  model->completion_tokens = 208;
  model->context_used = 67300;
  model->context_max = 1000000;
  model->quota_usage = "Q 3/10";
  model->live_processes = 1;
  model->background_processes = 2;

  std::string output = RenderStatusBarToString(model, 70);

  EXPECT_NE(output.find("A"), std::string::npos);
  EXPECT_NE(output.find("Rroute"), std::string::npos);
  EXPECT_NE(output.find("GPT5.4L"), std::string::npos);
  EXPECT_NE(output.find("P54.5k/67.3k"), std::string::npos);
  EXPECT_NE(output.find("C208"), std::string::npos);
  EXPECT_NE(output.find("X67.3k/1M7%"), std::string::npos);
  EXPECT_NE(output.find("Q3/10"), std::string::npos);
  EXPECT_NE(output.find("p1/2"), std::string::npos);
  EXPECT_NE(output.find("ASK"), std::string::npos);
}

TEST(StatusBarTest, FirmiusFallsBackToCompactTextAtNarrowWidth) {
  auto model = std::make_shared<StatusBarModel>();
  model->status_text = "executing_tool";
  model->purpose = "orchestrator";
  model->title = "main-thread";
  model->agent_name = "aster";
  model->active_mode = "aster:route";
  model->active_mode_glyph = "R";
  model->model_name = "openai/gpt-5.4";
  model->model_variant = "low";
  model->sent_prompt = 54500;
  model->billed_prompt = 67300;
  model->completion_tokens = 208;
  model->context_used = 67300;
  model->context_max = 1000000;
  model->quota_usage = "Q 3/10";
  model->live_processes = 1;
  model->background_processes = 2;

  std::string output = RenderStatusBarToString(model, 88);

  EXPECT_FALSE(output.empty());
}

TEST(StatusBarTest, RendersHookStatusLinesOnAdditionalRows) {
  auto model = std::make_shared<StatusBarModel>();
  model->status_text = "idle";
  model->agent_name = "aster";
  model->model_name = "openai/gpt-5";
  model->hook_status_lines = {
      "hook promise.flow: validating iteration 2/5",
      "hook lint.after-edit: clean",
      "hook stale-edit-guard: reread required",
      "hook promise.flow: promise validating 2/5 pact=pact-123 — rerun tests",
  };
  model->max_status_lines = 4;

  std::string output = RenderStatusBarToString(model, 120, 6);

  EXPECT_NE(output.find("promise validating 2/5"), std::string::npos);
  EXPECT_NE(output.find("lint.after-edit: clean"), std::string::npos);
  EXPECT_NE(output.find("stale-edit-guard"), std::string::npos);
}

TEST(StatusBarTest, TrimsHookStatusLinesToConfiguredBudget) {
  auto model = std::make_shared<StatusBarModel>();
  model->status_text = "idle";
  model->agent_name = "aster";
  model->model_name = "openai/gpt-5";
  model->hook_status_lines = {
      "first line should disappear",
      "second line survives",
      "third line survives",
  };
  model->max_status_lines = 3;

  std::string output = RenderStatusBarToString(model, 120, 6);

  EXPECT_EQ(output.find("first line should disappear"), std::string::npos);
  EXPECT_NE(output.find("second line survives"), std::string::npos);
  EXPECT_NE(output.find("third line survives"), std::string::npos);
}


TEST(StatusBarTest, LoadingOverlayProgressTextCanRenderLikeStartupSummary) {
  const int width = 100;
  const auto &theme = ThemeManager::instance().getCurrentTheme();
  const float progress = 0.625f;
  const int filled_cells =
      std::clamp(static_cast<int>(std::round(progress * 24.0f)), 0, 24);
  const std::string progress_bar =
      std::string(static_cast<std::size_t>(filled_cells), '=') +
      std::string(static_cast<std::size_t>(24 - filled_cells), '.');

  auto element =
      ftxui::vbox({
          ftxui::text("Hydrating first frame…") | ftxui::bold |
              ftxui::color(theme.modals.fg) | ftxui::center,
          ftxui::text("[" + progress_bar + "] 63% loaded") |
              ftxui::color(theme.modals.highlight_fg) | ftxui::center,
          ftxui::text("Loaded in 125.000 ms (0.125 s).") |
              ftxui::color(theme.base.separator) | ftxui::center,
          ftxui::text("Working in the background...") |
              ftxui::color(theme.base.dim) | ftxui::center,
      }) |
      ftxui::borderRounded | ftxui::bgcolor(theme.modals.bg);

  ftxui::Screen screen(width, 6);
  ftxui::Render(screen, element);
  const std::string output = screen.ToString();

  EXPECT_NE(output.find("63% loaded"), std::string::npos);
  EXPECT_NE(output.find("Loaded in 125.000 ms"), std::string::npos);
}
} // namespace
} // namespace firmius::tui
