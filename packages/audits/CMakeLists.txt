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
    src/audits/HarnessChaosAudit.cpp
    src/audits/OAuthWizardAudit.cpp
    src/audits/P15Audit.cpp
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
    src/audits/TuiPerformanceAudit.cpp
    src/audits/ReasoningTraceAudit.cpp
)

target_sources(firmius_audits PUBLIC
  FILE_SET HEADERS
  BASE_DIRS include
  FILES
    include/AuditRegistry.hpp
    include/audits/AntigravityProviderAudit.hpp
    include/audits/AntigravityQuotaAudit.hpp
    include/audits/BenchmarksAudit.hpp
    include/audits/CodexProviderAudit.hpp
    include/audits/ContextBudgetAudit.hpp
    include/audits/EditToolAudit.hpp
    include/audits/EditHistoryAudit.hpp
    include/audits/CodexQuotaAudit.hpp
    include/audits/HarnessChaosAudit.hpp
    include/audits/OAuthWizardAudit.hpp
    include/audits/P15Audit.hpp
    include/audits/ProviderAudit.hpp
    include/audits/ProviderStreamDebugAudit.hpp
    include/audits/LoopCancellationAudit.hpp
    include/audits/LspAudit.hpp
    include/audits/ModelLoadAudit.hpp
    include/audits/OpenRouterQuotaAudit.hpp
    include/audits/QwenProviderAudit.hpp
    include/audits/QwenQuotaAudit.hpp
    include/audits/SubagentStressAudit.hpp
    include/audits/WorkflowsAudit.hpp
    include/audits/ResumeTodoAudit.hpp
    include/audits/WebFetchAudit.hpp
    include/audits/WebSearchAudit.hpp
    include/audits/TuiPerformanceAudit.hpp
    include/audits/ReasoningTraceAudit.hpp
)

target_include_directories(firmius_audits PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(firmius_audits PUBLIC
    firmius::tui
    firmius::core
    firmius::provider
    firmius::shared
)

target_include_directories(firmius_audits PRIVATE
  ${rapidjson_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/packages/tui/include
)

add_executable(firmius_audit)
target_sources(firmius_audit PRIVATE src/main.cpp)
target_link_libraries(firmius_audit PRIVATE firmius::audits)

target_include_directories(firmius_audit PRIVATE
  ${rapidjson_SOURCE_DIR}/include
)
