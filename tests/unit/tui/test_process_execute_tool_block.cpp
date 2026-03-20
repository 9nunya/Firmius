#include "components/ProcessExecuteToolBlock.hpp"
#include "ThemeManager.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace firmius::tui {
namespace {

std::string render(const std::shared_ptr<ToolCallView> &view) {
  auto component = ProcessExecuteToolBlock(view);
  auto element = component->Render();
  ftxui::Screen screen(120, 20);
  ftxui::Render(screen, element);
  return screen.ToString();
}

TEST(ProcessExecuteToolBlockTest, RunningWithoutOutputShowsAwaitingState) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_execute";
  view->args = R"({"command":"sleep 1"})";
  view->phase = ToolPhase::Called;

  std::string output = render(view);
  EXPECT_NE(output.find("running, awaiting output"), std::string::npos);
  EXPECT_NE(output.find("sleep 1"), std::string::npos);
}

TEST(ProcessExecuteToolBlockTest, BackgroundRunningShowsTransitionAndOutput) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_execute";
  view->args = R"({"command":"tail -f server.log"})";
  view->phase = ToolPhase::BackgroundRunning;
  view->success = true;
  view->process_is_background = true;
  view->live_process_output = "ready\nline 2\n";

  std::string output = render(view);
  EXPECT_NE(output.find("moved to background"), std::string::npos);
  EXPECT_NE(output.find("ready"), std::string::npos);
}

TEST(ProcessExecuteToolBlockTest, LiveOutputDoesNotShowExit) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_execute";
  view->args = R"({"command":"sleep 1"})";
  view->phase = ToolPhase::Called;
  view->live_process_output = "tick\n";
  view->process_exit_known = true;
  view->process_exit_code = 0;

  std::string output = render(view);
  EXPECT_NE(output.find("tick"), std::string::npos);
  EXPECT_NE(output.find("live"), std::string::npos);
  EXPECT_EQ(output.find("[exit"), std::string::npos);
}

TEST(ProcessExecuteToolBlockTest, ProcessSpawnRendersLiveBackgroundOutput) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_spawn";
  view->args = R"({"command":"tail -f app.log"})";
  view->phase = ToolPhase::BackgroundRunning;
  view->success = true;
  view->process_is_background = true;
  view->live_process_output = "line 1\n";
  view->result = R"({"process_id":"proc-1"})";

  std::string output = render(view);
  EXPECT_NE(output.find("tail -f app.log"), std::string::npos);
  EXPECT_NE(output.find("line 1"), std::string::npos);
  EXPECT_NE(output.find("moved to background"), std::string::npos);
}

} // namespace
} // namespace firmius::tui
