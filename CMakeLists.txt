cmake_minimum_required(VERSION 3.16)
project(firmius VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CompilerFlags.cmake)
include(cmake/Dependencies.cmake)

enable_testing()

add_subdirectory(packages/core)
add_subdirectory(packages/shared)
add_subdirectory(packages/provider)
add_subdirectory(packages/tui)
add_subdirectory(packages/audits)
add_subdirectory(packages/cli)
add_subdirectory(packages/web)
add_subdirectory(tests/shared)
add_subdirectory(tests/core)
add_subdirectory(tests/unit)

# Install the firmius binary to user's bin location
install(TARGETS firmius DESTINATION bin)

# Install the debug symbols file alongside the binary
# Debuggers (gdb, lldb) will automatically find this file when debugging
# Note: The .debug file is generated at build time by POST_BUILD command
install(CODE "
  if(EXISTS \"\${CMAKE_CURRENT_BINARY_DIR}/packages/cli/firmius.debug\")
    file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin\"
         TYPE FILE OPTIONAL
         FILES \"\${CMAKE_CURRENT_BINARY_DIR}/packages/cli/firmius.debug\")
  endif()
" COMPONENT Runtime)

# Install prompts directory to ~/.firmius/prompts/
install(DIRECTORY prompts/ DESTINATION $ENV{HOME}/.firmius/prompts)

# Install workflows directory to ~/.firmius/workflows/
install(DIRECTORY workflows/ DESTINATION $ENV{HOME}/.firmius/workflows)

# Install hinting directory to ~/.firmius/hinting/
install(DIRECTORY hinting/ DESTINATION $ENV{HOME}/.firmius/hinting)

# Install themes directory to ~/.firmius/themes/
install(DIRECTORY themes/ DESTINATION $ENV{HOME}/.firmius/themes)

# Install bundled themes to a shared data directory for non-TUI frontends.
install(DIRECTORY themes/ DESTINATION share/firmius/themes)
