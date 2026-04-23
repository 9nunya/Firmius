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
    GTest::gmock
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
    GTest::gmock
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
    GTest::gmock
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
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_tools PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_tools)

# Web fetch tool tests (regression + real URL fetch)
add_executable(test_web_fetch
    tools/test_web_fetch.cpp
)
target_link_libraries(test_web_fetch PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_web_fetch PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_web_fetch)

add_executable(test_artifact_tools
    tools/test_artifact_tools.cpp
)
target_link_libraries(test_artifact_tools PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    GTest::gmock
)
target_include_directories(test_artifact_tools PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_artifact_tools)

add_executable(test_work_tools
    tools/test_work_tools.cpp
)
target_link_libraries(test_work_tools PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_work_tools PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_work_tools)

# Fleet lock tests
add_executable(test_fleet_lock
    tools/test_fleet_lock.cpp
)
target_link_libraries(test_fleet_lock PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_fleet_lock PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_fleet_lock)

# Fleet lock regression tests (documented test scenarios)
add_executable(test_fleet_lock_regression
    tools/test_fleet_lock_regression.cpp
)
target_link_libraries(test_fleet_lock_regression PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_fleet_lock_regression PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_fleet_lock_regression)

# Chunk dependency tests
add_executable(test_chunk_dependencies
    tools/test_chunk_dependencies.cpp
)
target_link_libraries(test_chunk_dependencies PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_chunk_dependencies PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_chunk_dependencies)

# Harness tests
add_executable(test_harness
    harness/test_harness.cpp
)
target_link_libraries(test_harness PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_harness PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_harness)

add_executable(test_qwen_provider
    provider/test_qwen_provider.cpp
)
target_link_libraries(test_qwen_provider PRIVATE
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_qwen_provider PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_qwen_provider)

add_executable(test_antigravity_provider
    provider/test_antigravity_provider.cpp
)
target_link_libraries(test_antigravity_provider PRIVATE
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_antigravity_provider PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_antigravity_provider)

add_executable(test_codex_provider
    provider/test_codex_provider.cpp
)
target_link_libraries(test_codex_provider PRIVATE
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_codex_provider PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_codex_provider)

add_executable(test_base_openai_provider
    provider/test_base_openai_provider.cpp
)
target_link_libraries(test_base_openai_provider PRIVATE
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_base_openai_provider PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_base_openai_provider)

add_executable(test_base_api_key_provider
    provider/test_base_api_key_provider.cpp
)
target_link_libraries(test_base_api_key_provider PRIVATE
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_base_api_key_provider PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_base_api_key_provider)

add_executable(test_base_oauth_provider
    provider/test_base_oauth_provider.cpp
)
target_link_libraries(test_base_oauth_provider PRIVATE
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_base_oauth_provider PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_base_oauth_provider)

add_executable(test_cli_options
    cli/test_cli_options.cpp
)
target_link_libraries(test_cli_options PRIVATE
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_cli_options PRIVATE
    ${PROJECT_SOURCE_DIR}/packages/cli/include
    ${PROJECT_SOURCE_DIR}/packages/shared/include
)
gtest_discover_tests(test_cli_options)

add_executable(test_nvidia_provider
    provider/test_nvidia_provider.cpp
)
target_link_libraries(test_nvidia_provider PRIVATE
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_nvidia_provider PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_nvidia_provider)

add_executable(test_openrouter_provider
    provider/test_openrouter_provider.cpp
)
target_link_libraries(test_openrouter_provider PRIVATE
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_openrouter_provider PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_openrouter_provider)

add_executable(test_kiro_provider
    provider/test_kiro_provider.cpp
)
target_link_libraries(test_kiro_provider PRIVATE
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_kiro_provider PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_kiro_provider)

add_executable(test_provider_stream_debug_audit
    audits/test_provider_stream_debug_audit.cpp
)
target_link_libraries(test_provider_stream_debug_audit PRIVATE
    firmius::audits
    firmius::provider
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_provider_stream_debug_audit PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_provider_stream_debug_audit)

add_executable(test_lsp_audit
    audits/test_lsp_audit.cpp
)
target_link_libraries(test_lsp_audit PRIVATE
    firmius::audits
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_lsp_audit PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_lsp_audit)

# PurposeLoader tests
add_executable(test_purpose_loader
    core/test_purpose_loader.cpp
)
target_link_libraries(test_purpose_loader PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_purpose_loader PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_purpose_loader)

add_executable(test_engine_images
    core/test_engine_images.cpp
)
target_link_libraries(test_engine_images PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_engine_images PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_engine_images)

add_executable(test_engine_stream_events
    core/test_engine_stream_events.cpp
)
target_link_libraries(test_engine_stream_events PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_engine_stream_events PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_engine_stream_events)

add_executable(test_engine_shutdown
    core/test_engine_shutdown.cpp
)
add_dependencies(test_engine_shutdown lsp_test_stub_server)
target_link_libraries(test_engine_shutdown PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
    firmius_test_mocks
    GTest::gtest_main
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_engine_shutdown PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
target_compile_definitions(test_engine_shutdown PRIVATE
    FIRMIUS_LSP_TEST_STUB_SERVER_PATH="$<TARGET_FILE:lsp_test_stub_server>"
)
gtest_discover_tests(test_engine_shutdown)

add_executable(test_process_manager
    core/test_process_manager.cpp
)
target_link_libraries(test_process_manager PRIVATE
    firmius::core
    firmius::shared
    firmius_test_mocks
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_process_manager PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_process_manager)

add_executable(test_environment
    core/test_environment.cpp
)
target_link_libraries(test_environment PRIVATE
    firmius::core
    firmius::shared
    firmius_test_mocks
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_environment PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_environment)

add_executable(test_json_rpc_transport
    lsp/test_json_rpc_transport.cpp
)
target_link_libraries(test_json_rpc_transport PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_json_rpc_transport PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_json_rpc_transport)

add_executable(lsp_test_stub_server
    lsp/lsp_test_stub_server.cpp
)
target_link_libraries(lsp_test_stub_server PRIVATE firmius::shared)

add_executable(test_lsp_protocol
    lsp/test_lsp_protocol.cpp
)
target_link_libraries(test_lsp_protocol PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_lsp_protocol PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_lsp_protocol)

add_executable(test_lsp_server_registry
    lsp/test_lsp_server_registry.cpp
)
target_link_libraries(test_lsp_server_registry PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_lsp_server_registry PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_lsp_server_registry)

add_executable(test_lsp_server_manager
    lsp/test_lsp_server_manager.cpp
)
add_dependencies(test_lsp_server_manager lsp_test_stub_server)
target_link_libraries(test_lsp_server_manager PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_lsp_server_manager PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
target_compile_definitions(test_lsp_server_manager PRIVATE
    FIRMIUS_LSP_TEST_STUB_SERVER_PATH="$<TARGET_FILE:lsp_test_stub_server>"
)
gtest_discover_tests(test_lsp_server_manager)


add_executable(test_benchmark_factory
    core/test_benchmark_factory.cpp
)
target_link_libraries(test_benchmark_factory PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_benchmark_factory PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_benchmark_factory)

add_executable(test_reference_expansion
    core/test_reference_expansion.cpp
)
target_link_libraries(test_reference_expansion PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_reference_expansion PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_reference_expansion)

# SubagentTool tests
add_executable(test_subagent_tool
    tools/test_subagent_tool.cpp
)
target_link_libraries(test_subagent_tool PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    GTest::gmock
)
target_include_directories(test_subagent_tool PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_subagent_tool)

add_executable(test_subagent_artifacts
    tools/test_subagent_artifacts.cpp
)
target_link_libraries(test_subagent_artifacts PRIVATE
    firmius::core
    firmius::shared
    firmius::provider
    GTest::gtest_main
    GTest::gmock
    GTest::gmock
)
target_include_directories(test_subagent_artifacts PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_subagent_artifacts)

# FSUtil tests
add_executable(test_fsutil
    shared/test_fsutil.cpp
)
target_link_libraries(test_fsutil PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_fsutil PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_fsutil)

# ModelUtil tests
add_executable(test_model_util
    shared/test_model_util.cpp
)
target_link_libraries(test_model_util PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_model_util PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_model_util)

add_executable(test_config_loader
    shared/test_config_loader.cpp
)
target_link_libraries(test_config_loader PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_config_loader PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_config_loader)

# TUI Markdown tests
add_executable(test_markdown
    tui/test_markdown.cpp
    ../../packages/tui/src/components/Markdown.cpp
)
target_link_libraries(test_markdown PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
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
    GTest::gmock
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
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/UserPreferences.cpp
    ../../packages/tui/src/components/GlintEffect.cpp
)
target_link_libraries(test_syntax_highlighter PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    ftxui::dom
    ftxui::screen
    ftxui::component
    tree_sitter_all
)
target_include_directories(test_syntax_highlighter PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_syntax_highlighter)

# TUI Diff Rendering tests
add_executable(test_diff_rendering
    tui/test_diff_rendering.cpp
    ../../packages/tui/src/components/FileEditDiff.cpp
)
target_link_libraries(test_diff_rendering PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_diff_rendering PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_diff_rendering)

add_executable(test_tui_hotkeys
    tui/test_tui_hotkeys.cpp
    ../../packages/tui/src/TUIHotkeys.cpp
)
target_link_libraries(test_tui_hotkeys PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    ftxui::component
)
target_include_directories(test_tui_hotkeys PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_tui_hotkeys)

add_executable(test_input_bar
    tui/test_input_bar.cpp
)
target_link_libraries(test_input_bar PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_input_bar PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
    ../../packages/core/include
)
gtest_discover_tests(test_input_bar)

add_executable(test_reference_autocomplete
    tui/test_reference_autocomplete.cpp
    ../../packages/tui/src/utils/ReferenceAutocomplete.cpp
)
target_link_libraries(test_reference_autocomplete PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_reference_autocomplete PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_reference_autocomplete)

add_executable(test_active_plan_state
    tui/test_active_plan_state.cpp
    ../../packages/tui/src/ActivePlanState.cpp
)
target_link_libraries(test_active_plan_state PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_active_plan_state PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_active_plan_state)

add_executable(test_stream_state_manager_history
    tui/test_stream_state_manager_history.cpp
    ../../packages/tui/src/StreamStateManager.cpp
)
target_link_libraries(test_stream_state_manager_history PRIVATE
    firmius::shared
    firmius::core
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_stream_state_manager_history PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
    ../../packages/core/include
    ../../packages/shared/include
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_stream_state_manager_history)

add_executable(test_stream_state_manager_live
    tui/test_stream_state_manager_live.cpp
    ../../packages/tui/src/StreamStateManager.cpp
)
target_link_libraries(test_stream_state_manager_live PRIVATE
    firmius::shared
    firmius::core
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_stream_state_manager_live PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
    ../../packages/core/include
    ../../packages/shared/include
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_stream_state_manager_live)

add_executable(test_transcript_grouping
    tui/test_transcript_grouping.cpp
    ../../packages/tui/src/components/TranscriptGrouping.cpp
)
target_link_libraries(test_transcript_grouping PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_transcript_grouping PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_transcript_grouping)

add_executable(test_chat_window_helpers
    tui/test_chat_window_helpers.cpp
    ../../packages/tui/src/components/ChatWindow.cpp
    ../../packages/tui/src/components/DiffRenderer.cpp
    ../../packages/tui/src/components/FileEditDiff.cpp
    ../../packages/tui/src/components/ToolBlock.cpp
    ../../packages/tui/src/components/ToolPresentationBlock.cpp
    ../../packages/tui/src/components/ErrorDisplay.cpp
    ../../packages/tui/src/components/Markdown.cpp
    ../../packages/tui/src/components/ScrollableBox.cpp
    ../../packages/tui/src/components/GlintEffect.cpp
    ../../packages/tui/src/components/SyntaxHighlighter.cpp
    ../../packages/tui/src/components/TranscriptGrouping.cpp
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/components/ANSIParser.cpp
    ../../packages/tui/src/UserPreferences.cpp
    ../../packages/tui/src/NotificationManager.cpp
    ../../packages/tui/src/utils/Clipboard.cpp
    ../../packages/tui/src/tools/ArtifactToolPresentation.cpp
    ../../packages/tui/src/tools/FileToolPresentation.cpp
    ../../packages/tui/src/tools/ToolPresentation.cpp
    ../../packages/tui/src/tools/ProcessToolPresentation.cpp
    ../../packages/tui/src/tools/PythonToolPresentation.cpp
    ../../packages/tui/src/tools/SubagentToolPresentation.cpp
    ../../packages/tui/src/tools/WebSearchToolPresentation.cpp
    ../../packages/tui/src/tools/McpToolPresentation.cpp
    ../../packages/tui/src/tools/WorkToolPresentation.cpp
    ../../packages/tui/src/tools/SearchToolPresentation.cpp
    ../../packages/tui/src/tools/SemanticToolPresentation.cpp
    ../../packages/tui/src/tools/GenericToolPresentation.cpp
)
target_link_libraries(test_chat_window_helpers PRIVATE
    firmius::shared
    firmius::core
    tree_sitter_all
    ftxui::dom
    ftxui::screen
    ftxui::component
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_chat_window_helpers PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
    ../../packages/core/include
    ../../packages/shared/include
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_chat_window_helpers)

add_executable(test_chat_window_quick_tool_grouping
    tui/test_chat_window_quick_tool_grouping.cpp
    ../../packages/tui/src/components/ChatWindow.cpp
    ../../packages/tui/src/components/DiffRenderer.cpp
    ../../packages/tui/src/components/FileEditDiff.cpp
    ../../packages/tui/src/components/ToolBlock.cpp
    ../../packages/tui/src/components/ToolPresentationBlock.cpp
    ../../packages/tui/src/components/ErrorDisplay.cpp
    ../../packages/tui/src/components/Markdown.cpp
    ../../packages/tui/src/components/ScrollableBox.cpp
    ../../packages/tui/src/components/GlintEffect.cpp
    ../../packages/tui/src/components/SyntaxHighlighter.cpp
    ../../packages/tui/src/components/TranscriptGrouping.cpp
    ../../packages/tui/src/components/ANSIParser.cpp
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/UserPreferences.cpp
    ../../packages/tui/src/NotificationManager.cpp
    ../../packages/tui/src/utils/Clipboard.cpp
    ../../packages/tui/src/tools/ArtifactToolPresentation.cpp
    ../../packages/tui/src/tools/FileToolPresentation.cpp
    ../../packages/tui/src/tools/ToolPresentation.cpp
    ../../packages/tui/src/tools/ProcessToolPresentation.cpp
    ../../packages/tui/src/tools/PythonToolPresentation.cpp
    ../../packages/tui/src/tools/SubagentToolPresentation.cpp
    ../../packages/tui/src/tools/WebSearchToolPresentation.cpp
    ../../packages/tui/src/tools/McpToolPresentation.cpp
    ../../packages/tui/src/tools/WorkToolPresentation.cpp
    ../../packages/tui/src/tools/SearchToolPresentation.cpp
    ../../packages/tui/src/tools/SemanticToolPresentation.cpp
    ../../packages/tui/src/tools/GenericToolPresentation.cpp
)
target_link_libraries(test_chat_window_quick_tool_grouping PRIVATE
    firmius::shared
    firmius::core
    tree_sitter_all
    ftxui::dom
    ftxui::screen
    ftxui::component
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_chat_window_quick_tool_grouping PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
    ../../packages/core/include
    ../../packages/shared/include
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_chat_window_quick_tool_grouping)

add_executable(test_model_picker_entries
    tui/test_model_picker_entries.cpp
)
target_link_libraries(test_model_picker_entries PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_model_picker_entries PRIVATE
    ../../packages/tui/include
    ../../packages/shared/include
)
gtest_discover_tests(test_model_picker_entries)

add_executable(test_help_overlay
    tui/test_help_overlay.cpp
    ../../packages/tui/src/components/Markdown.cpp
    ../../packages/tui/src/components/ScrollableBox.cpp
)
target_link_libraries(test_help_overlay PRIVATE
    GTest::gtest_main
    GTest::gmock
    ftxui::dom
    ftxui::screen
    ftxui::component
)
target_include_directories(test_help_overlay PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_help_overlay)

add_executable(test_error_display
    tui/test_error_display.cpp
    ../../packages/tui/src/components/ErrorDisplay.cpp
    ../../packages/tui/src/components/GlintEffect.cpp
    ../../packages/tui/src/components/Markdown.cpp
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/UserPreferences.cpp
)
target_link_libraries(test_error_display PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    ftxui::dom
    ftxui::screen
    ftxui::component
    tree_sitter_all
)
target_include_directories(test_error_display PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_error_display)

add_executable(test_plan_lane
    tui/test_plan_lane.cpp
    ../../packages/tui/src/components/PlanLane.cpp
    ../../packages/tui/src/components/GlintEffect.cpp
    ../../packages/tui/src/components/Markdown.cpp
    ../../packages/tui/src/components/ScrollableBox.cpp
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/UserPreferences.cpp
)
target_link_libraries(test_plan_lane PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    ftxui::dom
    ftxui::screen
    ftxui::component
)
target_include_directories(test_plan_lane PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_plan_lane)

add_executable(test_todo_lane
    tui/test_todo_lane.cpp
    ../../packages/tui/src/components/TodoLane.cpp
    ../../packages/tui/src/components/Markdown.cpp
    ../../packages/tui/src/components/ScrollableBox.cpp
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/UserPreferences.cpp
)
target_link_libraries(test_todo_lane PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    ftxui::dom
    ftxui::screen
    ftxui::component
)
target_include_directories(test_todo_lane PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_todo_lane)

add_executable(test_context_lane
    tui/test_context_lane.cpp
    ../../packages/tui/src/components/ContextLane.cpp
    ../../packages/tui/src/components/Markdown.cpp
    ../../packages/tui/src/components/ScrollableBox.cpp
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/UserPreferences.cpp
)
target_link_libraries(test_context_lane PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    ftxui::dom
    ftxui::screen
    ftxui::component
)
target_include_directories(test_context_lane PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_context_lane)

add_executable(test_work_panel_layout
    tui/test_work_panel_layout.cpp
    ../../packages/tui/src/WorkPanelLayout.cpp
)
target_link_libraries(test_work_panel_layout PRIVATE
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_work_panel_layout PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_work_panel_layout)

add_executable(test_status_bar
    tui/test_status_bar.cpp
    ../../packages/tui/src/components/StatusBar.cpp
    ../../packages/tui/src/components/GlintEffect.cpp
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/UserPreferences.cpp
)
target_link_libraries(test_status_bar PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    ftxui::dom
    ftxui::screen
    ftxui::component
)
target_include_directories(test_status_bar PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_status_bar)

add_executable(test_agent_strip
    tui/test_agent_strip.cpp
    ../../packages/tui/src/components/AgentStrip.cpp
    ../../packages/tui/src/components/Markdown.cpp
    ../../packages/tui/src/components/GlintEffect.cpp
    ../../packages/tui/src/components/ScrollableBox.cpp
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/UserPreferences.cpp
)
target_link_libraries(test_agent_strip PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    ftxui::dom
    ftxui::screen
    ftxui::component
)
target_include_directories(test_agent_strip PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_agent_strip)

add_executable(test_tool_presentation
    tui/test_tool_presentation.cpp
    ../../packages/tui/src/components/FileEditDiff.cpp
    ../../packages/tui/src/components/SyntaxHighlighter.cpp
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/UserPreferences.cpp
    ../../packages/tui/src/NotificationManager.cpp
    ../../packages/tui/src/components/GlintEffect.cpp
    ../../packages/tui/src/tools/ArtifactToolPresentation.cpp
    ../../packages/tui/src/tools/FileToolPresentation.cpp
    ../../packages/tui/src/tools/ProcessToolPresentation.cpp
    ../../packages/tui/src/tools/SubagentToolPresentation.cpp
    ../../packages/tui/src/tools/WebSearchToolPresentation.cpp
    ../../packages/tui/src/tools/PythonToolPresentation.cpp
    ../../packages/tui/src/tools/ToolPresentation.cpp
    ../../packages/tui/src/tools/McpToolPresentation.cpp
    ../../packages/tui/src/tools/WorkToolPresentation.cpp
    ../../packages/tui/src/tools/SearchToolPresentation.cpp
    ../../packages/tui/src/tools/SemanticToolPresentation.cpp
    ../../packages/tui/src/tools/GenericToolPresentation.cpp
)
target_link_libraries(test_tool_presentation PRIVATE
    firmius::shared
    firmius::core
    ftxui::dom
    ftxui::screen
    ftxui::component
    tree_sitter_all
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_tool_presentation PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_tool_presentation)
add_executable(test_permission_prompt_modal
    tui/test_permission_prompt_modal.cpp
    ../../packages/tui/src/modals/PermissionPromptModal.cpp
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/UserPreferences.cpp
)
target_link_libraries(test_permission_prompt_modal PRIVATE
    firmius::shared
    firmius::core
    GTest::gtest_main
    GTest::gmock
    ftxui::dom
    ftxui::screen
    ftxui::component
)
target_include_directories(test_permission_prompt_modal PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_permission_prompt_modal)



add_executable(test_notification_manager
    tui/test_notification_manager.cpp
    ../../packages/tui/src/NotificationManager.cpp
    ../../packages/tui/src/components/GlintEffect.cpp
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/UserPreferences.cpp
    ../../packages/tui/src/components/Markdown.cpp
    ../../packages/tui/src/components/ScrollableBox.cpp
)
target_link_libraries(test_notification_manager PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    ftxui::dom
    ftxui::screen
    ftxui::component
)
target_include_directories(test_notification_manager PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_notification_manager)

add_executable(test_notification_layering
    tui/test_notification_layering.cpp
    ../../packages/tui/src/NotificationManager.cpp
    ../../packages/tui/src/components/GlintEffect.cpp
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/UserPreferences.cpp
    ../../packages/tui/src/components/Markdown.cpp
    ../../packages/tui/src/components/ScrollableBox.cpp
)
target_link_libraries(test_notification_layering PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    ftxui::dom
    ftxui::screen
    ftxui::component
)
target_include_directories(test_notification_layering PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
)
gtest_discover_tests(test_notification_layering)
add_executable(test_tool_presentation_block
    tui/test_tool_presentation_block.cpp
    ../../packages/tui/src/components/DiffRenderer.cpp
    ../../packages/tui/src/components/FileEditDiff.cpp
    ../../packages/tui/src/components/SyntaxHighlighter.cpp
    ../../packages/tui/src/components/ToolPresentationBlock.cpp
    ../../packages/tui/src/components/ANSIParser.cpp
    ../../packages/tui/src/components/GlintEffect.cpp
    ../../packages/tui/src/tools/ArtifactToolPresentation.cpp
    ../../packages/tui/src/tools/FileToolPresentation.cpp
    ../../packages/tui/src/tools/ProcessToolPresentation.cpp
    ../../packages/tui/src/tools/SubagentToolPresentation.cpp
    ../../packages/tui/src/tools/WebSearchToolPresentation.cpp
    ../../packages/tui/src/tools/PythonToolPresentation.cpp
    ../../packages/tui/src/tools/ToolPresentation.cpp
    ../../packages/tui/src/tools/McpToolPresentation.cpp
    ../../packages/tui/src/tools/WorkToolPresentation.cpp
    ../../packages/tui/src/tools/SearchToolPresentation.cpp
    ../../packages/tui/src/tools/SemanticToolPresentation.cpp
    ../../packages/tui/src/tools/GenericToolPresentation.cpp
    ../../packages/tui/src/ThemeManager.cpp
    ../../packages/tui/src/UserPreferences.cpp
    ../../packages/tui/src/ActivePlanState.cpp
    ../../packages/tui/src/NotificationManager.cpp
    ../../packages/tui/src/StreamStateManager.cpp
)
target_link_libraries(test_tool_presentation_block PRIVATE
    firmius::shared
    firmius::core
    GTest::gtest_main
    GTest::gmock
    ftxui::dom
    ftxui::screen
    ftxui::component
    tree_sitter_all
)
target_include_directories(test_tool_presentation_block PRIVATE
    ../../packages/tui/include
    ../../packages/tui/src
    ../../packages/core/include
    ../../packages/shared/include
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_tool_presentation_block)

# Workflow tests
add_executable(test_workflow
    workflow/test_workflow.cpp
)
target_link_libraries(test_workflow PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_workflow PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_workflow)

# CommandIntentAnalyzer tests
add_executable(test_command_intent_analyzer
    core/test_command_intent_analyzer.cpp
)
target_link_libraries(test_command_intent_analyzer PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    GTest::gmock
)
target_include_directories(test_command_intent_analyzer PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_command_intent_analyzer)

add_executable(test_workspace
    core/test_workspace.cpp
)
target_link_libraries(test_workspace PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_workspace PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_workspace)

add_executable(test_runtime_overlay
    core/test_runtime_overlay.cpp
)
target_link_libraries(test_runtime_overlay PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    GTest::gmock
)
target_include_directories(test_runtime_overlay PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_runtime_overlay)

add_executable(test_rolling_context_manager
    core/test_rolling_context_manager.cpp
)
target_link_libraries(test_rolling_context_manager PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_rolling_context_manager PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_rolling_context_manager)
add_executable(test_stream_insanity
    core/test_stream_insanity.cpp
)
target_link_libraries(test_stream_insanity PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_stream_insanity PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_stream_insanity)

add_executable(test_memory_recall_tool
    tools/test_memory_recall_tool.cpp
)
target_link_libraries(test_memory_recall_tool PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_memory_recall_tool PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_memory_recall_tool)

add_executable(test_runtime_overlay_security
    core/test_runtime_overlay_security.cpp
)
target_link_libraries(test_runtime_overlay_security PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    GTest::gmock
)
target_include_directories(test_runtime_overlay_security PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_runtime_overlay_security)

# TerminalUtil unit tests
add_executable(test_terminal_util
    shared/test_terminal_util.cpp
)
target_link_libraries(test_terminal_util PRIVATE
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_terminal_util PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_terminal_util)

# ProcessInput regression tests
add_executable(test_process_input_regression
    tools/test_process_input_regression.cpp
)
target_link_libraries(test_process_input_regression PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_process_input_regression PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_process_input_regression)

