#include "tools/ArtifactsPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(ArtifactsPresenterTest, MatchesArtifacts) {
  ArtifactsPresenter p;
  EXPECT_TRUE(p.matches("Artifacts"));
  EXPECT_FALSE(p.matches("Edit"));
}

TEST(ArtifactsPresenterTest, WriteCalled) {
  ArtifactsPresenter p;
  ToolCallItem item("call-1", "Artifacts", "agent-1");
  item.setArgs(R"({"action":"Write","name":"plan.md","content":"# Plan\nDo stuff"})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("plan.md"), std::string::npos);
}

TEST(ArtifactsPresenterTest, WriteFinished) {
  ArtifactsPresenter p;
  ToolCallItem item("call-1", "Artifacts", "agent-1");
  item.setArgs(R"({"action":"Write","name":"plan.md","content":"# Plan\nDo stuff"})");
  item.setResult(true, R"({"status":"created","reference":"@artifact:owner/plan.md"})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("created"), std::string::npos);
}

TEST(ArtifactsPresenterTest, ReadFinished) {
  ArtifactsPresenter p;
  ToolCallItem item("call-1", "Artifacts", "agent-1");
  item.setArgs(R"({"action":"Read","name":"plan.md"})");
  item.setResult(true, R"({"content":"# Plan"})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
}

TEST(ArtifactsPresenterTest, ListFinished) {
  ArtifactsPresenter p;
  ToolCallItem item("call-1", "Artifacts", "agent-1");
  item.setArgs(R"({"action":"List"})");
  item.setResult(true, R"({"count":3})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("3"), std::string::npos);
}
