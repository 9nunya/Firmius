include(GoogleTest)

# TUI Object Library for tests
add_library(firmius_tui_objects OBJECT
    ${CMAKE_SOURCE_DIR}/packages/tui/src/MarkdownParser.cpp
    ${CMAKE_SOURCE_DIR}/packages/tui/src/CommandRegistry.cpp
    ${CMAKE_SOURCE_DIR}/packages/tui/src/ToolRenderer.cpp
    ${CMAKE_SOURCE_DIR}/packages/tui/src/AppState.cpp
    ${CMAKE_SOURCE_DIR}/packages/tui/src/FooterBar.cpp
    ${CMAKE_SOURCE_DIR}/packages/tui/src/SubagentStrip.cpp
    ${CMAKE_SOURCE_DIR}/packages/tui/src/ModalSystem.cpp
)

target_include_directories(firmius_tui_objects PUBLIC
    ${CMAKE_SOURCE_DIR}/packages/tui/include
    ${CMAKE_SOURCE_DIR}/packages/shared/include
    ${CMAKE_SOURCE_DIR}/packages/core/include
    ${rapidjson_SOURCE_DIR}/include
)

target_link_libraries(firmius_tui_objects PUBLIC
    firmius::core
    firmius::shared
    ftxui::dom
    ftxui::screen
    ftxui::component
)

# Markdown Parser Tests
add_executable(test_markdown_parser test_markdown_parser.cpp)
target_link_libraries(test_markdown_parser PRIVATE
    firmius_tui_objects
    GTest::gtest_main
)
gtest_discover_tests(test_markdown_parser)

# Command Registry Tests
add_executable(test_command_registry test_command_registry.cpp)
target_link_libraries(test_command_registry PRIVATE
    firmius_tui_objects
    GTest::gtest_main
)
gtest_discover_tests(test_command_registry)

# Tool Renderer Tests
add_executable(test_tool_renderer test_tool_renderer.cpp)
target_link_libraries(test_tool_renderer PRIVATE
    firmius_tui_objects
    GTest::gtest_main
)
gtest_discover_tests(test_tool_renderer)

# App State Tests
add_executable(test_app_state test_app_state.cpp)
target_link_libraries(test_app_state PRIVATE
    firmius_tui_objects
    GTest::gtest_main
)
gtest_discover_tests(test_app_state)

# Render Tests (FooterBar, SubagentStrip)
add_executable(test_render test_render.cpp)
target_link_libraries(test_render PRIVATE
    firmius_tui_objects
    GTest::gtest_main
)
gtest_discover_tests(test_render)
