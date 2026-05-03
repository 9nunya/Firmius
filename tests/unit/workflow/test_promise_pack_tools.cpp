#include "workflow/Workflow.hpp"
#include "workflow/WorkflowLoader.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "tools/ToolRegistry.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace firmius::core {
namespace {

class PromisePackToolRegistrationTest : public ::testing::Test {
protected:
  std::string tempDir_;
  std::string oldWorkflowsDir_;
  bool hadWorkflowsDir_ = false;

  void SetUp() override {
    char tempTemplate[] = "/tmp/firmius-promise-pack-XXXXXX";
    char *temp = ::mkdtemp(tempTemplate);
    ASSERT_NE(temp, nullptr);
    tempDir_ = temp;

    if (const char *v = std::getenv("FIRMIUS_WORKFLOWS_DIR")) {
      oldWorkflowsDir_ = v;
      hadWorkflowsDir_ = true;
    }
    ::setenv("FIRMIUS_WORKFLOWS_DIR", tempDir_.c_str(), 1);
  }

  void TearDown() override {
    if (hadWorkflowsDir_) {
      ::setenv("FIRMIUS_WORKFLOWS_DIR", oldWorkflowsDir_.c_str(), 1);
    } else {
      ::unsetenv("FIRMIUS_WORKFLOWS_DIR");
    }
    std::filesystem::remove_all(tempDir_);
  }

  void writeWorkflow(const std::string &name, const std::string &body) {
    std::ofstream out(std::filesystem::path(tempDir_) / name);
    out << body;
  }
};

TEST_F(PromisePackToolRegistrationTest, RegistersMakePactAndSealPactTools) {
  writeWorkflow(
      "make_pact.md",
      R"(---
id: promise.tool.make_pact

defines_tool:
  name: make_pact
  description: Open a promise.
  required_scope: Semantic
  schema:
    type: object
    required: [brief, done_when]
    properties:
      brief: { type: string }
      done_when:
        type: array
        items: { type: string }

action:
  kind: compose
  steps:
    - kind: state
      writes:
        - { scope: thread, path: promise.id, value: "p-1" }
result:
  return:
    pact_id: "{{state.thread.promise.id}}"
---
)");

  writeWorkflow(
      "seal_pact.md",
      R"(---
id: promise.tool.seal_pact

defines_tool:
  name: seal_pact
  description: Resolve a promise.
  required_scope: Semantic
  schema:
    type: object
    required: [evidence]
    properties:
      evidence:
        type: array
        items: { type: object }

action:
  kind: compose
  steps:
    - kind: prompt
      body: sealing
result:
  return:
    requested: true
---
)");

  WorkflowLoader::instance().init();

  ToolRegistry registry;
  registry.registerWorkflowDefinedTools();

  auto makeMeta = registry.getMetadataFor("make_pact");
  ASSERT_TRUE(makeMeta.has_value());
  EXPECT_EQ(makeMeta->name, "make_pact");

  auto sealMeta = registry.getMetadataFor("seal_pact");
  ASSERT_TRUE(sealMeta.has_value());
  EXPECT_EQ(sealMeta->name, "seal_pact");
}

} // namespace
} // namespace firmius::core
