#include "tools/ProcessPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(ProcessPresenterTest, MatchesProcessAndPython) {
  ProcessPresenter p;
  EXPECT_TRUE(p.matches("Process"));
  EXPECT_TRUE(p.matches("Python"));
  EXPECT_FALSE(p.matches("Edit"));
}

TEST(ProcessPresenterTest, PreparingPhase) {
  ProcessPresenter p;
  ToolCallItem item("call-1", "Process", "agent-1");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
}

TEST(ProcessPresenterTest, ExecuteCalled) {
  ProcessPresenter p;
  ToolCallItem item("call-1", "Process", "agent-1");
  item.setArgs(R"({"action":"Execute","command":"ls -la"})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 2u);
  EXPECT_NE(lines[0].find("ls -la"), std::string::npos);
}

TEST(ProcessPresenterTest, ExecuteFinishedSuccess) {
  ProcessPresenter p;
  ToolCallItem item("call-1", "Process", "agent-1");
  item.setArgs(R"({"action":"Execute","command":"ls"})");
  item.setResult(true, R"({"exit_code":0,"duration_ms":123.4,"stdout":"file1\nfile2"})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  bool foundSuccess = false;
  for (const auto& line : lines) {
    if (line.find("\xe2\x9c\x93") != std::string::npos) foundSuccess = true;
  }
  EXPECT_TRUE(foundSuccess);
}

TEST(ProcessPresenterTest, ExecuteFinishedError) {
  ProcessPresenter p;
  ToolCallItem item("call-1", "Process", "agent-1");
  item.setArgs(R"({"action":"Execute","command":"ls"})");
  item.setResult(false, R"({"exit_code":1,"stderr":"not found"})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  bool foundError = false;
  for (const auto& line : lines) {
    if (line.find("\xe2\x9c\x97") != std::string::npos) foundError = true;
  }
  EXPECT_TRUE(foundError);
}

TEST(ProcessPresenterTest, StatusFinished) {
  ProcessPresenter p;
  ToolCallItem item("call-1", "Process", "agent-1");
  item.setArgs(R"({"action":"Status","process_id":"abc"})");
  item.setResult(true, R"({"is_running":false,"exit_code":0,"duration_ms":500.0})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("exited"), std::string::npos);
}

TEST(ProcessPresenterTest, WaitCalled) {
  ProcessPresenter p;
  ToolCallItem item("call-1", "Process", "agent-1");
  item.setArgs(R"({"action":"Wait","process_id":"abc"})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("Waiting"), std::string::npos);
}

TEST(ProcessPresenterTest, KillCalled) {
  ProcessPresenter p;
  ToolCallItem item("call-1", "Process", "agent-1");
  item.setArgs(R"({"action":"Kill","process_id":"abc"})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
}

TEST(ProcessPresenterTest, ListFinished) {
  ProcessPresenter p;
  ToolCallItem item("call-1", "Process", "agent-1");
  item.setArgs(R"({"action":"List"})");
  item.setResult(true, R"({"count":5})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("5"), std::string::npos);
}

TEST(ProcessPresenterTest, PythonCalled) {
  ProcessPresenter p;
  ToolCallItem item("call-1", "Python", "agent-1");
  item.setArgs(R"json({"code":"print('hello')"})json");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 2u);
  EXPECT_NE(lines[0].find("Python"), std::string::npos);
}

TEST(ProcessPresenterTest, SpawnFinished) {
  ProcessPresenter p;
  ToolCallItem item("call-1", "Process", "agent-1");
  item.setArgs(R"({"action":"Spawn","command":"sleep 10"})");
  item.setResult(true, R"({"process_id":"pid-123","stdout":"output"})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("sleep 10"), std::string::npos);
}
