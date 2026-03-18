#include "audits/WorkflowsAudit.hpp"
#include "AgentRegistry.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "harness/Harness.hpp"
#include "workflow/Workflow.hpp"
#include "workflow/WorkflowLoader.hpp"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;
using namespace std::chrono_literals;

namespace {
constexpr int AUDIT_EXIT_SUCCESS = 0;
constexpr int AUDIT_EXIT_LOAD_FAILED = 10;
constexpr int AUDIT_EXIT_PARSE_FAILED = 20;
constexpr int AUDIT_EXIT_EXEC_FAILED = 30;
constexpr int AUDIT_EXIT_GENERAL_FAILURE = 1;

struct TestState {
  std::mutex mtx;
  std::condition_variable cv;
  std::atomic<bool> gotMessage{false};
  std::string capturedMessage;
};

bool checkDockerAvailable() {
  int result = std::system("docker info > /dev/null 2>&1");
  return result == 0;
}

bool checkSandboxImage() {
  int result = std::system(
      "docker image inspect firmius-sandbox:latest > /dev/null 2>&1");
  return result == 0;
}
} // namespace

std::string WorkflowsAudit::getId() const { return "workflows"; }

std::string WorkflowsAudit::getDescription() const {
  return "Verify workflow loading, argument replacement, and execution";
}

shared::AuditResult WorkflowsAudit::run(const std::vector<std::string> &args) {
  (void)args;
  shared::AuditResult result;
  result.auditId = getId();

  std::cout << "🚀 STARTING WORKFLOWS AUDIT" << std::endl;
  std::cout << "========================================" << std::endl;

  if (!checkDockerAvailable()) {
    std::cerr << "FAILED: Docker is not available." << std::endl;
    result.exitCode = AUDIT_EXIT_GENERAL_FAILURE;
    result.passed = false;
    result.output = "Docker not available";
    return result;
  }
  std::cout << "✓ Docker available" << std::endl;

  if (!checkSandboxImage()) {
    std::cerr << "FAILED: Docker image 'firmius-sandbox:latest' not found."
              << std::endl;
    result.exitCode = AUDIT_EXIT_GENERAL_FAILURE;
    result.passed = false;
    result.output = "Sandbox image not found";
    return result;
  }
  std::cout << "✓ Sandbox image found" << std::endl;

  // Create temp directory for test workflows
  char tempDirTemplate[] = "/tmp/firmius_workflows_XXXXXX";
  char *tempDir = mkdtemp(tempDirTemplate);
  if (!tempDir) {
    std::cerr << "FAILED: Could not create temp directory" << std::endl;
    result.exitCode = AUDIT_EXIT_GENERAL_FAILURE;
    result.passed = false;
    result.output = "Could not create temp directory";
    return result;
  }
  std::string tempDirStr(tempDir);
  std::cout << "Temp directory: " << tempDirStr << std::endl;

  // Create test workflow files
  std::string testWorkflow1 = tempDirStr + "/test_echo.md";
  {
    std::ofstream f(testWorkflow1);
    f << "---\n";
    f << "name: Test Echo\n";
    f << "description: Simple echo test with args\n";
    f << "---\n";
    f << "\n";
    f << "Echo these values: $1 and $2\n";
    f.close();
  }
  std::cout << "✓ Created test workflow: test_echo.md" << std::endl;

  std::string testWorkflow2 = tempDirStr + "/test_no_args.md";
  {
    std::ofstream f(testWorkflow2);
    f << "---\n";
    f << "name: No Args Test\n";
    f << "description: Workflow without arguments\n";
    f << "---\n";
    f << "\n";
    f << "This workflow has no placeholders.\n";
    f.close();
  }
  std::cout << "✓ Created test workflow: test_no_args.md" << std::endl;

  // Set environment to use temp workflows directory
  const char *originalWorkflowsDir = std::getenv("FIRMIUS_WORKFLOWS_DIR");
  std::string originalWorkflowsDirStr =
      originalWorkflowsDir ? originalWorkflowsDir : "";
  ::setenv("FIRMIUS_WORKFLOWS_DIR", tempDirStr.c_str(), 1);

  auto cleanupEnv = [&]() {
    if (!originalWorkflowsDirStr.empty()) {
      ::setenv("FIRMIUS_WORKFLOWS_DIR", originalWorkflowsDirStr.c_str(), 1);
    } else {
      ::unsetenv("FIRMIUS_WORKFLOWS_DIR");
    }
    std::filesystem::remove_all(tempDirStr);
  };

  // Initialize workflow loader
  std::cout << "\n=== PHASE 1: Workflow Loading ===" << std::endl;
  WorkflowLoader::instance().init();

  auto workflowIds = WorkflowLoader::instance().getWorkflowIds();
  if (workflowIds.empty()) {
    std::cerr << "FAILED: No workflows loaded" << std::endl;
    result.exitCode = AUDIT_EXIT_LOAD_FAILED;
    result.passed = false;
    result.output = "No workflows loaded";
    cleanupEnv();
    return result;
  }
  std::cout << "✓ Loaded " << workflowIds.size() << " workflows: ";
  for (const auto &id : workflowIds) {
    std::cout << id << " ";
  }
  std::cout << std::endl;

  // Test workflow retrieval
  const Workflow *wf = WorkflowLoader::instance().getWorkflow("test_echo");
  if (!wf) {
    std::cerr << "FAILED: Could not get test_echo workflow" << std::endl;
    result.exitCode = AUDIT_EXIT_LOAD_FAILED;
    result.passed = false;
    result.output = "Could not retrieve workflow";
    cleanupEnv();
    return result;
  }
  std::cout << "✓ Retrieved workflow: " << wf->name << std::endl;

  // Test argument parsing
  std::cout << "\n=== PHASE 2: Argument Placeholder Parsing ===" << std::endl;
  if (wf->argCount != 2) {
    std::cerr << "FAILED: Expected 2 args, got " << wf->argCount << std::endl;
    result.exitCode = AUDIT_EXIT_PARSE_FAILED;
    result.passed = false;
    result.output = "Argument count mismatch";
    cleanupEnv();
    return result;
  }
  std::cout << "✓ Correctly detected " << wf->argCount << " argument(s)"
            << std::endl;

  // Test placeholder replacement
  std::string built = wf->build({"hello", "world"});
  if (built.find("hello") == std::string::npos ||
      built.find("world") == std::string::npos) {
    std::cerr << "FAILED: Placeholder replacement failed" << std::endl;
    std::cerr << "Built: " << built << std::endl;
    result.exitCode = AUDIT_EXIT_PARSE_FAILED;
    result.passed = false;
    result.output = "Placeholder replacement failed";
    cleanupEnv();
    return result;
  }
  if (built.find("$1") != std::string::npos ||
      built.find("$2") != std::string::npos) {
    std::cerr << "FAILED: Unreplaced placeholders found" << std::endl;
    result.exitCode = AUDIT_EXIT_PARSE_FAILED;
    result.passed = false;
    result.output = "Unreplaced placeholders";
    cleanupEnv();
    return result;
  }
  std::cout << "✓ Placeholder replacement works: " << built << std::endl;

  // Test workflow with no args
  const Workflow *wfNoArgs =
      WorkflowLoader::instance().getWorkflow("test_no_args");
  if (!wfNoArgs) {
    std::cerr << "FAILED: Could not get test_no_args workflow" << std::endl;
    result.exitCode = AUDIT_EXIT_LOAD_FAILED;
    result.passed = false;
    result.output = "Could not retrieve workflow";
    cleanupEnv();
    return result;
  }
  if (wfNoArgs->argCount != 0) {
    std::cerr << "FAILED: Expected 0 args for test_no_args, got "
              << wfNoArgs->argCount << std::endl;
    result.exitCode = AUDIT_EXIT_PARSE_FAILED;
    result.passed = false;
    result.output = "Argument count mismatch for no-args workflow";
    cleanupEnv();
    return result;
  }
  std::cout << "✓ No-args workflow correctly parsed" << std::endl;

  // Test workflow execution through Harness
  std::cout << "\n=== PHASE 3: Workflow Execution ===" << std::endl;

  Panic::init();
  EnvLoader::load(".env.local");

  std::string originalHome;
  const char *homeEnv = std::getenv("HOME");
  if (homeEnv) {
    originalHome = homeEnv;
  }

  char *tempHome = mkdtemp(strdup("/tmp/firmius_home_XXXXXX"));
  if (!tempHome) {
    std::cerr << "FAILED: Could not create temp home directory" << std::endl;
    result.exitCode = AUDIT_EXIT_GENERAL_FAILURE;
    result.passed = false;
    result.output = "Could not create temp home";
    cleanupEnv();
    return result;
  }
  std::string tempHomeStr(tempHome);
  ::setenv("HOME", tempHomeStr.c_str(), 1);

  auto cleanupHarness = [&]() {
    Harness::instance().shutdown();
    if (!originalHome.empty()) {
      ::setenv("HOME", originalHome.c_str(), 1);
    }
    std::filesystem::remove_all(tempHomeStr);
    cleanupEnv();
  };

  auto &harness = Harness::instance();
  harness.init();

  std::string testDir = tempHomeStr + "/test_project";
  std::filesystem::create_directories(testDir);

  std::string threadId =
      harness.newThread({HostType::Docker}, testDir, "lead");
  if (threadId.empty()) {
    std::cerr << "FAILED: Could not create test thread" << std::endl;
    result.exitCode = AUDIT_EXIT_EXEC_FAILED;
    result.passed = false;
    result.output = "Could not create thread";
    cleanupHarness();
    return result;
  }
  std::cout << "✓ Created test thread: " << threadId << std::endl;

  // Subscribe to events to verify message was sent
  TestState state;
  int subId = harness.subscribe([&state](const AppEvent &ev) {
    std::visit(
        [&state](auto &&e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, UserMessageSent>) {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.gotMessage = true;
            state.capturedMessage = e.text;
            state.cv.notify_one();
          }
        },
        ev);
  });

  // Execute workflow
  std::cout << "[Phase 3] Executing workflow with args..." << std::endl;
  bool execSuccess = harness.executeWorkflow("test_echo", {"foo", "bar"});
  if (!execSuccess) {
    std::cerr << "FAILED: executeWorkflow returned false" << std::endl;
    result.exitCode = AUDIT_EXIT_EXEC_FAILED;
    result.passed = false;
    result.output = "executeWorkflow failed";
    harness.unsubscribe(subId);
    cleanupHarness();
    return result;
  }
  std::cout << "✓ executeWorkflow returned success" << std::endl;

  // Wait for message event
  {
    std::unique_lock<std::mutex> lk(state.mtx);
    bool gotEvent = state.cv.wait_for(
        lk, 10s, [&state] { return state.gotMessage.load(); });
    if (!gotEvent) {
      std::cerr << "FAILED: UserMessageSent event not received" << std::endl;
      result.exitCode = AUDIT_EXIT_EXEC_FAILED;
      result.passed = false;
      result.output = "Message event not received";
      harness.unsubscribe(subId);
      cleanupHarness();
      return result;
    }
  }
  std::cout << "✓ UserMessageSent event received" << std::endl;

  // Verify the message content has replaced args
  if (state.capturedMessage.find("foo") == std::string::npos ||
      state.capturedMessage.find("bar") == std::string::npos) {
    std::cerr << "FAILED: Message content doesn't have replaced args"
              << std::endl;
    std::cerr << "Captured: " << state.capturedMessage << std::endl;
    result.exitCode = AUDIT_EXIT_EXEC_FAILED;
    result.passed = false;
    result.output = "Message content incorrect";
    harness.unsubscribe(subId);
    cleanupHarness();
    return result;
  }
  std::cout << "✓ Message content verified: " << state.capturedMessage
            << std::endl;

  harness.unsubscribe(subId);

  // Test executing non-existent workflow
  std::cout << "\n=== PHASE 4: Error Handling ===" << std::endl;
  bool nonExistentResult =
      harness.executeWorkflow("nonexistent_workflow", {"arg1"});
  if (nonExistentResult) {
    std::cerr << "FAILED: executeWorkflow should return false for non-existent "
                 "workflow"
              << std::endl;
    result.exitCode = AUDIT_EXIT_EXEC_FAILED;
    result.passed = false;
    result.output = "Error handling failed";
    cleanupHarness();
    return result;
  }
  std::cout << "✓ Non-existent workflow correctly returns false" << std::endl;

  std::cout << "\n========================================" << std::endl;
  std::cout << "✅ WORKFLOWS AUDIT PASSED" << std::endl;

  result.exitCode = AUDIT_EXIT_SUCCESS;
  result.passed = true;
  result.output = "All workflow tests passed";

  cleanupHarness();
  return result;
}

} // namespace firmius::audits
