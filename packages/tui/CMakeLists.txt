add_executable(firmius)

target_sources(firmius PRIVATE
    src/main.cpp
    src/TUIState.cpp
    src/components/ChatWindow.cpp
    src/components/TurnBlock.cpp
    src/components/ToolBlock.cpp
    src/components/Markdown.cpp
    src/components/ScrollableBox.cpp
    src/components/TitleBar.cpp
    src/components/StatusBar.cpp
    src/components/InputBar.cpp
)

target_include_directories(firmius PRIVATE src include)

target_link_libraries(firmius PRIVATE
    firmius::core
    ftxui::screen
    ftxui::dom
    ftxui::component
)

# Include rapidjson headers
target_include_directories(firmius PRIVATE
    ${rapidjson_SOURCE_DIR}/include
    ${ftxui_SOURCE_DIR}/include
)

# Same compiler warnings as rest of project
target_compile_options(firmius PRIVATE
    -Wall -Wextra -Werror
)
