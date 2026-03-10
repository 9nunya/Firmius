add_library(firmius_audits STATIC)
add_library(firmius::audits ALIAS firmius_audits)

target_sources(firmius_audits PRIVATE
    src/AuditRegistry.cpp
    src/audits/AntigravityProviderAudit.cpp
    src/audits/AntigravityQuotaAudit.cpp
    src/audits/BenchmarksAudit.cpp
    src/audits/CodexProviderAudit.cpp
    src/audits/CodexQuotaAudit.cpp
    src/audits/HarnessChaosAudit.cpp
    src/audits/P15Audit.cpp
    src/audits/ProviderAudit.cpp
    src/audits/QwenProviderAudit.cpp
    src/audits/QwenQuotaAudit.cpp
    src/audits/SubagentStressAudit.cpp
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
    include/audits/CodexQuotaAudit.hpp
    include/audits/HarnessChaosAudit.hpp
    include/audits/P15Audit.hpp
    include/audits/ProviderAudit.hpp
    include/audits/QwenProviderAudit.hpp
    include/audits/QwenQuotaAudit.hpp
    include/audits/SubagentStressAudit.hpp
)

target_include_directories(firmius_audits PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(firmius_audits PUBLIC
    firmius::core
    firmius::provider
    firmius::shared
)

target_include_directories(firmius_audits PRIVATE
  ${rapidjson_SOURCE_DIR}/include
)

add_executable(firmius_audit)
target_sources(firmius_audit PRIVATE src/main.cpp)
target_link_libraries(firmius_audit PRIVATE firmius::audits)

target_include_directories(firmius_audit PRIVATE
  ${rapidjson_SOURCE_DIR}/include
)
