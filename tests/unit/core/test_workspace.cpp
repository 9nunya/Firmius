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

TEST(WorkspaceReadTracking, isLineReadIdentifiesCorrectRanges) {
  Workspace workspace("/tmp/work");
  const std::string path = "/tmp/work/file.txt";

  workspace.recordFileRead(path, 10, 20, false);
  EXPECT_TRUE(workspace.isLineRead(path, 10));
  EXPECT_TRUE(workspace.isLineRead(path, 15));
  EXPECT_TRUE(workspace.isLineRead(path, 20));
  EXPECT_FALSE(workspace.isLineRead(path, 9));
  EXPECT_FALSE(workspace.isLineRead(path, 21));
}

TEST(WorkspaceReadTracking, recordFileEditInvalidatesRanges) {
  Workspace workspace("/tmp/work");
  const std::string path = "/tmp/work/file.txt";

  workspace.recordFileRead(path, 1, 10, true);
  EXPECT_TRUE(workspace.hasFullyReadFile(path));
  EXPECT_TRUE(workspace.isLineRead(path, 5));

  workspace.recordFileEdit(path);
  EXPECT_FALSE(workspace.hasFullyReadFile(path));
  EXPECT_FALSE(workspace.isLineRead(path, 5));
  EXPECT_FALSE(workspace.hasReadFile(path));
}
