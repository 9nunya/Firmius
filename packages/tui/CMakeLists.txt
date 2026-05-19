add_library(firmius_tui STATIC)
add_library(firmius::tui ALIAS firmius_tui)

target_sources(firmius_tui PRIVATE
    src/App.cpp
    src/DaemonSession.cpp
    src/AppState.cpp
    src/EventRouter.cpp
    src/ActionDispatcher.cpp
    src/Terminal.cpp
    src/AnsiParser.cpp
    src/GlintRenderer.cpp
    src/ThemeManager.cpp
    src/StatusBar.cpp
    src/AgentTabBar.cpp
    src/InputBar.cpp
    src/BottomBar.cpp
    src/KeybindRegistry.cpp
    src/Layout.cpp
    src/CommandManager.cpp
    src/WorkflowCommand.cpp
    src/MenuList.cpp
    src/InfoOverlay.cpp
    src/AccountsOverlay.cpp
    src/ConnectOverlay.cpp
    src/RewindOverlay.cpp
    src/RedoOverlay.cpp
    src/RouterOverlay.cpp
    src/PurposesOverlay.cpp
    src/PermissionPromptOverlay.cpp
    src/PermissionsOverlay.cpp
    src/Clipboard.cpp
    src/NotificationCenter.cpp
    src/NotificationStrip.cpp
    # Items
    src/items/SimpleItems.cpp
    src/items/StreamingItems.cpp
    src/items/ToolCallItem.cpp
    src/items/QuickToolClusterItem.cpp
    # Utilities
    src/DiffRenderer.cpp
    src/AnsiOutputParser.cpp
    src/SyntaxHighlighter.cpp
    # Presenter system
    src/tools/ToolPresenterRegistry.cpp
    src/tools/ProcessPresenter.cpp
    src/tools/DelegatePresenter.cpp
    src/tools/EditPresenter.cpp
    src/tools/ReadPresenter.cpp
    src/tools/ListPresenter.cpp
    src/tools/GrepPresenter.cpp
    src/tools/GlobPresenter.cpp
    src/tools/ArtifactsPresenter.cpp
    src/tools/WebPresenter.cpp
    src/tools/LspPresenter.cpp
    src/tools/FleetPresenter.cpp
    src/tools/TodoPresenter.cpp
    src/tools/ModeSwitchPresenter.cpp
    src/tools/SkillPresenter.cpp
    src/tools/McpPresenter.cpp
    src/tools/GenericPresenter.cpp
)

target_include_directories(firmius_tui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(firmius_tui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(firmius_tui PUBLIC
    firmius::server
    firmius::shared
)

target_link_libraries(firmius_tui PRIVATE
    tree_sitter_all
)

target_compile_features(firmius_tui PUBLIC cxx_std_20)

if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC))
    target_compile_options(firmius_tui PRIVATE
        -Wall -Wextra -Werror
        # -Wmaybe-uninitialized is GCC-only; Clang doesn't have it.
        $<$<CXX_COMPILER_ID:GNU>:-Wno-error=maybe-uninitialized>)
elseif(MSVC)
    target_compile_options(firmius_tui PRIVATE /W4 /WX)
endif()
