find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

if(TARGET SQLite3::SQLite3)
  set(FIRMIUS_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
  set(FIRMIUS_SQLITE_TARGET SQLite::SQLite3)
endif()

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
    src/environment/PolicyEngine.cpp
    src/environment/PermissionSuggestionEngine.cpp
    src/tools/FileEditTool.cpp
    src/tools/ReadTool.cpp
    src/tools/ListTool.cpp
    src/tools/GrepTool.cpp
    src/tools/GlobTool.cpp
    src/tools/WebTool.cpp
    src/tools/ProcessTool.cpp
    src/tools/DelegateTool.cpp
    src/tools/FleetTool.cpp
    src/tools/ArtifactsTool.cpp
    src/tools/LspTool.cpp
    src/tools/LspDiagnosticsTool.cpp
    src/tools/LspQueryTool.cpp
    src/tools/ToolRegistry.cpp
    src/mcp/McpStdioSession.cpp
    src/mcp/McpHttpSession.cpp
    src/mcp/McpClient.cpp
    src/tools/PythonExecuteTool.cpp
    src/tools/WebFetchTool.cpp
    src/tools/WebSearchTool.cpp
    src/tools/SkillLoadTool.cpp
    src/tools/FleetLockTool.cpp
    src/tools/FleetLockRespondTool.cpp
    src/tools/FleetStatusTool.cpp
    src/tools/TodoWriteTool.cpp
    src/tools/PinTool.cpp
    src/agents/SkillLoader.cpp
    src/agents/HintingLoader.cpp
    src/agents/PurposeLoader.cpp
    src/agents/prompts/PromptComposer.cpp
    src/agents/modes/Mode.cpp
    src/agents/hooks/HookRegistry.cpp
    src/agents/hooks/HookEnvelope.cpp
    src/agents/hooks/HookState.cpp
    src/agents/hooks/ScriptRuntime.cpp
    src/agents/hooks/TemplateEngine.cpp
    src/agents/RuntimeOverlay.cpp
    src/agents/ContextBudget.cpp
    src/agents/UserMemoryWorkspace.cpp
    src/agents/StreamSanityDetector.cpp
    src/agents/Agent.cpp
    src/agents/AgentPermissionChecks.cpp
    src/agents/working_memory/DeflationArchive.cpp
    src/agents/working_memory/Deflator.cpp
    src/agents/working_memory/PinPolicy.cpp
    src/agents/working_memory/WorkingMemory.cpp
    src/agents/working_memory/WorkingMemoryWorker.cpp
    src/persistence/Journaler.cpp
    src/persistence/ThreadManager.cpp
    src/persistence/HistoryEditor.cpp
    src/Engine.cpp
    src/harness/ThreadLockManager.cpp
    src/Harness.cpp
    src/benchmarks/MBPPBenchmark.cpp
    src/benchmarks/AgentBench.cpp
    src/benchmarks/SWEBench.cpp
    src/benchmarks/SWEBenchTaskSpec.cpp
    src/benchmarks/TuringSWEBenchPP.cpp
    src/benchmarks/BenchmarkFactory.cpp
    src/benchmarks/BenchmarkSession.cpp
    src/workflow/Workflow.cpp
    src/workflow/WorkflowLoader.cpp
    src/artifacts/ReferenceExpansion.cpp
    src/lsp/JsonRpcTransport.cpp
    src/lsp/LspClient.cpp
    src/lsp/LspServerRegistry.cpp
    src/lsp/LspServerManager.cpp
    src/lsp/LspService.cpp
    src/embedding/EmbeddingStore.cpp
    src/embedding/EmbeddingWorker.cpp
    src/embedding/ModelDownloader.cpp
    src/embedding/OnnxEmbedder.cpp
)

target_sources(firmius_core PUBLIC
  FILE_SET HEADERS
  BASE_DIRS include
  FILES
    include/AgentRegistry.hpp
    include/Engine.hpp
    include/agents/Agent.hpp
    include/agents/AgentPermissionChecks.hpp
    include/agents/ContextBudget.hpp
    include/agents/HintingLoader.hpp
    include/agents/PurposeLoader.hpp
    include/agents/RuntimeOverlay.hpp
    include/agents/SkillLoader.hpp
    include/agents/StreamSanityDetector.hpp
    include/agents/UserMemoryWorkspace.hpp
    include/agents/hooks/HookEnvelope.hpp
    include/agents/hooks/HookOutcome.hpp
    include/agents/hooks/HookRegistry.hpp
    include/agents/hooks/HookState.hpp
    include/agents/hooks/ScriptRuntime.hpp
    include/agents/hooks/TemplateEngine.hpp
    include/agents/modes/Mode.hpp
    include/agents/prompts/PromptComposer.hpp
    include/agents/working_memory/DeflationArchive.hpp
    include/agents/working_memory/Deflator.hpp
    include/agents/working_memory/PinPolicy.hpp
    include/agents/working_memory/WorkingMemory.hpp
    include/agents/working_memory/WorkingMemoryWorker.hpp
    include/artifacts/ReferenceExpansion.hpp
    include/benchmarks/AgentBench.hpp
    include/benchmarks/BenchmarkFactory.hpp
    include/benchmarks/BenchmarkSession.hpp
    include/benchmarks/MBPPBenchmark.hpp
    include/benchmarks/SWEBench.hpp
    include/benchmarks/SWEBenchTaskSpec.hpp
    include/benchmarks/TuringSWEBenchPP.hpp
    include/embedding/EmbeddingStore.hpp
    include/embedding/EmbeddingWorker.hpp
    include/embedding/ModelDownloader.hpp
    include/embedding/OnnxEmbedder.hpp
    include/environment/CommandIntentAnalyzer.hpp
    include/environment/Environment.hpp
    include/environment/PermissionPolicy.hpp
    include/environment/PermissionSuggestionEngine.hpp
    include/environment/Permissions.hpp
    include/environment/PolicyEngine.hpp
    include/environment/ProcessManager.hpp
    include/environment/Workspace.hpp
    include/harness/Harness.hpp
    include/harness/ThreadLockManager.hpp
    include/hosts/DockerHost.hpp
    include/hosts/LocalHost.hpp
    include/lsp/JsonRpcTransport.hpp
    include/lsp/LspClient.hpp
    include/lsp/LspProtocol.hpp
    include/lsp/LspServerManager.hpp
    include/lsp/LspServerRegistry.hpp
    include/lsp/LspServerSpec.hpp
    include/lsp/LspService.hpp
    include/mcp/IMcpSession.hpp
    include/mcp/McpClient.hpp
    include/mcp/McpHttpSession.hpp
    include/mcp/McpManager.hpp
    include/mcp/McpStdioSession.hpp
    include/persistence/HistoryEditor.hpp
    include/persistence/Journaler.hpp
    include/persistence/ThreadManager.hpp
    include/tools/ArtifactsTool.hpp
    include/tools/DelegateTool.hpp
    include/tools/FileEditTool.hpp
    include/tools/FleetLockRespondTool.hpp
    include/tools/FleetLockTool.hpp
    include/tools/FleetStatusTool.hpp
    include/tools/FleetTool.hpp
    include/tools/GlobTool.hpp
    include/tools/GrepTool.hpp
    include/tools/ListTool.hpp
    include/tools/LspDiagnosticsTool.hpp
    include/tools/LspQueryTool.hpp
    include/tools/LspTool.hpp
    include/tools/PinTool.hpp
    include/tools/ProcessTool.hpp
    include/tools/PythonExecuteTool.hpp
    include/tools/ReadTool.hpp
    include/tools/SkillLoadTool.hpp
    include/tools/TodoWriteTool.hpp
    include/tools/ToolRegistry.hpp
    include/tools/WebFetchTool.hpp
    include/tools/WebSearchTool.hpp
    include/tools/WebTool.hpp
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
    ${FIRMIUS_SQLITE_TARGET}
)

# ─── Optional Luau (sandboxed scripting for kind: script hook actions) ─────
if(FIRMIUS_ENABLE_LUAU_HOOKS)
  target_compile_definitions(firmius_core PRIVATE FIRMIUS_ENABLE_LUAU_HOOKS=1)
  target_compile_definitions(firmius_core PRIVATE LUA_API=extern\"C\")
  target_compile_definitions(firmius_core PRIVATE LUACODE_API=extern\"C\")
  target_link_libraries(firmius_core PRIVATE
    Luau.VM
    Luau.Compiler
    Luau.Ast
    Luau.Common
    Luau.Config
    Luau.CLI.lib
    isocline
  )
endif()

# ─── Optional ONNX Runtime (embedding inference) ─────────────────────────
if(ONNXRUNTIME_FOUND)
  target_compile_definitions(firmius_core PRIVATE FIRMIUS_HAS_ONNX=1)
  target_link_libraries(firmius_core PRIVATE onnxruntime)
endif()

target_include_directories(firmius_core PRIVATE
  ${rapidjson_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
