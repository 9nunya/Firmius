cmake_minimum_required(VERSION 3.16)
project(firmius VERSION 1.0.0 LANGUAGES CXX)

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
add_subdirectory(tests/shared)
add_subdirectory(tests/provider)
add_subdirectory(tests/core)
add_subdirectory(tests/unit)
add_subdirectory(tests/tui)
