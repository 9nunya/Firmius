include(GoogleTest)

add_executable(test_serialization test_serialization.cpp)

target_link_libraries(test_serialization PRIVATE
  firmius::shared
  gtest_main
)

# RapidJSON includes might be missing for the test target
target_include_directories(test_serialization PRIVATE
  ${rapidjson_SOURCE_DIR}/include
)

gtest_discover_tests(test_serialization)
