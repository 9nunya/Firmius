find_package(CURL REQUIRED)

add_library(firmius_provider STATIC)
add_library(firmius::provider ALIAS firmius_provider)

target_sources(firmius_provider PRIVATE 
    src/providers/NanoGPTProvider.cpp
    src/providers/OpenRouterProvider.cpp
    src/providers/ZaiProvider.cpp
    src/providers/ZenProvider.cpp
)

target_sources(firmius_provider PUBLIC 
  FILE_SET HEADERS
  BASE_DIRS include
  FILES
    include/providers/NanoGPTProvider.hpp
    include/providers/OpenRouterProvider.hpp
    include/providers/ZaiProvider.hpp
    include/providers/ZenProvider.hpp
)

target_include_directories(firmius_provider PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(firmius_provider PUBLIC
    firmius::shared
    CURL::libcurl
)

target_include_directories(firmius_provider PRIVATE
  ${rapidjson_SOURCE_DIR}/include
)
