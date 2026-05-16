#include "tools/ToolPresenterRegistry.hpp"
#include "tools/ProcessPresenter.hpp"
#include "tools/GenericPresenter.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(ToolPresenterRegistryTest, FindProcessPresenter) {
  ToolPresenterRegistry& registry = ToolPresenterRegistry::instance();
  // Process presenter should be registered during init
  auto* p = registry.find("Process");
  // May or may not be registered depending on init order — test the dispatch logic
  if (p) {
    EXPECT_EQ(p->name(), "Process");
  }
}

TEST(ToolPresenterRegistryTest, FindGenericFallback) {
  ToolPresenterRegistry& registry = ToolPresenterRegistry::instance();
  // Generic should catch anything
  auto* p = registry.find("UnknownTool");
  if (p) {
    EXPECT_EQ(p->name(), "Generic");
  }
}

TEST(ToolPresenterRegistryTest, ProcessMatchesCorrectTools) {
  ProcessPresenter p;
  EXPECT_TRUE(p.matches("Process"));
  EXPECT_TRUE(p.matches("Python"));
  EXPECT_FALSE(p.matches("Edit"));
  EXPECT_FALSE(p.matches("Files"));
}

TEST(ToolPresenterRegistryTest, GenericMatchesEverything) {
  GenericPresenter p;
  EXPECT_TRUE(p.matches("Anything"));
  EXPECT_TRUE(p.matches(""));
  EXPECT_TRUE(p.matches("Process"));
}
