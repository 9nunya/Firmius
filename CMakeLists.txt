cmake_minimum_required(VERSION 3.16)
project(firmius VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/Dependencies.cmake)
include(cmake/CompilerFlags.cmake)

enable_testing()

add_subdirectory(packages/core)
add_subdirectory(packages/shared)
add_subdirectory(packages/provider)
add_subdirectory(packages/tui)
add_subdirectory(packages/audits)
add_subdirectory(tests/shared)
add_subdirectory(tests/core)
add_subdirectory(tests/unit)

# Install the firmius binary to user's bin location
install(TARGETS firmius DESTINATION bin)

# Install prompts directory to ~/.firmius/prompts/
install(DIRECTORY prompts/ DESTINATION $ENV{HOME}/.firmius/prompts)

# Install workflows directory to ~/.firmius/workflows/
install(DIRECTORY workflows/ DESTINATION $ENV{HOME}/.firmius/workflows)

# Install themes directory to ~/.firmius/themes/
install(DIRECTORY themes/ DESTINATION $ENV{HOME}/.firmius/themes)
