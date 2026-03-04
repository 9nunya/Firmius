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

    auto host = std::make_unique<DockerHost>(containerId);
    
    ProviderRegistry::instance().registerProvider(std::make_shared<NanoGPTProvider>());
    ProviderRegistry::instance().registerProvider(std::make_shared<OpenRouterProvider>(""));
    ProviderRegistry::instance().registerProvider(std::make_shared<ZaiProvider>(""));
    ProviderRegistry::instance().registerProvider(std::make_shared<ZenProvider>(""));
    ProviderRegistry::instance().registerProvider(std::make_shared<ChutesProvider>(""));

    ToolRegistry registry;
    registry.registerTool(std::make_unique<FileReadTool>());
    registry.registerTool(std::make_unique<FileEditTool>());
    registry.registerTool(std::make_unique<ProcessExecuteTool>());

    AgentContext context;
    context.config.providerId = "nanogpt";
    context.environment.type = HostType::Docker;
    context.environment.identifier = containerId;
    context.environment.cwd = "/work";
    context.permissions.allowedPaths = {"/work", "/tmp"};
    context.permissions.allowedScopes = {ToolScope::FilesystemRead, ToolScope::FilesystemWrite, ToolScope::Process};

    Agent agent(context, std::unique_ptr<firmius::shared::IHost>(host.get()), registry);
    SWEBench benchmark(agent, *host);

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
