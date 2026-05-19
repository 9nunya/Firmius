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

# Todo tool tests
add_executable(test_todo_write_tool
    tools/test_todo_write_tool.cpp
)
target_link_libraries(test_todo_write_tool PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_todo_write_tool PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_todo_write_tool)

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

add_executable(test_hooks_platform_audit
    audits/test_hooks_platform_audit.cpp
)
target_link_libraries(test_hooks_platform_audit PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_hooks_platform_audit PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_hooks_platform_audit)

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

add_executable(test_hooks_platform
    core/test_hooks_platform.cpp
)
target_link_libraries(test_hooks_platform PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_hooks_platform PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_hooks_platform)

add_executable(test_hook_agent_action_e2e
    core/test_hook_agent_action_e2e.cpp
)
target_link_libraries(test_hook_agent_action_e2e PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_hook_agent_action_e2e PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_hook_agent_action_e2e)

add_executable(test_hook_agent_stop_event
    core/test_hook_agent_stop_event.cpp
)
target_link_libraries(test_hook_agent_stop_event PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_hook_agent_stop_event PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_hook_agent_stop_event)

add_executable(test_hook_defined_tool_registry
    tools/test_hook_defined_tool_registry.cpp
)
target_link_libraries(test_hook_defined_tool_registry PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_hook_defined_tool_registry PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_hook_defined_tool_registry)

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

add_executable(test_policy_engine
    core/test_policy_engine.cpp
)
target_link_libraries(test_policy_engine PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_policy_engine PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_policy_engine)

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

add_executable(test_mode_registry
    agents/test_mode_registry.cpp
)
target_link_libraries(test_mode_registry PRIVATE
    firmius::core
    firmius::shared
    GTest::gtest_main
    GTest::gmock
)
target_include_directories(test_mode_registry PRIVATE
    ${rapidjson_SOURCE_DIR}/include
)
gtest_discover_tests(test_mode_registry)

add_executable(test_edit_undo
    core/test_edit_undo.cpp
)
target_link_libraries(test_edit_undo PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_edit_undo PRIVATE
    ${rapidjson_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
gtest_discover_tests(test_edit_undo)

add_executable(test_harness_compound_undo
    harness/test_harness_compound_undo.cpp
)
target_link_libraries(test_harness_compound_undo PRIVATE
    firmius::core
    firmius::provider
    firmius::shared
    GTest::gtest_main
    GTest::gmock
    CURL::libcurl
)
target_include_directories(test_harness_compound_undo PRIVATE
    ${rapidjson_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
gtest_discover_tests(test_harness_compound_undo)


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
