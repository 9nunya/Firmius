#include "artifacts/ReferenceExpansion.hpp"
#include "persistence/ThreadManager.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace firmius::core;

namespace {

class ReferenceExpansionTest : public ::testing::Test {
protected:
  void SetUp() override {
    originalHome_ = std::getenv("HOME") ? std::getenv("HOME") : "";
    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_ref_expand_" +
                 std::to_string(static_cast<long long>(
                     std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(testHome_ / ".firmius" / "threads");
    setenv("HOME", testHome_.c_str(), 1);

    manager_ = std::make_unique<ThreadManager>(
        (testHome_ / ".firmius" / "threads").string());
    threadId_ = createThread();
    cwd_ = testHome_ / "workspace";
    std::filesystem::create_directories(cwd_);
  }

  void TearDown() override {
    manager_.reset();
    std::filesystem::remove_all(testHome_);
    if (originalHome_.empty()) {
      unsetenv("HOME");
    } else {
      setenv("HOME", originalHome_.c_str(), 1);
    }
  }

  std::string createThread() {
    firmius::shared::ThreadMetadata metadata;
    metadata.title = "Reference Expansion Test";
    metadata.cwd = testHome_.string();
    metadata.hostOptions.type = firmius::shared::HostType::Local;
    metadata.leadPersona = "lead";
    return manager_->createThread(metadata);
  }

  void writeManifest(
      const std::map<std::string, AgentManifestEntry> &entries) const {
    manager_->writeAgentManifest(threadId_, entries);
  }

  std::filesystem::path testHome_;
  std::filesystem::path cwd_;
  std::string originalHome_;
  std::unique_ptr<ThreadManager> manager_;
  std::string threadId_;
};

TEST_F(ReferenceExpansionTest, ExpandsArtifactReferenceWithFriendlyName) {
  writeManifest({{"agent-a", {"planner", "", "planner", "Planner", true}}});
  manager_->writeArtifact(threadId_, "agent-a", "planner", "REPORT.md", "alpha");

  const std::string expanded = firmius::core::artifacts::expandInboundReferences(
      threadId_, cwd_.string(), "Review @artifact:planner/REPORT.md");
  EXPECT_NE(expanded.find("<artifact path=\"planner/REPORT.md\">"),
            std::string::npos);
  EXPECT_NE(expanded.find("alpha"), std::string::npos);
}

TEST_F(ReferenceExpansionTest, ExpandsFileReferenceAndLineRange) {
  std::filesystem::create_directories(cwd_ / "src");
  {
    std::ofstream file(cwd_ / "src" / "file.ts");
    file << "line-1\nline-2\nline-3\n";
  }

  const std::string full = firmius::core::artifacts::expandInboundReferences(
      threadId_, cwd_.string(), "Inspect @src/file.ts");
  EXPECT_NE(full.find("<file path=\"src/file.ts\">"), std::string::npos);
  EXPECT_NE(full.find("line-1"), std::string::npos);
  EXPECT_NE(full.find("line-3"), std::string::npos);

  const std::string ranged = firmius::core::artifacts::expandInboundReferences(
      threadId_, cwd_.string(), "Inspect @src/file.ts:2-3");
  EXPECT_NE(ranged.find("<file path=\"src/file.ts\" lines=\"2-3\">"),
            std::string::npos);
  EXPECT_EQ(ranged.find("line-1"), std::string::npos);
  EXPECT_NE(ranged.find("line-2"), std::string::npos);
  EXPECT_NE(ranged.find("line-3"), std::string::npos);
}

TEST_F(ReferenceExpansionTest, FailsForMissingArtifactAndMissingFile) {
  writeManifest({{"agent-a", {"planner", "", "planner", "Planner", true}}});

  EXPECT_THROW(
      firmius::core::artifacts::expandInboundReferences(
          threadId_, cwd_.string(), "Read @artifact:planner/REPORT.md"),
      std::runtime_error);

  EXPECT_THROW(
      firmius::core::artifacts::expandInboundReferences(
          threadId_, cwd_.string(), "Read @src/missing.ts"),
      std::runtime_error);
}

TEST_F(ReferenceExpansionTest, FailsForAmbiguousArtifactAndInvalidSyntax) {
  writeManifest({
      {"agent-a", {"planner", "", "planner", "Planner", true}},
      {"agent-b", {"auditor", "", "auditor", "Auditor", true}},
  });
  manager_->writeArtifact(threadId_, "agent-a", "planner", "REPORT.md", "A");
  manager_->writeArtifact(threadId_, "agent-b", "auditor", "REPORT.md", "B");

  EXPECT_THROW(
      firmius::core::artifacts::expandInboundReferences(
          threadId_, cwd_.string(), "Use @artifact:REPORT.md"),
      std::runtime_error);

  EXPECT_THROW(
      firmius::core::artifacts::expandInboundReferences(
          threadId_, cwd_.string(), "Bad token @artifact:"),
      std::runtime_error);
}

TEST_F(ReferenceExpansionTest, FailsForInvalidOrOutOfBoundsRanges) {
  std::filesystem::create_directories(cwd_ / "src");
  {
    std::ofstream file(cwd_ / "src" / "file.ts");
    file << "line-1\nline-2\n";
  }

  EXPECT_THROW(
      firmius::core::artifacts::expandInboundReferences(
          threadId_, cwd_.string(), "Bad @src/file.ts:3-2"),
      std::runtime_error);
  EXPECT_THROW(
      firmius::core::artifacts::expandInboundReferences(
          threadId_, cwd_.string(), "Bad @src/file.ts:1-20"),
      std::runtime_error);
}

} // namespace
