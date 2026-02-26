add_executable(firmius_host_audit audit.cpp)

target_link_libraries(firmius_host_audit PRIVATE
    firmius::core
    firmius::shared
)

target_include_directories(firmius_host_audit PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
