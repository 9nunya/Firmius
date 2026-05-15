add_executable(firmius_tui_v2)

target_sources(firmius_tui_v2 PRIVATE
    src/main.cpp
    src/App.cpp
    src/DaemonSession.cpp
    src/AppState.cpp
    src/EventRouter.cpp
    src/ActionDispatcher.cpp
    src/Terminal.cpp
    src/TranscriptRenderer.cpp
    src/StatusBar.cpp
    src/InputBar.cpp
    src/BottomBar.cpp
    src/KeybindRegistry.cpp
    src/Layout.cpp
    src/CommandManager.cpp
    src/MenuList.cpp
)

target_include_directories(firmius_tui_v2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(firmius_tui_v2 PRIVATE
    firmius::server
    firmius::shared
)

target_compile_features(firmius_tui_v2 PRIVATE cxx_std_20)

if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC))
    target_compile_options(firmius_tui_v2 PRIVATE -Wall -Wextra -Werror -Wno-error=maybe-uninitialized)
elseif(MSVC)
    target_compile_options(firmius_tui_v2 PRIVATE /W4 /WX)
endif()

# Test target
add_executable(firmius_tui_v2_tests)
target_sources(firmius_tui_v2_tests PRIVATE
    tests/AppStateTest.cpp
    tests/EventRouterTest.cpp
    tests/KeybindRegistryTest.cpp
    tests/TranscriptRendererTest.cpp
    # Link the library sources (not main.cpp) so tests can access them
    src/AppState.cpp
    src/EventRouter.cpp
    src/KeybindRegistry.cpp
    src/TranscriptRenderer.cpp
    src/Terminal.cpp
    src/ActionDispatcher.cpp
    src/DaemonSession.cpp
    src/StatusBar.cpp
    src/InputBar.cpp
    src/BottomBar.cpp
    src/App.cpp
    src/Layout.cpp
    src/CommandManager.cpp
    src/MenuList.cpp
)

target_include_directories(firmius_tui_v2_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(firmius_tui_v2_tests PRIVATE
    firmius::server
    firmius::shared
    GTest::gtest_main
)

target_compile_features(firmius_tui_v2_tests PRIVATE cxx_std_20)

if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC))
    target_compile_options(firmius_tui_v2_tests PRIVATE -Wall -Wextra -Werror -Wno-error=maybe-uninitialized)
elseif(MSVC)
    target_compile_options(firmius_tui_v2_tests PRIVATE /W4 /WX)
endif()

add_test(NAME firmius_tui_v2_tests COMMAND firmius_tui_v2_tests)
