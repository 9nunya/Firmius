#include "workflow/Workflow.hpp"
#include "workflow/WorkflowLoader.hpp"
#include "agents/hooks/HookState.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace firmius::core {
namespace {

class HooksAuditTest : public ::testing::Test {
protected:
  std::string tempDir_;
  std::string oldWorkflowsDir_;
  bool hadWorkflowsDir_ = false;
  std::string oldHome_;
  bool hadHome_ = false;
  std::string oldFirmiusHome_;
  bool hadFirmiusHome_ = false;

  void SetUp() override {
    char tempTemplate[] = "/tmp/firmius_hooks_audit_XXXXXX";
    char *temp = ::mkdtemp(tempTemplate);
    ASSERT_NE(temp, nullptr);
    tempDir_ = temp;
    std::filesystem::create_directories(std::filesystem::path(tempDir_) / "hooks");

    if (const char *v = std::getenv("FIRMIUS_WORKFLOWS_DIR")) {
      oldWorkflowsDir_ = v;
      hadWorkflowsDir_ = true;
    }
    if (const char *v = std::getenv("HOME")) {
      oldHome_ = v;
      hadHome_ = true;
    }
    if (const char *v = std::getenv("FIRMIUS_HOME")) {
      oldFirmiusHome_ = v;
      hadFirmiusHome_ = true;
    }

    ::setenv("FIRMIUS_WORKFLOWS_DIR", tempDir_.c_str(), 1);
    ::setenv("HOME", tempDir_.c_str(), 1);
    ::setenv("FIRMIUS_HOME", tempDir_.c_str(), 1);
  }

  void TearDown() override {
    if (hadWorkflowsDir_)
      ::setenv("FIRMIUS_WORKFLOWS_DIR", oldWorkflowsDir_.c_str(), 1);
    else
      ::unsetenv("FIRMIUS_WORKFLOWS_DIR");
    if (hadHome_)
      ::setenv("HOME", oldHome_.c_str(), 1);
    else
      ::unsetenv("HOME");
    if (hadFirmiusHome_)
      ::setenv("FIRMIUS_HOME", oldFirmiusHome_.c_str(), 1);
    else
      ::unsetenv("FIRMIUS_HOME");
    std::filesystem::remove_all(tempDir_);
  }

  std::string writeWorkflow(const std::string &name, const std::string &body) {
    const auto path = std::filesystem::path(tempDir_) / name;
    std::ofstream out(path);
    out << body;
    out.close();
    return path.string();
  }
};

TEST_F(HooksAuditTest, PromisePackShapeLoadsAndExecutesCoreSurfaces) {
  writeWorkflow(
      "promise_hook.md",
      R"(---
name: Promise Audit
trigger:
  on_event: pre_tool_use
  block: true
  match:
    tool: Files.Edit
action:
  kind: compose
  steps:
    - kind: prompt
      body: "promise {{tool}}"
    - kind: state
      body: noop
emit:
  outcome: reject
defines_tool:
  name: make_pact
  description: audit tool
  required_scope: Semantic
  schema:
    type: object
    properties:
      brief: { type: string }
---
audit body
)");

  WorkflowLoader::instance().init();
  const Workflow *workflow = WorkflowLoader::instance().getWorkflow("promise_hook");
  ASSERT_NE(workflow, nullptr);
  EXPECT_TRUE(workflow->isHook());
  EXPECT_EQ(workflow->trigger.event, WorkflowEventKind::PreToolUse);
  EXPECT_EQ(workflow->action.kind, WorkflowActionKind::Compose);
  EXPECT_EQ(workflow->action.composeSteps.size(), 2u);
  ASSERT_TRUE(workflow->emit.has_value());
  EXPECT_EQ(workflow->emit->outcomeTemplate, "reject");
  ASSERT_TRUE(workflow->definesTool.has_value());
  EXPECT_EQ(workflow->definesTool->name, "make_pact");
}

TEST_F(HooksAuditTest, AuditFlagsCurrentProductionGapsExplicitly) {
  std::vector<std::string> gaps;
  gaps.push_back("Hook pack metadata is descriptive and not enforced as an install capability contract yet");
  gaps.push_back("Hook replay tooling is still basic and should cover more event types");
  gaps.push_back("TUI hook status rendering is script-based, but pack author diagnostics need more UI");
  gaps.push_back("Hook match predicates are still equals-only; regex/jsonpath/expression are not production-ready");

  EXPECT_GE(gaps.size(), 4u);
  EXPECT_NE(gaps[0].find("metadata"), std::string::npos);
}

TEST_F(HooksAuditTest, EndToEndThreadStateInfrastructureWorks) {
  hooks::HookState::instance().bindThread("audit-thread");
  ASSERT_TRUE(hooks::HookState::instance().writeJson(
      hooks::HookState::Scope::Thread, "promise.iteration", "4", "audit.hook"));

  const auto snap = hooks::HookState::instance().snapshotJson("audit.hook");
  EXPECT_NE(snap.find("promise"), std::string::npos);
}

} // namespace
} // namespace firmius::core
