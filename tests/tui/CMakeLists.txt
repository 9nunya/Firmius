include(GoogleTest)

# All TUI unit tests live in a single executable. Splitting them per-file
# would multiply link time without adding much (every test pulls in the
# whole firmius_tui static library either way).
add_executable(firmius_tui_tests
    AccountsOverlayTest.cpp
    AnsiOutputParserTest.cpp
    AnsiParserTest.cpp
    AppStateTest.cpp
    ArtifactsPresenterTest.cpp
    DaemonSessionTest.cpp
    DelegatePresenterTest.cpp
    DiffRendererTest.cpp
    EditPresenterTest.cpp
    EventRouterTest.cpp
    FleetPresenterTest.cpp
    GlobPresenterTest.cpp
    GrepPresenterTest.cpp
    KeybindRegistryTest.cpp
    ListPresenterTest.cpp
    LspPresenterTest.cpp
    MenuListTest.cpp
    MiscPresenterTest.cpp
    ProcessPresenterTest.cpp
    ReadPresenterTest.cpp
    ToolPresenterRegistryTest.cpp
    TranscriptItemTest.cpp
    TranscriptRendererTest.cpp
    WebPresenterTest.cpp
)

target_link_libraries(firmius_tui_tests PRIVATE
    firmius::tui
    GTest::gtest_main
)

target_compile_features(firmius_tui_tests PRIVATE cxx_std_20)

if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC))
    target_compile_options(firmius_tui_tests PRIVATE -Wall -Wextra -Werror -Wno-error=maybe-uninitialized)
elseif(MSVC)
    target_compile_options(firmius_tui_tests PRIVATE /W4 /WX)
endif()

add_test(NAME firmius_tui_tests COMMAND firmius_tui_tests)
