#include "AuditCliUtils.hpp"
#include "audits/LspAudit.hpp"
#include <gtest/gtest.h>

TEST(LspAuditCli, CanonicalIdLeavesLspUnchanged) {
  EXPECT_EQ(firmius::audits::cli::canonicalAuditId("lsp"), "lsp");
}

TEST(LspAudit, DescriptionMentionsPreClonedReposTimingAndCleanup) {
  firmius::audits::LspAudit audit;
  const auto description = audit.getDescription();
  EXPECT_NE(description.find("pre-cloned"), std::string::npos);
  EXPECT_NE(description.find("timing"), std::string::npos);
  EXPECT_NE(description.find("cleanup"), std::string::npos);
}
