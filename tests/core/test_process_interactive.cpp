#include <gtest/gtest.h>
#include "hosts/LocalHost.hpp"
#include "agents/Agent.hpp"
#include "tools/ToolRegistry.hpp"
#include "tools/ProcessSpawnTool.hpp"
#include "tools/ProcessInputTool.hpp"
#include "tools/ProcessStatusTool.hpp"
#include "tools/ProcessWaitTool.hpp"
#include "providers/ProviderRegistry.hpp"
#include "providers/NanoGPTProvider.hpp"
#include "Panic.hpp"
#include <rapidjson/document.h>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::provider;

class ProcessInteractiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        Panic::init();
        
        // Register a dummy provider for Agent construction
        auto& reg = ProviderRegistry::instance();
        if (!reg.getProvider("nanogpt")) {
            reg.registerProvider(std::make_shared<NanoGPTProvider>());
        }

        // Setup AgentContext
        AgentContext ctx;
        ctx.config.providerId = "nanogpt";
        ctx.config.modelId = "test-model";
        ctx.environment.type = HostType::Local;
        ctx.environment.cwd = "/tmp";
        ctx.permissions.allowedScopes = {ToolScope::Process};

        // Registry and tools
        registry.registerTool(std::make_unique<ProcessSpawnTool>());
        registry.registerTool(std::make_unique<ProcessInputTool>());
        registry.registerTool(std::make_unique<ProcessStatusTool>());
        registry.registerTool(std::make_unique<ProcessWaitTool>());

        host = std::make_unique<LocalHost>();
        agent = std::make_unique<Agent>(ctx, *host, registry);
    }

    ToolRegistry registry;
    std::unique_ptr<LocalHost> host;
    std::unique_ptr<Agent> agent;

    std::string callTool(const std::string& name, const std::string& jsonArgs) {
        rapidjson::Document doc;
        doc.Parse(jsonArgs.c_str());
        ToolContext ctx{*host, *agent};
        auto res = registry.execute(name, doc, ctx);
        if (!res.success) {
            throw std::runtime_error("Tool " + name + " failed: " + res.error);
        }
        return res.data;
    }
};

TEST_F(ProcessInteractiveTest, PythonInteraction) {
    // 1. Spawn interactive python
    std::string spawnData = callTool("process_spawn", R"({"command": "python3 -i"})");
    rapidjson::Document spawnDoc;
    spawnDoc.Parse(spawnData.c_str());
    std::string pid = spawnDoc["process_id"].GetString();
    EXPECT_FALSE(pid.empty());

    // 2. Send input
    callTool("process_input", R"({"process_id": ")" + pid + R"(", "input": "print('firmius_interactive_test')\n"})");

    // 3. Wait for output pattern
    std::string waitData = callTool("process_wait", R"({"process_id": ")" + pid + R"(", "pattern": "firmius_interactive_test", "timeout_ms": 5000})");
    rapidjson::Document waitDoc;
    waitDoc.Parse(waitData.c_str());
    EXPECT_TRUE(waitDoc["patternFound"].GetBool());
    EXPECT_TRUE(waitDoc["isRunning"].GetBool());

    // 4. Send exit
    callTool("process_input", R"({"process_id": ")" + pid + R"(", "input": "exit()\n"})");

    // 5. Verify process exits
    // Poll status a bit
    bool exited = false;
    for (int i = 0; i < 20; ++i) {
        std::string statusData = callTool("process_status", R"({"process_id": ")" + pid + R"("})");
        rapidjson::Document statusDoc;
        statusDoc.Parse(statusData.c_str());
        std::cout << "[Poll " << i << "] Running: " << (statusDoc["isRunning"].GetBool() ? "Yes" : "No") 
                  << ", Stdout: " << statusDoc["stdout"].GetString() << std::endl;
        if (!statusDoc["isRunning"].GetBool()) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    EXPECT_TRUE(exited);
}

TEST_F(ProcessInteractiveTest, PatternWaitTimeout) {
    // 1. Spawn a long running process
    std::string spawnData = callTool("process_spawn", R"({"command": "sleep 10"})");
    rapidjson::Document spawnDoc;
    spawnDoc.Parse(spawnData.c_str());
    std::string pid = spawnDoc["process_id"].GetString();

    // 2. Wait for a pattern that will never appear with a short timeout
    // process_wait should fail if timeout is reached
    rapidjson::Document doc;
    doc.Parse(R"({"process_id": "placeholder", "pattern": "never", "timeout_ms": 1000})");
    doc["process_id"].SetString(pid.c_str(), doc.GetAllocator());
    
    ToolContext ctx{*host, *agent};
    auto res = registry.execute("process_wait", doc, ctx);
    
    EXPECT_FALSE(res.success);
    EXPECT_TRUE(res.error.find("Timeout") != std::string::npos);
}
