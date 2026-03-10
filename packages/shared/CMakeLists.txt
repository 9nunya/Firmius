add_library(firmius_shared STATIC)
add_library(firmius::shared ALIAS firmius_shared)

target_sources(firmius_shared PRIVATE
    src/Serialization.cpp
    src/EnvLoader.cpp
    src/Panic.cpp
    src/utils/JSONSchema.cpp
    src/utils/StringUtil.cpp
    src/utils/HistoryMetrics.cpp
    src/utils/FSUtil.cpp
    src/utils/Hashline.cpp
    src/utils/Logger.cpp
    src/utils/ToolSummaries.cpp
    src/utils/TempOAuthServer.cpp
    src/utils/GCPHttpClient.cpp
    src/providers/oauth/BaseOAuthProvider.cpp
    src/providers/antigravity/AntigravityProvider.cpp
    src/providers/antigravity/AntigravityProtocol.cpp
    src/providers/BaseOpenAIProvider.cpp
    src/providers/ProviderRegistry.cpp
    src/ConfigLoader.cpp
)

target_sources(firmius_shared PUBLIC
  FILE_SET HEADERS
  BASE_DIRS include
  FILES
    include/Enums.hpp
    include/Metrics.hpp
    include/Message.hpp
    include/Events.hpp
    include/Context.hpp
    include/Serialization.hpp
    include/IProvider.hpp
    include/EnvLoader.hpp
    include/HarnessEvents.hpp
    include/Panic.hpp
    include/IAgent.hpp
    include/IHost.hpp
    include/IHostProcess.hpp
    include/ITool.hpp
    include/IBenchmark.hpp
    include/HarnessEvents.hpp
    include/utils/JSONSchema.hpp
    include/utils/StringUtil.hpp
    include/utils/HistoryMetrics.hpp
    include/utils/FSUtil.hpp
    include/utils/Hashline.hpp
    include/utils/Logger.hpp
    include/utils/ToolSummaries.hpp
    include/utils/ToolView.hpp
    include/utils/TempOAuthServer.hpp
    include/utils/GCPHttpClient.hpp
    include/providers/oauth/OAuthWizard.hpp
    include/providers/oauth/BaseOAuthProvider.hpp
    include/providers/antigravity/AntigravityProvider.hpp
    include/providers/antigravity/AntigravityProtocol.hpp
    include/providers/BaseOpenAIProvider.hpp
    include/ConfigLoader.hpp
)

target_include_directories(firmius_shared PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_include_directories(firmius_shared SYSTEM PRIVATE
  ${rapidjson_SOURCE_DIR}/include
)

find_package(CURL REQUIRED)
target_link_libraries(firmius_shared PRIVATE CURL::libcurl uuid)
