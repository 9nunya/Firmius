add_executable(firmius)

target_sources(firmius PRIVATE
    src/main.cpp
    src/TUIState.cpp
    src/StreamStateManager.cpp
    src/commands/CommandManager.cpp
    src/commands/NewCommand.cpp
    src/commands/ThreadsCommand.cpp
    src/commands/ModelCommand.cpp
    src/commands/UndoCommand.cpp
    src/commands/ConfigCommand.cpp
    src/commands/ConnectCommand.cpp
    src/commands/QuotasCommand.cpp
    src/commands/AccountsCommand.cpp
    src/commands/WorkflowsCommand.cpp
    src/modals/ModalRegistry.cpp
    src/modals/ThreadPickerModal.cpp
    src/modals/ThreadLockedModal.cpp
    src/modals/ModelPickerModal.cpp
    src/modals/ConfigDisplayModal.cpp
    src/modals/OAuthWizardModal.cpp
    src/modals/APIKeyWizardModal.cpp
    src/modals/ConfirmationModal.cpp
    src/modals/QuotasModal.cpp
    src/modals/AccountsModal.cpp
    src/components/ChatWindow.cpp
    src/components/ToolBlock.cpp
    src/components/FileEditToolBlock.cpp
    src/components/FileReadToolBlock.cpp
    src/components/ListDirectoryToolBlock.cpp
    src/components/ProcessExecuteToolBlock.cpp
    src/components/SubagentToolBlock.cpp
    src/components/LogWindow.cpp
    src/components/Markdown.cpp
    src/components/ScrollableBox.cpp
    src/components/TitleBar.cpp
    src/components/StatusBar.cpp
    src/components/AgentStrip.cpp
    src/components/InputBar.cpp
    src/components/GlintEffect.cpp
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
