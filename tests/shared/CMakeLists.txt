include(GoogleTest)

add_executable(test_serialization test_serialization.cpp)
add_executable(test_tool_summaries test_tool_summaries.cpp)
add_executable(test_hashline test_hashline.cpp)

target_link_libraries(test_serialization PRIVATE
  firmius::shared
  gtest_main
)

target_link_libraries(test_tool_summaries PRIVATE
  firmius::shared
  gtest_main
)

target_link_libraries(test_hashline PRIVATE
  firmius::shared
  gtest_main
)

# RapidJSON includes might be missing for the test target
target_include_directories(test_serialization PRIVATE
  ${rapidjson_SOURCE_DIR}/include
)

target_include_directories(test_tool_summaries PRIVATE
  ${rapidjson_SOURCE_DIR}/include
)

target_include_directories(test_hashline PRIVATE
  ${rapidjson_SOURCE_DIR}/include
)

gtest_discover_tests(test_serialization)
gtest_discover_tests(test_tool_summaries)
gtest_discover_tests(test_hashline)
