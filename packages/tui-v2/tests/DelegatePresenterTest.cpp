#include "tools/DelegatePresenter.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(DelegatePresenterTest, MatchesDelegate) {
  DelegatePresenter p;
  EXPECT_TRUE(p.matches("Delegate"));
  EXPECT_FALSE(p.matches("Process"));
}

TEST(DelegatePresenterTest, PreparingPhase) {
  DelegatePresenter p;
  ToolCallItem item("call-1", "Delegate", "agent-1");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
}

TEST(DelegatePresenterTest, SpawnCalled) {
  DelegatePresenter p;
  ToolCallItem item("call-1", "Delegate", "agent-1");
  item.setArgs(R"({"action":"Spawn","title":"Research Agent","persona":"researcher","task":"Find info"})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 2u);
  EXPECT_NE(lines[0].find("Summoning"), std::string::npos);
  EXPECT_NE(lines[0].find("Research Agent"), std::string::npos);
}

TEST(DelegatePresenterTest, SpawnFinishedSuccess) {
  DelegatePresenter p;
  ToolCallItem item("call-1", "Delegate", "agent-1");
  item.setArgs(R"({"action":"Spawn","title":"Research Agent"})");
  item.setResult(true, R"({"status":"completed","result":"Found the answer"})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  bool foundSuccess = false;
  for (const auto& line : lines) {
    if (line.find("\xe2\x9c\x93") != std::string::npos) foundSuccess = true;
  }
  EXPECT_TRUE(foundSuccess);
}

TEST(DelegatePresenterTest, SpawnFinishedError) {
  DelegatePresenter p;
  ToolCallItem item("call-1", "Delegate", "agent-1");
  item.setArgs(R"({"action":"Spawn","title":"Research Agent"})");
  item.setResult(false, R"({"status":"failed","result":"Timeout"})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  bool foundError = false;
  for (const auto& line : lines) {
    if (line.find("\xe2\x9c\x97") != std::string::npos) foundError = true;
  }
  EXPECT_TRUE(foundError);
}

TEST(DelegatePresenterTest, WaitCalled) {
  DelegatePresenter p;
  ToolCallItem item("call-1", "Delegate", "agent-1");
  item.setArgs(R"({"action":"Wait","agent_id":"sub-1"})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("Waiting"), std::string::npos);
}

TEST(DelegatePresenterTest, StopCalled) {
  DelegatePresenter p;
  ToolCallItem item("call-1", "Delegate", "agent-1");
  item.setArgs(R"({"action":"Stop","agent_id":"sub-1"})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
}

TEST(DelegatePresenterTest, StopFinished) {
  DelegatePresenter p;
  ToolCallItem item("call-1", "Delegate", "agent-1");
  item.setArgs(R"({"action":"Stop","agent_id":"sub-1"})");
  item.setResult(true, R"({"status":"terminated"})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("Stopped"), std::string::npos);
}
