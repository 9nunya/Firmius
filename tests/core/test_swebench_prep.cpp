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
#include "tools/ToolRegistry.hpp"
#include "benchmarks/SWEBench.hpp"
#include "Panic.hpp"
#include <iostream>
#include <memory>

using namespace firmius::shared;
using namespace firmius::core;
using namespace firmius::provider;

int main(int argc, char** argv) {
    Panic::init();
    EnvLoader::load(".env.local");

    std::string containerId = EnvLoader::get("DOCKER_AUDIT_CONTAINER");
    if (containerId.empty()) {
        std::cerr << "DOCKER_AUDIT_CONTAINER not set" << std::endl;
        return 1;
    }

    BenchmarkConfig config;
    config.hostOptions.type = HostType::Docker;
    config.hostOptions.containerName = containerId;
    config.hostOptions.connectToExisting = true;
    config.cwd = "/work";
    config.personaName = "lead";
    config.providerId = "nanogpt";
    config.modelId = "gpt-4";

    SWEBench benchmark(config);

    auto tasks = benchmark.listTasks();
    if (tasks.empty()) {
        std::cerr << "No tasks found" << std::endl;
        return 1;
    }

    std::string taskId = "django__django-12774";
    if (argc > 1) taskId = argv[1];

    std::cout << "--- Testing SWEBench Prep for Task: " << taskId << " ---" << std::endl;
    
    if (benchmark.prepareTask(taskId)) {
        std::cout << "\033[1;32mSUCCESS: Environment prepared and baseline tests failed as expected.\033[0m" << std::endl;
        return 0;
    } else {
        std::cout << "\033[1;31mFAILURE: Preparation failed or baseline tests did not behave as expected.\033[0m" << std::endl;
        return 1;
    }
}
