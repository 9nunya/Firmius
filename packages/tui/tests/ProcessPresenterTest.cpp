#include "tools/ProcessPresenter.hpp"
#include "tools/ProcessPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui;

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
  EXPECT_NE(ansi::strip(lines[0]).find("ls -la"), std::string::npos);
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
  EXPECT_NE(ansi::strip(lines[0]).find("sleep 10"), std::string::npos);
}


// Multi-line bash commands (e.g. python -c "...\n...\n...") used to be
// truncated to the first line because highlightLine() only returned the
// first highlighted line. Every source line of the command must now show
// up across the rendered rows.
TEST(ProcessPresenterTest, ExecuteMultilineCommandPreservesAllLines) {
  ProcessPresenter p;
  ToolCallItem item("call-1", "Process", "agent-1");
  item.setArgs(R"json({"action":"Execute","command":"python -c \"\nimport sys\nprint('hello')\nprint('world')\n\""})json");
  item.setResult(true, R"({"exit_code":0,"duration_ms":12.0,"stdout":"hello\nworld"})");
  auto lines = p.render(item, {}, 120);
  std::string joined;
  for (const auto& l : lines) {
    joined += ansi::strip(l);
    joined += '\n';
  }
  EXPECT_NE(joined.find("python -c"), std::string::npos);
  EXPECT_NE(joined.find("import sys"), std::string::npos);
  EXPECT_NE(joined.find("print('hello')"), std::string::npos);
  EXPECT_NE(joined.find("print('world')"), std::string::npos);
}

// A single very long command must wrap onto continuation rows instead of
// being truncated horizontally — the previous renderer wrapped fine here,
// this guards against the multi-line fix regressing single-line wrapping.
TEST(ProcessPresenterTest, ExecuteLongCommandWrapsToMultipleRows) {
  ProcessPresenter p;
  ToolCallItem item("call-1", "Process", "agent-1");
  std::string longCmd = "echo ";
  for (int i = 0; i < 30; ++i) longCmd += "verylongargument" + std::to_string(i) + " ";
  std::string args =
      std::string(R"({"action":"Execute","command":")") + longCmd + R"("})";
  item.setArgs(args);
  item.setResult(true, R"({"exit_code":0,"duration_ms":1.0,"stdout":""})");
  // Narrow width forces wrapping.
  auto lines = p.render(item, {}, 60);
  // We need at least 2 rendered rows for the wrapped header alone.
  int rowsContainingCommand = 0;
  for (const auto& l : lines) {
    if (ansi::strip(l).find("verylongargument") != std::string::npos) {
      ++rowsContainingCommand;
    }
  }
  EXPECT_GE(rowsContainingCommand, 2);
}
