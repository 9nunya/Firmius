include(GoogleTest)
find_package(CURL REQUIRED)

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

target_include_directories(firmius_core_audit PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_swebench_prep PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_utils PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_fleet PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_process_interactive PRIVATE ${rapidjson_SOURCE_DIR}/include)

