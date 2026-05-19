add_library(firmius_shared STATIC)
add_library(firmius::shared ALIAS firmius_shared)

target_sources(firmius_shared PRIVATE
    src/Serialization.cpp
    src/EnvLoader.cpp
    src/Panic.cpp
    src/utils/Base64.cpp
    src/utils/JSONSchema.cpp
    src/utils/JSONSchemaFromJson.cpp
    src/utils/JsonUtil.cpp
    src/utils/MathUtil.cpp
    src/utils/StringUtil.cpp
    src/utils/HistoryMetrics.cpp
    src/utils/FSUtil.cpp
    src/utils/LineRange.cpp
    src/utils/Logger.cpp
    src/utils/ErrorCleaner.cpp
    src/utils/ToolSummaries.cpp
    src/utils/TempOAuthServer.cpp
    src/utils/GCPHttpClient.cpp
    src/utils/ModelUtil.cpp
    src/utils/PermissionProfiles.cpp
    src/utils/PlatformPaths.cpp
    src/utils/ShellUtil.cpp
    src/utils/FrontmatterParser.cpp
    src/utils/Hashline.cpp
    src/utils/GlobMatch.cpp
    src/utils/SpillIfLarge.cpp
    src/ConfigLoader.cpp
    src/HeuristicTokenizer.cpp
)

target_sources(firmius_shared PUBLIC
  FILE_SET HEADERS
  BASE_DIRS include
  FILES
    include/ConfigLoader.hpp
    include/Context.hpp
    include/EnvLoader.hpp
    include/Enums.hpp
    include/Events.hpp
    include/HeuristicTokenizer.hpp
    include/IAgent.hpp
    include/IAudit.hpp
    include/IBenchmark.hpp
    include/ICommandIntent.hpp
    include/IEnvironment.hpp
    include/IHost.hpp
    include/IHostProcess.hpp
    include/IPermissions.hpp
    include/IProvider.hpp
    include/ITokenizer.hpp
    include/ITool.hpp
    include/Message.hpp
    include/Metrics.hpp
    include/Panic.hpp
    include/Serialization.hpp
    include/Skill.hpp
    include/providers/BackoffConstants.hpp
    include/utils/AbortController.hpp
    include/utils/Base64.hpp
    include/utils/FSUtil.hpp
    include/utils/FastHash.hpp
    include/utils/FrontmatterParser.hpp
    include/utils/GCPHttpClient.hpp
    include/utils/GlobMatch.hpp
    include/utils/Hashline.hpp
    include/utils/HashUtil.hpp
    include/utils/HistoryMetrics.hpp
    include/utils/Icons.hpp
    include/utils/InterruptibleSleep.hpp
    include/utils/JSONSchema.hpp
    include/utils/JSONSchemaFromJson.hpp
    include/utils/LineRange.hpp
    include/utils/Logger.hpp
    include/utils/MathUtil.hpp
    include/utils/ModelUtil.hpp
    include/utils/PermissionProfiles.hpp
    include/utils/PlatformPaths.hpp
    include/utils/ShellUtil.hpp
    include/utils/SpillIfLarge.hpp
    include/utils/StringUtil.hpp
    include/utils/TempOAuthServer.hpp
    include/utils/TerminalUtil.hpp
    include/utils/ToolSummaries.hpp
    include/utils/ToolView.hpp
)

target_include_directories(firmius_shared PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_include_directories(firmius_shared SYSTEM PRIVATE
  ${rapidjson_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party
)

# Internal headers (not part of the public API) live alongside their .cpp
# counterparts under src/. Adding src/ to the PRIVATE include path lets
# implementation files include them without any "../" path gymnastics.
target_include_directories(firmius_shared PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(CURL REQUIRED)
target_link_libraries(firmius_shared PRIVATE CURL::libcurl)

if(WIN32)
  target_compile_definitions(firmius_shared PRIVATE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
  )
endif()

