find_package(CURL REQUIRED)

add_library(firmius_provider STATIC)
add_library(firmius::provider ALIAS firmius_provider)

target_sources(firmius_provider PRIVATE 
    src/providers/CodexProvider.cpp
    src/providers/ChutesProvider.cpp
    src/providers/LMStudioProvider.cpp
    src/providers/NanoGPTProvider.cpp
    src/providers/OpenRouterProvider.cpp
    src/providers/ZaiProvider.cpp
    src/providers/ZenProvider.cpp
    src/providers/BaseOpenAIProvider.cpp
    src/providers/ProviderRegistry.cpp
    src/providers/AntigravityProvider.cpp
    src/providers/AntigravityProtocol.cpp
    src/providers/BaseOAuthProvider.cpp
)

target_sources(firmius_provider PUBLIC 
  FILE_SET HEADERS
  BASE_DIRS include
  FILES
    include/providers/CodexProvider.hpp
    include/providers/ChutesProvider.hpp
    include/providers/LMStudioProvider.hpp
    include/providers/NanoGPTProvider.hpp
    include/providers/OpenRouterProvider.hpp
    include/providers/ZaiProvider.hpp
    include/providers/ZenProvider.hpp
    include/providers/BaseOpenAIProvider.hpp
    include/providers/ProviderRegistry.hpp
    include/providers/AntigravityProvider.hpp
    include/providers/AntigravityProtocol.hpp
    include/providers/BaseOAuthProvider.hpp
    include/providers/OAuthWizard.hpp
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
