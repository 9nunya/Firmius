include(GoogleTest)
find_package(CURL REQUIRED)

# Mock test doubles library for isolated unit testing
add_library(firmius_test_mocks INTERFACE)
target_include_directories(firmius_test_mocks INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${rapidjson_SOURCE_DIR}/include
)
target_link_libraries(firmius_test_mocks INTERFACE
    firmius::shared
    firmius::core
)

# Persistence tests
add_executable(test_thread_manager
    persistence/test_thread_manager.cpp
)
target_link_libraries(test_thread_manager PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
)
target_include_directories(test_thread_manager PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_thread_manager)

add_executable(test_journaler
    persistence/test_journaler.cpp
)
target_link_libraries(test_journaler PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
)
target_include_directories(test_journaler PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_journaler)

# Host tests
add_executable(test_localhost
    hosts/test_localhost.cpp
)
target_link_libraries(test_localhost PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    CURL::libcurl
)
target_include_directories(test_localhost PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_localhost)

# Tool tests (with mocks for isolation)
add_executable(test_tools
    tools/test_tools.cpp
)
target_link_libraries(test_tools PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_tools PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_tools)

# Harness tests
add_executable(test_harness
    harness/test_harness.cpp
)
target_link_libraries(test_harness PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
    GTest::gtest_main
    CURL::libcurl
)
target_include_directories(test_harness PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_harness)

# PurposeLoader tests
add_executable(test_purpose_loader
    core/test_purpose_loader.cpp
)
target_link_libraries(test_purpose_loader PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
)
target_include_directories(test_purpose_loader PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_purpose_loader)

# SubagentTool tests
add_executable(test_subagent_tool
    tools/test_subagent_tool.cpp
)
target_link_libraries(test_subagent_tool PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_subagent_tool PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_subagent_tool)

# FSUtil tests
add_executable(test_fsutil
    shared/test_fsutil.cpp
)
target_link_libraries(test_fsutil PRIVATE
    firmius::shared
    GTest::gtest_main
)
target_include_directories(test_fsutil PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_fsutil)

# TUI Markdown tests
add_executable(test_markdown
    tui/test_markdown.cpp
    ../../packages/tui/src/components/Markdown.cpp
)
target_link_libraries(test_markdown PRIVATE
    firmius::shared
    GTest::gtest_main
    ftxui::dom
    ftxui::screen
)
target_include_directories(test_markdown PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_markdown)

# TUI ANSI Parser tests
add_executable(test_ansi_parser
    tui/test_ansi_parser.cpp
    ../../packages/tui/src/components/ANSIParser.cpp
)
target_link_libraries(test_ansi_parser PRIVATE
    firmius::shared
    GTest::gtest_main
    ftxui::dom
    ftxui::screen
)
target_include_directories(test_ansi_parser PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_ansi_parser)

# TUI Syntax Highlighter tests
add_executable(test_syntax_highlighter
    tui/test_syntax_highlighter.cpp
    ../../packages/tui/src/components/SyntaxHighlighter.cpp
    ../../packages/tui/src/NotificationManager.cpp
    ../../packages/tui/src/components/GlintEffect.cpp
)
target_link_libraries(test_syntax_highlighter PRIVATE
    firmius::shared
    GTest::gtest_main
    ftxui::dom
    ftxui::screen
    ftxui::component
)
target_include_directories(test_syntax_highlighter PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_syntax_highlighter)

# TUI Diff Rendering tests
add_executable(test_diff_rendering
    tui/test_diff_rendering.cpp
)
target_link_libraries(test_diff_rendering PRIVATE
    firmius::shared
    GTest::gtest_main
)
target_include_directories(test_diff_rendering PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_diff_rendering)

# Workflow tests
add_executable(test_workflow
    workflow/test_workflow.cpp
)
target_link_libraries(test_workflow PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
)
target_include_directories(test_workflow PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_workflow)
