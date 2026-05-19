#include "tools/FleetPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui;

TEST(FleetPresenterTest, MatchesFleet) {
  FleetPresenter p;
  EXPECT_TRUE(p.matches("Fleet"));
  EXPECT_FALSE(p.matches("Edit"));
}

TEST(FleetPresenterTest, LockAcquireCalled) {
  FleetPresenter p;
  ToolCallItem item("call-1", "Fleet", "agent-1");
  item.setArgs(R"({"action":"Lock","mode":"acquire","paths":["foo.cpp"]})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("Acquiring"), std::string::npos);
}

TEST(FleetPresenterTest, LockAcquireFinished) {
  FleetPresenter p;
  ToolCallItem item("call-1", "Fleet", "agent-1");
  item.setArgs(R"({"action":"Lock","mode":"acquire","paths":["foo.cpp"],"lock_id":"lock-1"})");
  item.setResult(true, R"({"lock_id":"lock-1"})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("lock-1"), std::string::npos);
}

TEST(FleetPresenterTest, RespondAccepted) {
  FleetPresenter p;
  ToolCallItem item("call-1", "Fleet", "agent-1");
  item.setArgs(R"({"action":"Respond","accept":true})");
  item.setResult(true, R"({"accepted":true})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("Accepted"), std::string::npos);
}

TEST(FleetPresenterTest, StatusFinished) {
  FleetPresenter p;
  ToolCallItem item("call-1", "Fleet", "agent-1");
  item.setArgs(R"({"action":"Status"})");
  item.setResult(true, R"({"locks":[{"id":"l1"},{"id":"l2"}]})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("2"), std::string::npos);
}
