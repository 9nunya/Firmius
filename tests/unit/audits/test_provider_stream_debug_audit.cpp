#include "audits/ProviderStreamDebugAudit.hpp"

#include <gtest/gtest.h>

using firmius::audits::ProviderStreamDebugAudit;

TEST(ProviderStreamDebugAudit, ResolveModelIdSkipsFlags) {
  EXPECT_TRUE(ProviderStreamDebugAudit::resolveModelIdArg(
                  {"qwen", "--thread-id=abc"})
                  .empty());
}

TEST(ProviderStreamDebugAudit, ResolveModelIdUsesExplicitPositionalModel) {
  EXPECT_EQ(ProviderStreamDebugAudit::resolveModelIdArg(
                {"qwen", "qwen3-coder-flash", "--thread-id=abc"}),
            "qwen3-coder-flash");
}
