cmake_minimum_required(VERSION 3.16)
project(firmius VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(cmake/CompilerFlags.cmake)
include(cmake/Dependencies.cmake)

enable_testing()

add_subdirectory(packages/core)
add_subdirectory(packages/shared)
add_subdirectory(packages/provider)
add_subdirectory(packages/tui)
add_subdirectory(packages/web)
add_subdirectory(packages/audits)
add_subdirectory(packages/cli)
add_subdirectory(tests/shared)
add_subdirectory(tests/core)
add_subdirectory(tests/unit)

# Install the firmius binary to the platform-specific bindir.
install(TARGETS firmius DESTINATION ${CMAKE_INSTALL_BINDIR})

# Install the debug symbols file alongside the binary when generated.
# Debuggers (gdb, lldb) will automatically find this file when debugging.
# Note: The .debug file is generated at build time by POST_BUILD command.
install(CODE "
  if(EXISTS \"\${CMAKE_CURRENT_BINARY_DIR}/packages/cli/firmius.debug\")
    file(INSTALL DESTINATION \"\${CMAKE_INSTALL_FULL_BINDIR}\"
         TYPE FILE OPTIONAL
         FILES \"\${CMAKE_CURRENT_BINARY_DIR}/packages/cli/firmius.debug\")
  endif()
" COMPONENT Runtime)

# Derive a platform-aware per-user config/data root for workflow assets.
if(WIN32)
  if(DEFINED ENV{APPDATA} AND NOT "$ENV{APPDATA}" STREQUAL "")
    set(FIRMIUS_USER_DATA_DIR "$ENV{APPDATA}/firmius")
  elseif(DEFINED ENV{USERPROFILE} AND NOT "$ENV{USERPROFILE}" STREQUAL "")
    set(FIRMIUS_USER_DATA_DIR "$ENV{USERPROFILE}/.firmius")
  else()
    set(FIRMIUS_USER_DATA_DIR "${CMAKE_INSTALL_FULL_DATADIR}/firmius")
  endif()
else()
  if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    set(FIRMIUS_USER_DATA_DIR "$ENV{HOME}/.firmius")
  else()
    set(FIRMIUS_USER_DATA_DIR "${CMAKE_INSTALL_FULL_DATADIR}/firmius")
  endif()
endif()

# Install prompts/workflows/hinting/themes to a platform-aware per-user path.
install(DIRECTORY prompts/ DESTINATION ${FIRMIUS_USER_DATA_DIR}/prompts)
install(DIRECTORY workflows/ DESTINATION ${FIRMIUS_USER_DATA_DIR}/workflows)
install(DIRECTORY hinting/ DESTINATION ${FIRMIUS_USER_DATA_DIR}/hinting)
install(DIRECTORY themes/ DESTINATION ${FIRMIUS_USER_DATA_DIR}/themes)

# Install bundled themes to a shared data directory.
install(DIRECTORY themes/ DESTINATION ${CMAKE_INSTALL_DATADIR}/firmius/themes)
