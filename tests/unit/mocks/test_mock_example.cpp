#include <gtest/gtest.h>
#include "mocks/MockHost.hpp"
#include "mocks/MockAgent.hpp"
#include "mocks/MockHostProcess.hpp"
#include "tools/FileReadTool.hpp"
#include "ITool.hpp"

using namespace firmius::test;
using namespace firmius::core;
using namespace firmius::shared;

/**
 * Example test demonstrating MockHost usage with tools.
 */
TEST(MockHostExample, FileReadToolExecution) {
    // Create a mock host with pre-configured files
    MockHost host;
    host.addMockFile("/work/test.txt", "Hello, World!");
    
    // Verify initial state
    EXPECT_TRUE(host.exists("/work/test.txt"));
    
    // Read the file using the mock host directly
    auto content = host.readFile("/work/test.txt");
    std::string text(content.begin(), content.end());
    EXPECT_EQ(text, "Hello, World!");
    
    // Verify the call was recorded
    EXPECT_TRUE(host.wasCalledWith("readFile", {{"path", "/work/test.txt"}}));
    EXPECT_EQ(host.getCallCount("readFile"), 1);
}

TEST(MockHostExample, CommandExecution) {
    MockHost host;
    
    // Configure expected command results
    host.setExecResult("ls -la", 0, "file1\nfile2\n", "");
    host.setExecResultPattern("git.*", 0, "git output", "");
    
    // Execute commands
    auto result1 = host.exec("ls -la");
    EXPECT_EQ(result1.exitCode, 0);
    EXPECT_EQ(result1.stdoutData, "file1\nfile2\n");
    
    auto result2 = host.exec("git status");
    EXPECT_EQ(result2.exitCode, 0);
    EXPECT_EQ(result2.stdoutData, "git output");
    
    // Verify commands were recorded
    EXPECT_TRUE(host.expectExecCalledWith("ls.*"));
    EXPECT_TRUE(host.expectExecCalledWith("git.*"));
}

TEST(MockHostExample, ErrorSimulation) {
    MockHost host;
    host.throwOnRead("/restricted/file.txt");
    
    EXPECT_THROW(host.readFile("/restricted/file.txt"), std::runtime_error);
    
    // Other files should work fine
    host.addMockFile("/allowed/file.txt", "content");
    auto content = host.readFile("/allowed/file.txt");
    EXPECT_FALSE(content.empty());
}

TEST(MockAgentExample, ContextManagement) {
    // Create agent with custom context
    AgentContext ctx;
    ctx.identity.id = "test-agent";
    ctx.environment.cwd = "/work";
    
    MockAgent agent(ctx);
    
    // Verify context is accessible
    EXPECT_EQ(agent.getContext().identity.id, "test-agent");
    EXPECT_EQ(agent.getContext().environment.cwd, "/work");
    
    // Modify context via convenience method
    agent.setContextField("identity.name", "Test Agent");
    EXPECT_EQ(agent.getContext().identity.name, "Test Agent");
    
    // Test path resolution
    std::string resolved = agent.resolvePath("./src/main.cpp");
    EXPECT_FALSE(resolved.empty());
    EXPECT_TRUE(agent.wasMethodCalled("resolvePath"));
}

TEST(MockAgentExample, FileTracking) {
    MockAgent agent;
    
    // Initially not read
    EXPECT_FALSE(agent.hasReadFile("/work/config.json"));
    
    // Mark as read
    agent.markFileAsRead("/work/config.json");
    
    // Now it should be tracked
    EXPECT_TRUE(agent.hasReadFile("/work/config.json"));
    EXPECT_TRUE(agent.wasCalledWith("markFileAsRead", {{"path", "/work/config.json"}}));
}

TEST(MockAgentExample, ProcessSpawning) {
    MockAgent agent;
    
    // Configure process result
    agent.setSpawnResult("npm install", 0, "installed", "");
    
    // Spawn process
    std::string processId = agent.spawnProcess("npm install");
    EXPECT_FALSE(processId.empty());
    
    // Inspect the process
    auto snapshot = agent.inspectProcess(processId);
    EXPECT_EQ(snapshot.exitCode, 0);
    EXPECT_EQ(snapshot.stdoutData, "installed");
}

TEST(MockAgentExample, InterruptHandling) {
    MockAgent agent;
    
    EXPECT_FALSE(agent.isInterrupted());
    
    agent.interrupt();
    
    EXPECT_TRUE(agent.isInterrupted());
    EXPECT_EQ(agent.getContext().state.currentStatus, AgentStatus::Cancelled);
}

TEST(MockHostProcessExample, OutputConfiguration) {
    MockHostProcessConfig config;
    config.systemId = "test-pid-123";
    config.running = true;
    config.exitCode = 42;
    config.stdoutData = "process output";
    
    MockHostProcess process(config);
    
    EXPECT_EQ(process.getSystemId(), "test-pid-123");
    EXPECT_TRUE(process.isRunning());
    
    auto result = process.wait();
    EXPECT_EQ(result.exitCode, 42);
    EXPECT_EQ(result.stdoutData, "process output");
    EXPECT_FALSE(process.isRunning());
}

TEST(MockHostProcessExample, OutputCallback) {
    MockHostProcess process;
    
    std::string capturedOutput;
    process.onOutput([&capturedOutput](const std::string& data, bool isError) {
        if (!isError) {
            capturedOutput += data;
        }
    });
    
    process.simulateOutput("Hello ", false);
    process.simulateOutput("World!", false);
    
    EXPECT_EQ(capturedOutput, "Hello World!");
}

TEST(MockHostProcessExample, StdinCapture) {
    MockHostProcess process;
    
    process.write("input line 1");
    process.write("input line 2");
    
    auto inputs = process.getWrittenData();
    EXPECT_EQ(inputs.size(), 2);
    EXPECT_EQ(inputs[0], "input line 1");
    EXPECT_EQ(inputs[1], "input line 2");
}
