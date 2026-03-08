add_library(firmius_shared STATIC)
add_library(firmius::shared ALIAS firmius_shared)

target_sources(firmius_shared PRIVATE
    src/Serialization.cpp
    src/EnvLoader.cpp
    src/Panic.cpp
    src/utils/JSONSchema.cpp
    src/utils/StringUtil.cpp
    src/utils/FSUtil.cpp
    src/utils/ToolSummaries.cpp
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
    include/utils/FSUtil.hpp
    include/utils/ToolSummaries.hpp
    include/utils/ToolView.hpp
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
