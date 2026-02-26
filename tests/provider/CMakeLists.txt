add_executable(firmius_provider_audit audit.cpp)

target_link_libraries(firmius_provider_audit PRIVATE
    firmius::provider
    firmius::core
)

# RapidJSON includes
target_include_directories(firmius_provider_audit PRIVATE
  ${rapidjson_SOURCE_DIR}/include
)
