add_library(firmius_audits STATIC)
add_library(firmius::audits ALIAS firmius_audits)

target_sources(firmius_audits PRIVATE
    src/AuditRegistry.cpp
    src/audits/AntigravityProviderAudit.cpp
    src/audits/AntigravityQuotaAudit.cpp
    src/audits/BenchmarksAudit.cpp
    src/audits/CodexProviderAudit.cpp
    src/audits/ContextBudgetAudit.cpp
    src/audits/EditToolAudit.cpp
    src/audits/EditHistoryAudit.cpp
    src/audits/CodexQuotaAudit.cpp
    src/audits/DaemonRuntimeIsolationAudit.cpp
    src/audits/HarnessChaosAudit.cpp
    src/audits/OAuthWizardAudit.cpp
    src/audits/P15Audit.cpp
    src/audits/PersistenceStressAudit.cpp
    src/audits/PromisesAudit.cpp
    src/audits/ProviderAudit.cpp
    src/audits/ProviderStreamDebugAudit.cpp
    src/audits/QwenProviderAudit.cpp
    src/audits/LoopCancellationAudit.cpp
    src/audits/LspAudit.cpp
    src/audits/ModelLoadAudit.cpp
    src/audits/OpenRouterQuotaAudit.cpp
    src/audits/QwenQuotaAudit.cpp
    src/audits/SubagentStressAudit.cpp
    src/audits/WorkflowsAudit.cpp
    src/audits/ResumeTodoAudit.cpp
    src/audits/WebFetchAudit.cpp
    src/audits/WebSearchAudit.cpp
    src/audits/ReasoningTraceAudit.cpp
    src/audits/TokenWasteAudit.cpp
    src/audits/CacheAudit.cpp
    src/audits/EmbeddingAudit.cpp
    src/audits/WorkingMemoryAudit.cpp
    src/audits/WorkingMemoryLiveAudit.cpp
    src/MigrationCli.cpp
)
target_sources(firmius_audits PUBLIC
  FILE_SET HEADERS
  BASE_DIRS include
  FILES
    include/AuditCliUtils.hpp
    include/AuditRegistry.hpp
    include/MigrationCli.hpp
    include/audits/AntigravityProviderAudit.hpp
    include/audits/AntigravityQuotaAudit.hpp
    include/audits/BenchmarksAudit.hpp
    include/audits/CacheAudit.hpp
    include/audits/CodexProviderAudit.hpp
    include/audits/CodexQuotaAudit.hpp
    include/audits/ContextBudgetAudit.hpp
    include/audits/DaemonRuntimeIsolationAudit.hpp
    include/audits/EditHistoryAudit.hpp
    include/audits/EditToolAudit.hpp
    include/audits/EmbeddingAudit.hpp
    include/audits/HarnessChaosAudit.hpp
    include/audits/LoopCancellationAudit.hpp
    include/audits/LspAudit.hpp
    include/audits/ModelLoadAudit.hpp
    include/audits/OAuthWizardAudit.hpp
    include/audits/OpenRouterQuotaAudit.hpp
    include/audits/P15Audit.hpp
    include/audits/PersistenceStressAudit.hpp
    include/audits/PromisesAudit.hpp
    include/audits/ProviderAudit.hpp
    include/audits/ProviderStreamDebugAudit.hpp
    include/audits/QwenProviderAudit.hpp
    include/audits/QwenQuotaAudit.hpp
    include/audits/ReasoningTraceAudit.hpp
    include/audits/ResumeTodoAudit.hpp
    include/audits/SubagentStressAudit.hpp
    include/audits/TokenWasteAudit.hpp
    include/audits/WebFetchAudit.hpp
    include/audits/WebSearchAudit.hpp
    include/audits/WorkflowsAudit.hpp
    include/audits/WorkingMemoryAudit.hpp
    include/audits/WorkingMemoryLiveAudit.hpp
)

target_include_directories(firmius_audits PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(firmius_audits PUBLIC
    firmius::core
    firmius::provider
    firmius::shared
    firmius::server
)

target_include_directories(firmius_audits PRIVATE
  ${rapidjson_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/packages/server/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(firmius_shutdown_hang_repro)
target_sources(firmius_shutdown_hang_repro PRIVATE src/repros/ShutdownHangRepro.cpp)
target_link_libraries(firmius_shutdown_hang_repro PRIVATE firmius::core firmius::shared)
target_include_directories(firmius_shutdown_hang_repro PRIVATE ${rapidjson_SOURCE_DIR}/include)

add_executable(firmius_audit)
target_sources(firmius_audit PRIVATE src/main.cpp)
target_link_libraries(firmius_audit PRIVATE firmius::audits)

target_include_directories(firmius_audit PRIVATE
  ${rapidjson_SOURCE_DIR}/include
)

add_executable(firmius_migrate)
target_sources(firmius_migrate PRIVATE src/migrate_main.cpp)
target_link_libraries(firmius_migrate PRIVATE firmius::audits)
target_include_directories(firmius_migrate PRIVATE ${rapidjson_SOURCE_DIR}/include)
