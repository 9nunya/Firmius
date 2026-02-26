#include "EnvLoader.hpp"
#include "hosts/LocalHost.hpp"
#include "hosts/DockerHost.hpp"
#include "tools/FileReadTool.hpp"
#include "tools/FileEditTool.hpp"
#include "tools/ProcessExecuteTool.hpp"
#include "tools/ToolRegistry.hpp"
#include <iostream>
#include <memory>
#include <rapidjson/prettywriter.h>

using namespace firmius::shared;
using namespace firmius::shared;

void printJson(const rapidjson::Value& v) {
    rapidjson::StringBuffer sb;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
    v.Accept(writer);
    std::cout << sb.GetString() << std::endl;
}

void runScenario(IHost& host, const std::string& hostName, const AgentPermissions& perms) {
    std::cout << "\n=== Running Scenario on " << hostName << " ===" << std::endl;
    
    ToolRegistry registry;
    registry.registerTool(std::make_unique<FileReadTool>());
    registry.registerTool(std::make_unique<FileEditTool>());
    registry.registerTool(std::make_unique<ProcessExecuteTool>());

    ToolContext ctx{host, perms};

    // 1. Write hello.py
    std::cout << "1. Writing hello.py..." << std::endl;
    rapidjson::Document writeInput;
    writeInput.SetObject();
    writeInput.AddMember("path", "/tmp/hello.py", writeInput.GetAllocator());
    writeInput.AddMember("content", "print('Hello from Path 3 Audit!')", writeInput.GetAllocator());
    auto writeRes = registry.execute("file_edit", writeInput, ctx);
    printJson(writeRes);

    // 2. Read hello.py
    std::cout << "2. Reading hello.py..." << std::endl;
    rapidjson::Document readInput;
    readInput.SetObject();
    readInput.AddMember("path", "/tmp/hello.py", readInput.GetAllocator());
    auto readRes = registry.execute("file_read", readInput, ctx);
    printJson(readRes);

    // 3. Execute hello.py
    std::cout << "3. Executing hello.py..." << std::endl;
    rapidjson::Document execInput;
    execInput.SetObject();
    execInput.AddMember("command", "python3 /tmp/hello.py", execInput.GetAllocator());
    auto execRes = registry.execute("process_execute", execInput, ctx);
    printJson(execRes);

    // 4. Try to access forbidden path
    std::cout << "4. Testing security (Accessing /etc/shadow)..." << std::endl;
    rapidjson::Document secretInput;
    secretInput.SetObject();
    secretInput.AddMember("path", "/etc/shadow", secretInput.GetAllocator());
    auto secretRes = registry.execute("file_read", secretInput, ctx);
    printJson(secretRes);

    // 5. Cleanup
    std::cout << "5. Cleaning up..." << std::endl;
    host.exec("rm /tmp/hello.py");
}

int main() {
    EnvLoader::load(".env.local");

    AgentPermissions perms;
    perms.allowedScopes = {ToolScope::FilesystemRead, ToolScope::FilesystemWrite, ToolScope::Process};
    perms.allowedPaths = {"/tmp"};
    perms.allowOutsideCwd = false;

    // Local Host
    LocalHost local;
    local.init();
    runScenario(local, "LocalHost", perms);
    local.destroy();

    // Docker Host (if configured)
    std::string containerId = EnvLoader::get("DOCKER_AUDIT_CONTAINER");
    if (!containerId.empty()) {
        try {
            DockerHost docker(containerId);
            docker.init();
            runScenario(docker, "DockerHost (" + containerId + ")", perms);
            docker.destroy();
        } catch (const std::exception& e) {
            std::cerr << "DockerHost Audit failed: " << e.what() << std::endl;
        }
    } else {
        std::cout << "\nSkip DockerHost (DOCKER_AUDIT_CONTAINER not set)" << std::endl;
    }

    return 0;
}
