add_executable(firmius)

target_sources(firmius PRIVATE
    src/main.cpp
    src/AppState.cpp
    src/ChatRenderer.cpp
    src/CommandHelper.cpp
    src/CommandRegistry.cpp
    src/EventBridge.cpp
    src/FooterBar.cpp
    src/InputComponent.cpp
    src/MarkdownParser.cpp
    src/ModalSystem.cpp
    src/SubagentStrip.cpp
    src/ToolRenderer.cpp
    src/WelcomeScreen.cpp
)

target_include_directories(firmius PRIVATE src include)

target_link_libraries(firmius PRIVATE
    firmius::core
    ftxui::component
    ftxui::dom
    ftxui::screen
)

# Include rapidjson headers
target_include_directories(firmius PRIVATE ${rapidjson_SOURCE_DIR}/include)

# Same compiler warnings as rest of project
target_compile_options(firmius PRIVATE
    -Wall -Wextra -Werror
)
