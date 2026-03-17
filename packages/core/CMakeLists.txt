find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_library(firmius_core STATIC)
add_library(firmius::core ALIAS firmius_core)

target_sources(firmius_core PRIVATE
    src/hosts/LocalHost.cpp
    src/hosts/DockerHost.cpp
    src/environment/CommandIntentAnalyzer.cpp
    src/environment/ProcessManager.cpp
    src/environment/Workspace.cpp
    src/environment/Environment.cpp
    src/environment/Permissions.cpp
    src/tools/FileReadTool.cpp
    src/tools/FileEditTool.cpp
    src/tools/ProcessExecuteTool.cpp
    src/tools/ToolRegistry.cpp
    src/tools/SubagentTool.cpp
    src/tools/SubagentWaitTool.cpp
    src/tools/SubagentTerminateTool.cpp
    src/tools/PythonExecuteTool.cpp
    src/tools/ListDirectoryTool.cpp
    src/tools/GlobTool.cpp
    src/tools/GrepTool.cpp
    src/tools/WebFetchTool.cpp
    src/tools/ProcessSpawnTool.cpp
    src/tools/ProcessStatusTool.cpp
    src/tools/ProcessWaitTool.cpp
    src/tools/ProcessInputTool.cpp
    src/agents/PurposeLoader.cpp
    src/agents/Agent.cpp
    src/agents/AgentPermissionChecks.cpp
    src/agents/AgentPermissionChecks.cpp
    src/persistence/Journaler.cpp
    src/persistence/ThreadManager.cpp
    src/persistence/HistoryEditor.cpp
    src/Engine.cpp
    src/harness/ThreadLockManager.cpp
    src/Harness.cpp
    src/benchmarks/MBPPBenchmark.cpp
    src/benchmarks/AgentBench.cpp
    src/benchmarks/SWEBench.cpp
    src/benchmarks/BenchmarkSession.cpp
    src/workflow/Workflow.cpp
    src/workflow/WorkflowLoader.cpp
)

target_sources(firmius_core PUBLIC
  FILE_SET HEADERS
  BASE_DIRS include
  FILES
    include/hosts/LocalHost.hpp
    include/hosts/DockerHost.hpp
    include/environment/CommandIntentAnalyzer.hpp
    include/environment/ProcessManager.hpp
    include/environment/Workspace.hpp
    include/environment/Environment.hpp
    include/environment/Permissions.hpp
    include/tools/FileReadTool.hpp
    include/tools/FileEditTool.hpp
    include/tools/ProcessExecuteTool.hpp
    include/tools/ToolRegistry.hpp
    include/tools/SubagentTool.hpp
    include/tools/SubagentWaitTool.hpp
    include/tools/SubagentTerminateTool.hpp
    include/tools/PythonExecuteTool.hpp
    include/tools/ListDirectoryTool.hpp
    include/tools/GlobTool.hpp
    include/tools/GrepTool.hpp
    include/tools/WebFetchTool.hpp
    include/tools/ProcessSpawnTool.hpp
    include/tools/ProcessStatusTool.hpp
    include/tools/ProcessWaitTool.hpp
    include/tools/ProcessInputTool.hpp
    include/agents/PurposeLoader.hpp
    include/agents/Agent.hpp
    include/agents/AgentPermissionChecks.hpp
    include/agents/AgentPermissionChecks.hpp
    include/persistence/Journaler.hpp
    include/persistence/ThreadManager.hpp
    include/persistence/HistoryEditor.hpp
    include/AgentRegistry.hpp
    include/Engine.hpp
    include/harness/Harness.hpp
    include/harness/ThreadLockManager.hpp
    include/benchmarks/MBPPBenchmark.hpp
    include/benchmarks/AgentBench.hpp
    include/benchmarks/SWEBench.hpp
    include/benchmarks/BenchmarkSession.hpp
    include/workflow/Workflow.hpp
    include/workflow/WorkflowLoader.hpp
)

target_include_directories(firmius_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/packages/provider/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(firmius_core PUBLIC
    firmius::shared
    firmius::provider
    CURL::libcurl
    Threads::Threads
    uuid
)

target_include_directories(firmius_core PRIVATE
  ${rapidjson_SOURCE_DIR}/include
)
