include(GoogleTest)

add_executable(firmius_core_audit audit.cpp)
target_link_libraries(firmius_core_audit PRIVATE
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

target_include_directories(firmius_core_audit PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_swebench_prep PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_utils PRIVATE ${rapidjson_SOURCE_DIR}/include)
