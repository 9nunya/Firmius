#include <gtest/gtest.h>

#include "environment/Environment.hpp"
#include "mocks/MockHost.hpp"

using namespace firmius::core;
using namespace firmius::test;

TEST(EnvironmentTest, CleanupRunsHostCleanupBeforeDestroyAndIsIdempotent) {
  auto host = std::make_shared<MockHost>();
  auto environment = std::make_shared<Environment>(
      host, "/tmp", [](const firmius::shared::StreamEvent &) {});

  environment->cleanup();
  environment->cleanup();

  ASSERT_GE(host->getCalls().size(), 2U);
  EXPECT_EQ(host->getCallCount("cleanup"), 1U);
  EXPECT_EQ(host->getCallCount("destroy"), 1U);
  EXPECT_EQ(host->getCalls()[0].method, "cleanup");
  EXPECT_EQ(host->getCalls()[1].method, "destroy");
}
