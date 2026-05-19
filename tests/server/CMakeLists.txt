include(GoogleTest)

add_executable(firmius_server_daemon_ipc_smoke_tests
    DaemonClientIpcSmokeTests.cpp
)
target_link_libraries(firmius_server_daemon_ipc_smoke_tests PRIVATE
    firmius::server
    GTest::gtest_main
)
add_test(NAME firmius_server_daemon_ipc_smoke_tests
         COMMAND firmius_server_daemon_ipc_smoke_tests)

add_executable(firmius_server_tui_parity_audit_tests
    DaemonTuiParityAuditTests.cpp
)
target_link_libraries(firmius_server_tui_parity_audit_tests PRIVATE
    firmius::server
    GTest::gtest_main
)
add_test(NAME firmius_server_tui_parity_audit_tests
         COMMAND firmius_server_tui_parity_audit_tests)
