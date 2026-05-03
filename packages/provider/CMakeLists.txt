find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)

if(TARGET SQLite3::SQLite3)
  set(FIRMIUS_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
  set(FIRMIUS_SQLITE_TARGET SQLite::SQLite3)
endif()
add_library(firmius_provider STATIC)
add_library(firmius::provider ALIAS firmius_provider)

target_sources(firmius_provider PRIVATE
    src/providers/CodexProvider.cpp
    src/providers/ChutesProvider.cpp
    src/providers/KimiProvider.cpp
    src/providers/KiloProvider.cpp
    src/providers/LMStudioProvider.cpp
    src/providers/NanoGPTProvider.cpp
    src/providers/NvidiaProvider.cpp
    src/providers/NvidiaModels.cpp
    src/providers/OpenRouterProvider.cpp
    src/providers/ZaiProvider.cpp
    src/providers/ZenProvider.cpp
    src/providers/BaseOpenAIProvider.cpp
    src/providers/ConfigurableOpenAIProvider.cpp
    src/providers/ConfigurableAnthropicProvider.cpp
    src/providers/RetryPolicyResolver.cpp
    src/providers/ProviderRegistry.cpp
    src/providers/AntigravityProvider.cpp
    src/providers/AntigravityProtocol.cpp
    src/providers/BaseOAuthProvider.cpp
    src/providers/QwenProvider.cpp
    src/providers/KiroProvider.cpp
    src/providers/WindsurfModels.cpp
    src/providers/WindsurfProvider.cpp
    src/providers/WindsurfProvider_grpc.cpp
    src/providers/WindsurfProvider_local.cpp
    src/providers/WindsurfLspManager.cpp
    src/providers/BaseAPIKeyProvider.cpp
     src/providers/BaseAnthropicProvider.cpp
     src/providers/LLMSearchProviderRegistry.cpp
    src/providers/GoogleSearchProvider.cpp
)

target_sources(firmius_provider PUBLIC
  FILE_SET HEADERS
  BASE_DIRS include
  FILES
    include/providers/CodexProvider.hpp
    include/providers/ChutesProvider.hpp
    include/providers/KimiProvider.hpp
    include/providers/KiloProvider.hpp
    include/providers/LMStudioProvider.hpp
    include/providers/NanoGPTProvider.hpp
    include/providers/NvidiaProvider.hpp
    include/providers/NvidiaModels.hpp
    include/providers/OpenRouterProvider.hpp
    include/providers/ZaiProvider.hpp
    include/providers/ZenProvider.hpp
    include/providers/BaseOpenAIProvider.hpp
    include/providers/ConfigurableOpenAIProvider.hpp
    include/providers/ConfigurableAnthropicProvider.hpp
    include/providers/RetryPolicyResolver.hpp
    include/providers/ProviderRegistry.hpp
    include/providers/AntigravityProvider.hpp
    include/providers/AntigravityProtocol.hpp
    include/providers/BaseOAuthProvider.hpp
    include/providers/OAuthWizard.hpp
    include/providers/QwenProvider.hpp
    include/providers/KiroProvider.hpp
    include/providers/WindsurfModels.hpp
    include/providers/WindsurfProvider.hpp
    include/providers/BaseAPIKeyProvider.hpp
    include/providers/BaseAnthropicProvider.hpp
    include/providers/LLMSearchProvider.hpp
    include/providers/LLMSearchProviderRegistry.hpp
    include/providers/GoogleSearchProvider.hpp
)

target_include_directories(firmius_provider PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(firmius_provider PUBLIC
    firmius::shared
    CURL::libcurl
    ${FIRMIUS_SQLITE_TARGET}
)

target_include_directories(firmius_provider PRIVATE
  ${rapidjson_SOURCE_DIR}/include
)
