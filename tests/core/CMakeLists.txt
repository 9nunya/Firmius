include(GoogleTest)
find_package(CURL REQUIRED)

add_executable(test_permissions test_permissions.cpp)
target_link_libraries(test_permissions PRIVATE
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
)

add_executable(test_process_interactive test_process_interactive.cpp)
target_link_libraries(test_process_interactive PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
    CURL::libcurl
    GTest::gtest_main
)
gtest_discover_tests(test_process_interactive)

add_executable(test_reference_persistence test_reference_persistence.cpp)
target_link_libraries(test_reference_persistence PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
    CURL::libcurl
    GTest::gtest_main
)
target_include_directories(test_reference_persistence PRIVATE ${rapidjson_SOURCE_DIR}/include)
gtest_discover_tests(test_reference_persistence)

add_executable(test_model_rotation test_model_rotation.cpp)
target_link_libraries(test_model_rotation PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
    CURL::libcurl
    GTest::gtest_main
)
target_include_directories(test_model_rotation PRIVATE ${rapidjson_SOURCE_DIR}/include)
gtest_discover_tests(test_model_rotation)

target_include_directories(test_permissions PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_swebench_prep PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_utils PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_fleet PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_process_interactive PRIVATE ${rapidjson_SOURCE_DIR}/include)
target_include_directories(test_reference_persistence PRIVATE ${rapidjson_SOURCE_DIR}/include)
