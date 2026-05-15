#include "workflow/Workflow.hpp"
#include "workflow/WorkflowLoader.hpp"
#include "agents/hooks/HookEnvelope.hpp"
#include "agents/hooks/TemplateEngine.hpp"

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
    const char *original = std::getenv("FIRMIUS_WORKFLOWS_DIR");
    if (original) {
      originalWorkflowsDir_ = original;
      hadOriginalDir_ = true;
    }

    char tempTemplate[] = "/tmp/firmius_workflow_test_XXXXXX";
    char *temp = mkdtemp(tempTemplate);
    tempDir_ = temp ? std::string(temp) : "/tmp/firmius_workflow_test";

    ::setenv("FIRMIUS_WORKFLOWS_DIR", tempDir_.c_str(), 1);
  }

  void TearDown() override {
    if (hadOriginalDir_ && !originalWorkflowsDir_.empty()) {
      ::setenv("FIRMIUS_WORKFLOWS_DIR", originalWorkflowsDir_.c_str(), 1);
    } else {
      ::unsetenv("FIRMIUS_WORKFLOWS_DIR");
    }

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

  std::string result = wf.build({"only"});

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

  size_t maxArg = 0;
  std::string body = wf.body;
  for (size_t i = 1; i <= 9; ++i) {
    std::string placeholder = "$" + std::to_string(i);
    if (body.find(placeholder) != std::string::npos) {
      maxArg = std::max(maxArg, i);
    }
  }

  EXPECT_EQ(maxArg, 3u);
}

TEST_F(WorkflowTest, LoadsHookPlatformFieldsFromMarkdown) {
  createWorkflow(
      "hook_test.md",
      R"(---
name: Hook Test
description: Verify hook platform YAML loads
trigger:
  on_event: pre_tool_use
  block: true
  match:
    tool: Files.Edit
    persona: coder
action:
  kind: state
  writes:
    - { scope: thread, path: promise.iteration, value: "{{tool.args.value}}" }
emit:
  outcome: "reject"
defines_tool:
  name: make_pact
  description: user space pact tool
  required_scope: Semantic
  schema:
    type: object
    properties:
      brief: { type: string }
---
Body
)");

  WorkflowLoader::instance().init();
  const Workflow *wf = WorkflowLoader::instance().getWorkflow("hook_test");
  ASSERT_NE(wf, nullptr);
  EXPECT_TRUE(wf->isHook());
  EXPECT_EQ(wf->trigger.event, WorkflowEventKind::PreToolUse);
  ASSERT_EQ(wf->action.kind, WorkflowActionKind::State);
  ASSERT_EQ(wf->action.stateWrites.size(), 1u);
  EXPECT_EQ(wf->action.stateWrites[0].path, "promise.iteration");
  ASSERT_TRUE(wf->emit.has_value());
  EXPECT_EQ(wf->emit->outcomeTemplate, "reject");
  ASSERT_TRUE(wf->definesTool.has_value());
  EXPECT_EQ(wf->definesTool->name, "make_pact");
  EXPECT_NE(wf->definesTool->schemaJson.find("brief"), std::string::npos);
}

TEST_F(WorkflowTest, LoadsStructuredMatchPredicatesFromMarkdown) {
  createWorkflow(
      "hook_match_structured.md",
      R"(---
name: Hook Match Structured
description: structured match predicates parse
trigger:
  on_event: pre_tool_use
  block: true
  match:
    tool: { equals: Files.Edit }
    persona: { present: true }
    custom: { present: false }
action:
  kind: state
  writes:
    - { scope: thread, path: promise.iteration, value: "{{tool.args.value}}" }
---
Body
 )");

  WorkflowLoader::instance().init();
  const Workflow *wf = WorkflowLoader::instance().getWorkflow("hook_match_structured");
  ASSERT_NE(wf, nullptr);
  EXPECT_TRUE(wf->isHook());
  EXPECT_EQ(wf->trigger.event, WorkflowEventKind::PreToolUse);

  EXPECT_EQ(wf->trigger.match.equals.at("tool"), "Files.Edit");
  EXPECT_EQ(wf->trigger.match.present.at("persona"), true);
  EXPECT_EQ(wf->trigger.match.present.at("custom"), false);
}

TEST_F(WorkflowTest, LoadsRawRemainderSlashWorkflow) {
  createWorkflow(
      "promise_command.md",
      R"(---
id: promise.command.promise
slash_command: /promise
raw_remainder: true
args:
  - name: task
    type: string
    description: task text
action:
  kind: script
  language: luau
  body: "return outcome.allow{ text = event.payload.extra.raw_args }"
---
)");

  WorkflowLoader::instance().init();
  const Workflow *wf =
      WorkflowLoader::instance().getWorkflow("promise.command.promise");
  ASSERT_NE(wf, nullptr);
  ASSERT_TRUE(wf->slashCommand.has_value());
  EXPECT_EQ(*wf->slashCommand, "/promise");
  EXPECT_TRUE(wf->rawRemainder);
  EXPECT_EQ(wf->action.kind, WorkflowActionKind::Script);
}

TEST(TemplateEngineRegression, RendersStateAndToolArgsPaths) {
  hooks::TemplateContext ctx = hooks::makeTemplateContext(
      R"({"thread":{"promise":{"iteration":2}}})",
      R"({"payload":{"persona":"forge"}})",
      R"({"path":"src/demo.cpp","value":7})",
      R"({"verdict":{"kind":"reject"}})",
      {{"persona", "forge"}});

  const std::string rendered = hooks::renderTemplate(
      "{{persona}} {{state.thread.promise.iteration}} {{tool.args.path}} {{subagent.return.verdict.kind}}",
      ctx);
  EXPECT_EQ(rendered, "forge 2 src/demo.cpp reject");
}

TEST(HookEnvelopeRegression, SerializesToolArgsIntoPayload) {
  hooks::EventPayload payload;
  payload.threadId = "thread-1";
  payload.agentId = "agent-1";
  payload.persona = "forge";
  payload.toolName = "Files.Edit";
  payload.toolArgsJson = R"({"path":"src/demo.cpp"})";

  const auto env = hooks::buildEnvelope("hook.id",
                                        WorkflowEventKind::PreToolUse,
                                        payload, "{}");
  const std::string json = hooks::serializeEnvelope(env);
  EXPECT_NE(json.find("src/demo.cpp"), std::string::npos);
  EXPECT_NE(json.find("Files.Edit"), std::string::npos);
}

} // namespace firmius::core
