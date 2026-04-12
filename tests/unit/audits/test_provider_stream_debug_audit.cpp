#include "AuditCliUtils.hpp"
#include "audits/ProviderStreamDebugAudit.hpp"

#include <gtest/gtest.h>

using firmius::audits::ProviderStreamDebugAudit;

TEST(AuditCliUtils, ProviderLiveAgentAliasInjectsLiveFlag) {
  const auto args = firmius::audits::cli::normalizeAuditArgs(
      "provider_live_agent", {"-x", "codex", "-m", "gpt-5.4"});
  ASSERT_FALSE(args.empty());
  EXPECT_EQ(args.front(), "--live-agent");
}

TEST(AuditCliUtils, ProviderStreamDebugAliasDoesNotInjectLiveFlag) {
  const auto args = firmius::audits::cli::normalizeAuditArgs(
      "provider_stream_debug", {"codex", "gpt-5.4"});
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(args[0], "codex");
}

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

TEST(ProviderStreamDebugAudit, ResolveModelIdSkipsLiveAgentShortFlags) {
  EXPECT_EQ(ProviderStreamDebugAudit::resolveModelIdArg(
                {"codex", "--live-agent", "-p", "planner", "-x", "codex", "-m",
                 "gpt-5.4", "-v", "xhigh", "-C", "/tmp", "-f", "prompt.md"}),
            "");
}

TEST(ProviderStreamDebugAudit, ResolveModelIdSkipsLongFormModelVariantFlag) {
  EXPECT_EQ(ProviderStreamDebugAudit::resolveModelIdArg(
                {"codex", "gpt-5.4", "--model-variant", "xhigh",
                 "--thread-id=abc"}),
            "gpt-5.4");
}
