add_library(firmius_server STATIC)
add_library(firmius::server ALIAS firmius_server)

target_sources(firmius_server PRIVATE
    src/daemon/ProtocolSerialization.cpp
    src/daemon/SocketTransport.cpp
    src/daemon/DaemonService.cpp
    src/daemon/DaemonServer.cpp
    src/daemon/DaemonClient.cpp
)

target_sources(firmius_server PUBLIC
  FILE_SET HEADERS
  BASE_DIRS include
  FILES
    include/daemon/DaemonClient.hpp
    include/daemon/DaemonLog.hpp
    include/daemon/DaemonServer.hpp
    include/daemon/DaemonService.hpp
    include/daemon/Protocol.hpp
    include/daemon/ProtocolSerialization.hpp
    include/daemon/SocketTransport.hpp
)

target_include_directories(firmius_server PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(firmius_server PUBLIC
  firmius::core
  firmius::shared
)

if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC))
  target_compile_options(firmius_server PRIVATE -Wall -Wextra -Werror)
elseif(MSVC)
  target_compile_options(firmius_server PRIVATE /W4 /WX)
endif()

add_executable(firmiusd)
target_sources(firmiusd PRIVATE src/main.cpp)
target_link_libraries(firmiusd PRIVATE firmius::server)

add_executable(firmius_server_daemon_ipc_smoke_tests)
target_sources(firmius_server_daemon_ipc_smoke_tests PRIVATE
  src/daemon/DaemonClientIpcSmokeTests.cpp
)
target_link_libraries(firmius_server_daemon_ipc_smoke_tests PRIVATE
  firmius::server
  GTest::gtest_main
)
add_test(NAME firmius_server_daemon_ipc_smoke_tests COMMAND firmius_server_daemon_ipc_smoke_tests)

add_executable(firmius_server_tui_parity_audit_tests)
target_sources(firmius_server_tui_parity_audit_tests PRIVATE
  src/daemon/DaemonTuiParityAuditTests.cpp
)
target_link_libraries(firmius_server_tui_parity_audit_tests PRIVATE
  firmius::server
  GTest::gtest_main
)
add_test(NAME firmius_server_tui_parity_audit_tests COMMAND firmius_server_tui_parity_audit_tests)

target_include_directories(firmiusd PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/packages/shared/include>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/packages/core/include>
  $<INSTALL_INTERFACE:include>
)

if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC))
  target_compile_options(firmiusd PRIVATE -Wall -Wextra -Werror)
elseif(MSVC)
  target_compile_options(firmiusd PRIVATE /W4 /WX)
endif()

if(WIN32)
  target_link_libraries(firmius_server PRIVATE ws2_32)
endif()
