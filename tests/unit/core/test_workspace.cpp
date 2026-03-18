#include "environment/Workspace.hpp"

#include <gtest/gtest.h>

using namespace firmius::core;

TEST(WorkspaceReadTracking, FullCoverageAcrossSlicesCountsAsFullyRead) {
  Workspace workspace("/tmp/work");
  const std::string path = "/tmp/work/file.txt";

  workspace.recordFileRead(path, 1, 3, false);
  EXPECT_FALSE(workspace.hasFullyReadFile(path));

  workspace.recordFileRead(path, 4, 6, true);
  EXPECT_TRUE(workspace.hasFullyReadFile(path));
}

TEST(WorkspaceReadTracking, MissingGapDoesNotCountAsFullyRead) {
  Workspace workspace("/tmp/work");
  const std::string path = "/tmp/work/file.txt";

  workspace.recordFileRead(path, 1, 2, false);
  workspace.recordFileRead(path, 4, 6, true);

  EXPECT_FALSE(workspace.hasFullyReadFile(path));
}

TEST(WorkspaceReadTracking, OutOfOrderSlicesStillMergeCoverage) {
  Workspace workspace("/tmp/work");
  const std::string path = "/tmp/work/file.txt";

  workspace.recordFileRead(path, 4, 6, true);
  EXPECT_FALSE(workspace.hasFullyReadFile(path));

  workspace.recordFileRead(path, 1, 3, false);
  EXPECT_TRUE(workspace.hasFullyReadFile(path));
}
