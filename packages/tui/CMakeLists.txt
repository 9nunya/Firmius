add_executable(firmius)

target_sources(firmius PRIVATE
    src/main.cpp
    src/TUIState.cpp
    src/TUIHotkeys.cpp
    src/ActivePlanState.cpp
    src/WorkPanelLayout.cpp
    src/StreamStateManager.cpp
    src/NotificationManager.cpp
    src/ThemeManager.cpp
    src/UserPreferences.cpp
    src/commands/CommandManager.cpp
    src/commands/NewCommand.cpp
    src/commands/ThreadsCommand.cpp
    src/commands/ModelCommand.cpp
    src/commands/UndoCommand.cpp
    src/commands/CompactCommand.cpp
    src/commands/ConfigCommand.cpp
    src/commands/ConnectCommand.cpp
    src/commands/BenchmarksCommand.cpp
    src/commands/QuotasCommand.cpp
    src/commands/AccountsCommand.cpp
    src/commands/RouterCommand.cpp
    src/commands/PurposesCommand.cpp
    src/commands/WorkflowsCommand.cpp
    src/modals/ModalRegistry.cpp
    src/modals/ThreadPickerModal.cpp
    src/modals/ThreadLockedModal.cpp
    src/modals/ModelPickerModal.cpp
    src/modals/ConfigDisplayModal.cpp
    src/modals/RouterModal.cpp
    src/modals/PurposesModal.cpp
    src/modals/PermissionPromptModal.cpp
    src/modals/OAuthWizardModal.cpp
    src/modals/APIKeyWizardModal.cpp
    src/modals/ConfirmationModal.cpp
    src/modals/QuotasModal.cpp
    src/modals/AccountsModal.cpp
    src/components/ChatWindow.cpp
    src/components/TranscriptGrouping.cpp
    src/components/ErrorDisplay.cpp
    src/components/ToolPresentationBlock.cpp
    src/components/ToolBlock.cpp
    src/components/FileEditDiff.cpp
    src/components/LogWindow.cpp
    src/components/Markdown.cpp
    src/components/ScrollableBox.cpp
    src/components/TitleBar.cpp
    src/components/StatusBar.cpp
    src/components/AgentStrip.cpp
    src/components/PlanLane.cpp
    src/components/TodoLane.cpp
    src/components/InputBar.cpp
    src/components/GlintEffect.cpp
    src/components/HelpOverlay.cpp
    src/components/ANSIParser.cpp
    src/components/SyntaxHighlighter.cpp
    src/tools/ArtifactToolPresentation.cpp
    src/tools/FileToolPresentation.cpp
    src/tools/ProcessToolPresentation.cpp
    src/tools/SubagentToolPresentation.cpp
    src/tools/ToolPresentation.cpp
    src/tools/WorkToolPresentation.cpp
    src/utils/ReferenceAutocomplete.cpp
    src/utils/Clipboard.cpp
)

target_include_directories(firmius PRIVATE src include)

target_link_libraries(firmius PRIVATE
    firmius::core
    ftxui::screen
    ftxui::dom
    ftxui::component
    tree_sitter_all
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
