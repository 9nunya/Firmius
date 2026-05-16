#include "KeybindRegistry.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(KeybindRegistryTest, RegisterAndHandle) {
  KeybindRegistry registry;
  bool invoked = false;

  registry.registerKeybind({"a", "Test action", ActivityContext::Idle, false,
                            [&invoked]() { invoked = true; }});

  // Doesn't match wrong key
  EXPECT_FALSE(registry.handleKey("b", ActivityContext::Idle));
  EXPECT_FALSE(invoked);

  // Doesn't match wrong context
  EXPECT_FALSE(registry.handleKey("a", ActivityContext::Active));
  EXPECT_FALSE(invoked);

  // Matches correct key and context
  EXPECT_TRUE(registry.handleKey("a", ActivityContext::Idle));
  EXPECT_TRUE(invoked);
}

TEST(KeybindRegistryTest, AlwaysActive) {
  KeybindRegistry registry;
  bool invoked = false;

  registry.registerKeybind({"ctrl+c", "Quit", ActivityContext::Idle, true,
                            [&invoked]() { invoked = true; }});

  // Even though it's registered for Idle, alwaysActive makes it match Active
  EXPECT_TRUE(registry.handleKey("ctrl+c", ActivityContext::Active));
  EXPECT_TRUE(invoked);
}

TEST(KeybindRegistryTest, ListKeybinds) {
  KeybindRegistry registry;

  registry.registerKeybind({"n", "New", ActivityContext::Idle, false, nullptr});
  registry.registerKeybind({"esc", "Stop", ActivityContext::Active, false, nullptr});
  registry.registerKeybind({"q", "Quit", ActivityContext::Idle, true, nullptr});

  auto idleBinds = registry.listKeybinds(ActivityContext::Idle);
  EXPECT_EQ(idleBinds.size(), 2u); // n and q

  auto activeBinds = registry.listKeybinds(ActivityContext::Active);
  EXPECT_EQ(activeBinds.size(), 2u); // esc and q
}
