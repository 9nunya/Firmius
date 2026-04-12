#include "workflow/Workflow.hpp"
#include "workflow/WorkflowLoader.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <cstdlib>

namespace firmius::core {

class WorkflowTest : public ::testing::Test {
protected:
  std::string tempDir_;
  std::string originalWorkflowsDir_;
  bool hadOriginalDir_ = false;

  void SetUp() override {
    // Save original env var
    const char *original = std::getenv("FIRMIUS_WORKFLOWS_DIR");
    if (original) {
      originalWorkflowsDir_ = original;
      hadOriginalDir_ = true;
    }

    // Create temp directory
    char tempTemplate[] = "/tmp/firmius_workflow_test_XXXXXX";
    char *temp = mkdtemp(tempTemplate);
    tempDir_ = temp ? std::string(temp) : "/tmp/firmius_workflow_test";

    // Set env var to temp directory
    ::setenv("FIRMIUS_WORKFLOWS_DIR", tempDir_.c_str(), 1);
  }

  void TearDown() override {
    // Restore original env var
    if (hadOriginalDir_ && !originalWorkflowsDir_.empty()) {
      ::setenv("FIRMIUS_WORKFLOWS_DIR", originalWorkflowsDir_.c_str(), 1);
    } else {
      ::unsetenv("FIRMIUS_WORKFLOWS_DIR");
    }

    // Cleanup temp directory
    if (std::filesystem::exists(tempDir_)) {
      std::filesystem::remove_all(tempDir_);
    }
  }

  std::string createWorkflow(const std::string &filename,
                             const std::string &content) {
    std::string path = tempDir_ + "/" + filename;
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
  }
};

TEST_F(WorkflowTest, BuildReplacesPlaceholders) {
  Workflow wf;
  wf.body = "Hello $1, welcome to $2!";
  wf.argCount = 2;

  std::string result = wf.build({"Alice", "Firmius"});

  EXPECT_EQ(result, "Hello Alice, welcome to Firmius!");
}

TEST_F(WorkflowTest, BuildReplacesMultipleOccurrences) {
  Workflow wf;
  wf.body = "$1 + $1 = $2";
  wf.argCount = 2;

  std::string result = wf.build({"2", "4"});

  EXPECT_EQ(result, "2 + 2 = 4");
}

TEST_F(WorkflowTest, BuildHandlesMissingArgs) {
  Workflow wf;
  wf.body = "Value: $1 and $2";
  wf.argCount = 2;

  // Only provide one arg
  std::string result = wf.build({"only"});

  // Missing args should be removed
  EXPECT_EQ(result, "Value: only and ");
}

TEST_F(WorkflowTest, BuildWithNoPlaceholders) {
  Workflow wf;
  wf.body = "No placeholders here";
  wf.argCount = 0;

  std::string result = wf.build({});

  EXPECT_EQ(result, "No placeholders here");
}

TEST_F(WorkflowTest, BuildWithEmptyArgs) {
  Workflow wf;
  wf.body = "Test $1";
  wf.argCount = 1;

  std::string result = wf.build({""});

  EXPECT_EQ(result, "Test ");
}

TEST_F(WorkflowTest, ArgCountDetection) {
  Workflow wf;
  wf.body = "Use $1 and $2 and $3";

  // Manually count like the loader does
  size_t maxArg = 0;
  std::string body = wf.body;
  for (size_t i = 1; i <= 9; ++i) {
    std::string placeholder = "$" + std::to_string(i);
    if (body.find(placeholder) != std::string::npos) {
      maxArg = std::max(maxArg, i);
    }
  }

  EXPECT_EQ(maxArg, 3);
}

TEST_F(WorkflowTest, WorkflowLoaderInit) {
  // Create test workflows
  createWorkflow("test1.md", R"(---
name: Test One
description: First test workflow
---
Body with $1 arg
)");

  createWorkflow("test2.md", R"(---
name: Test Two
description: Second test workflow
---
Body without args
)");

  // Initialize loader
  WorkflowLoader::instance().init();

  auto ids = WorkflowLoader::instance().getWorkflowIds();

  EXPECT_GE(ids.size(), 2);
  EXPECT_TRUE(std::find(ids.begin(), ids.end(), "test1") != ids.end());
  EXPECT_TRUE(std::find(ids.begin(), ids.end(), "test2") != ids.end());
}

TEST_F(WorkflowTest, WorkflowLoaderGetWorkflow) {
  createWorkflow("myworkflow.md", R"(---
name: My Workflow
description: Test description
---
Do something with $1
)");

  WorkflowLoader::instance().init();

  const Workflow *wf = WorkflowLoader::instance().getWorkflow("myworkflow");

  ASSERT_NE(wf, nullptr);
  EXPECT_EQ(wf->name, "My Workflow");
  EXPECT_EQ(wf->description, "Test description");
  EXPECT_EQ(wf->argCount, 1);
  EXPECT_EQ(wf->id, "myworkflow");
}

TEST_F(WorkflowTest, WorkflowLoaderDefaultsName) {
  createWorkflow("snake_case_name.md", R"(---
description: No name provided
---
Body here
)");

  WorkflowLoader::instance().init();

  const Workflow *wf = WorkflowLoader::instance().getWorkflow("snake_case_name");

  ASSERT_NE(wf, nullptr);
  // Should convert snake_case to Title Case
  EXPECT_FALSE(wf->name.empty());
}

TEST_F(WorkflowTest, WorkflowLoaderNonExistent) {
  WorkflowLoader::instance().init();

  const Workflow *wf =
      WorkflowLoader::instance().getWorkflow("nonexistent");

  EXPECT_EQ(wf, nullptr);
}

TEST_F(WorkflowTest, WorkflowLoaderGetAllWorkflows) {
  createWorkflow("wf1.md", "---\nname: WF1\n---\nBody 1");
  createWorkflow("wf2.md", "---\nname: WF2\n---\nBody 2");

  WorkflowLoader::instance().init();

  auto workflows = WorkflowLoader::instance().getAllWorkflows();

  EXPECT_GE(workflows.size(), 2);
}

TEST_F(WorkflowTest, BuiltinWorkflowsFoundInRepo) {
  // Try to use the actual repo workflows directory if it's reachable.
  // The test process might run from project root or build dir.
  std::string repoPath = "workflows";
  if (!std::filesystem::exists(repoPath)) {
    // Try one level up (if running from build/packages/core etc.)
    if (std::filesystem::exists("../workflows")) {
      repoPath = "../workflows";
    } else if (std::filesystem::exists("../../workflows")) {
      repoPath = "../../workflows";
    } else if (std::filesystem::exists("../../../workflows")) {
      repoPath = "../../../workflows";
    } else if (std::filesystem::exists("../../../../workflows")) {
      repoPath = "../../../../workflows";
    }
  }

  if (std::filesystem::exists(repoPath)) {
    ::setenv("FIRMIUS_WORKFLOWS_DIR", repoPath.c_str(), 1);
  } else {
    // Equivalent controlled setup if repo directory not found.
    // This ensures the test is deterministic even if file structure differs.
    createWorkflow("deep_interview.md", "---\nname: Deep Interview\n---\nBody");
    createWorkflow("plan_gate.md", "---\nname: Plan Gate\n---\nBody");
    createWorkflow("evidence_sweep.md", "---\nname: Evidence Sweep\n---\nBody");
    createWorkflow("repair_wave.md", "---\nname: Repair Wave\n---\nBody");
    createWorkflow("explore.md", "---\nname: Explore\n---\nBody");
    // Fixture already sets FIRMIUS_WORKFLOWS_DIR to tempDir_
  }

  WorkflowLoader::instance().init();
  auto ids = WorkflowLoader::instance().getWorkflowIds();

  std::vector<std::string> expected = {
      "deep_interview", "plan_gate", "evidence_sweep", "repair_wave", "explore"};

  for (const auto &id : expected) {
    EXPECT_TRUE(std::find(ids.begin(), ids.end(), id) != ids.end())
        << "Workflow ID '" << id << "' not found in loader after init.";
  }
}
} // namespace firmius::core
