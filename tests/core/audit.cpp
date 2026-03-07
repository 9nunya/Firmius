#include "EnvLoader.hpp"
#include "hosts/DockerHost.hpp"
#include "providers/ProviderRegistry.hpp"
#include "providers/NanoGPTProvider.hpp"
#include "providers/OpenRouterProvider.hpp"
#include "providers/ZaiProvider.hpp"
#include "providers/ZenProvider.hpp"
#include "providers/ChutesProvider.hpp"
#include "agents/Agent.hpp"
#include "agents/PurposeLoader.hpp"
#include "tools/FileReadTool.hpp"
#include "tools/FileEditTool.hpp"
#include "tools/ProcessExecuteTool.hpp"
#include "tools/PythonExecuteTool.hpp"
#include "tools/ListDirectoryTool.hpp"
#include "tools/GlobTool.hpp"
#include "tools/GrepTool.hpp"
#include "tools/WebFetchTool.hpp"
#include "tools/ToolRegistry.hpp"
#include "benchmarks/MBPPBenchmark.hpp"
#include "benchmarks/AgentBench.hpp"
#include "benchmarks/SWEBench.hpp"
#include "Panic.hpp"
#include <iostream>
#include <memory>
#include <random>
#include <algorithm>

using namespace firmius::shared;
using namespace firmius::core;
using namespace firmius::provider;

int main(int argc, char** argv) {
    Panic::init();
    EnvLoader::load(".env.local");

    std::string benchType = "mbpp";
    std::string taskId = "";
    std::string provId = "zen";
    std::string modelId = "big-pickle";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--bench" && i + 1 < argc) {
            benchType = argv[++i];
        } else if (arg == "--task" && i + 1 < argc) {
            taskId = argv[++i];
        } else if (arg == "--provider" && i + 1 < argc) {
            provId = argv[++i];
        } else if (arg == "--model" && i + 1 < argc) {
            modelId = argv[++i];
        }
    }

    std::string containerId = EnvLoader::get("DOCKER_AUDIT_CONTAINER");
    if (containerId.empty()) {
        std::cerr << "WARN: DOCKER_AUDIT_CONTAINER not set in .env.local" << std::endl;
    }

    HostCreationOptions opts;
    opts.type = HostType::Docker;
    opts.containerName = containerId;
    opts.connectToExisting = !containerId.empty();
    auto host = std::make_unique<DockerHost>(opts);
    if (containerId.empty()) {
        host->init();
    }

    auto& providerRegistry = ProviderRegistry::instance();
    providerRegistry.registerProvider(std::make_shared<NanoGPTProvider>());
    providerRegistry.registerProvider(std::make_shared<OpenRouterProvider>(""));
    providerRegistry.registerProvider(std::make_shared<ZaiProvider>(""));
    providerRegistry.registerProvider(std::make_shared<ZenProvider>(""));
    providerRegistry.registerProvider(std::make_shared<ChutesProvider>(""));

    ToolRegistry registry;
    registry.registerTool(std::make_unique<FileReadTool>());
    registry.registerTool(std::make_unique<FileEditTool>());
    registry.registerTool(std::make_unique<ProcessExecuteTool>());
    registry.registerTool(std::make_unique<PythonExecuteTool>());
    registry.registerTool(std::make_unique<ListDirectoryTool>());
    registry.registerTool(std::make_unique<GlobTool>());
    registry.registerTool(std::make_unique<GrepTool>());
    registry.registerTool(std::make_unique<WebFetchTool>());

    AgentContext context;
    context.config.providerId = provId;
    context.config.modelId = modelId;
    context.environment.type = HostType::Docker;
    context.environment.identifier = containerId;
    context.environment.cwd = "/work";
    context.permissions.allowedPaths = {"/work", "/tmp"};
    context.permissions.allowedScopes = {ToolScope::FilesystemRead, ToolScope::FilesystemWrite, ToolScope::Process};

    Agent agent(context, std::unique_ptr<firmius::shared::IHost>(host.get()), registry);

    std::unique_ptr<IBenchmark> benchmark;
    if (benchType == "mbpp") {
        benchmark = std::make_unique<MBPPBenchmark>(agent, *host);
    } else if (benchType == "agentbench") {
        benchmark = std::make_unique<AgentBench>(agent, *host);
    } else if (benchType == "swebench") {
        benchmark = std::make_unique<SWEBench>(agent, *host);
    } else {
        std::cerr << "Unknown benchmark: " << benchType << std::endl;
        return 1;
    }

    std::cout << "--- Starting Benchmark: " << benchType << " ---" << std::endl;
    auto tasks = benchmark->listTasks();
    if (tasks.empty()) {
        std::cerr << "No tasks found in benchmark" << std::endl;
        return 1;
    }

    if (taskId.empty()) {
        std::random_device rd;
        std::mt19937 g(rd());
        std::uniform_int_distribution<> dis(0, (int)tasks.size() - 1);
        taskId = tasks[dis(g)];
    }

    std::cout << "Target Task ID: " << taskId << std::endl;

    std::cout << "\n[1/2] Preparing task environment..." << std::endl;
    if (!benchmark->prepareTask(taskId)) {
        std::cerr << "Preparation FAILED (Environment is not ready or baseline tests did not fail as expected)." << std::endl;
        return 1;
    }

    std::cout << "\n[2/2] Running agent..." << std::endl;
    auto result = benchmark->runTask(taskId);

    std::cout << "\n--- Benchmark Result ---" << std::endl;
    std::cout << "Task ID: " << result.taskId << std::endl;
    std::cout << "Passed: " << (result.passed ? "YES" : "NO") << std::endl;
    std::cout << "Output: " << result.output << std::endl;

    return result.passed ? 0 : 1;
}
