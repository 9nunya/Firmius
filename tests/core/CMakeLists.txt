include(GoogleTest)
find_package(CURL REQUIRED)

add_executable(firmius_core_audit audit.cpp)
target_link_libraries(firmius_core_audit PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
)

add_executable(p15_audit p15_audit.cpp)
target_link_libraries(p15_audit PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
)

add_executable(antigravity_provider_audit antigravity_provider_audit.cpp)
target_link_libraries(antigravity_provider_audit PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
)

add_executable(subagent_stress_audit subagent_stress_audit.cpp)
target_link_libraries(subagent_stress_audit PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
)

add_executable(test_permissions test_permissions.cpp)
target_link_libraries(test_permissions PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
)

add_executable(codex_provider_audit codex_provider_audit.cpp)
target_link_libraries(codex_provider_audit PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
)

add_executable(harness_chaos_audit harness_chaos_audit.cpp)
target_link_libraries(harness_chaos_audit PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
)

add_executable(test_swebench_prep test_swebench_prep.cpp)
target_link_libraries(test_swebench_prep PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
)

add_executable(test_utils test_utils.cpp)
target_link_libraries(test_utils PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
)
gtest_discover_tests(test_utils)

add_executable(test_fleet test_fleet.cpp)
target_link_libraries(test_fleet PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
    CURL::libcurl
    uuid
)

add_executable(test_process_interactive test_process_interactive.cpp)
target_link_libraries(test_process_interactive PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
    CURL::libcurl
    GTest::gtest_main
    uuid
)
gtest_discover_tests(test_process_interactive)

add_executable(antigravity_quota_audit antigravity_quota_audit.cpp)
target_link_libraries(antigravity_quota_audit PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
)

add_executable(codex_quota_audit codex_quota_audit.cpp)
target_link_libraries(codex_quota_audit PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
)

target_include_directories(firmius_core_audit PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(p15_audit PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(antigravity_provider_audit PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(subagent_stress_audit PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_permissions PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(antigravity_quota_audit PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(codex_provider_audit PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(codex_quota_audit PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(harness_chaos_audit PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_swebench_prep PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_utils PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_fleet PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_process_interactive PRIVATE ${rapidjson_SOURCE_DIR}/include)
