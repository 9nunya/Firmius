include(FetchContent)

# ─── FetchContent cache & robustness ─────────────────────────────────────────
# Allow callers to override cache location (e.g., -DFETCHCONTENT_BASE_DIR=...)
# or provide FIRMIUS_FETCHCONTENT_BASE in the environment.
if(NOT DEFINED FETCHCONTENT_BASE_DIR)
  if(DEFINED ENV{FIRMIUS_FETCHCONTENT_BASE})
    set(_firmius_fetch_base "$ENV{FIRMIUS_FETCHCONTENT_BASE}")
  elseif(DEFINED ENV{XDG_CACHE_HOME})
    set(_firmius_fetch_base "$ENV{XDG_CACHE_HOME}/firmius/fetchcontent")
  elseif(DEFINED ENV{HOME})
    set(_firmius_fetch_base "$ENV{HOME}/.cache/firmius/fetchcontent")
  else()
    set(_firmius_fetch_base "${CMAKE_SOURCE_DIR}/.cache/fetchcontent")
  endif()
  set(FETCHCONTENT_BASE_DIR "${_firmius_fetch_base}" CACHE PATH
      "Base directory for FetchContent downloads" FORCE)
  unset(_firmius_fetch_base)
endif()

option(FIRMIUS_FETCHCONTENT_OFFLINE "Disable FetchContent downloads/updates" OFF)
option(FIRMIUS_FETCHCONTENT_QUIET "Silence FetchContent progress" ON)
option(FIRMIUS_FETCHCONTENT_VERBOSE "Verbose FetchContent download logging" OFF)

if(FIRMIUS_FETCHCONTENT_VERBOSE)
  set(FIRMIUS_FETCHCONTENT_QUIET OFF CACHE BOOL "" FORCE)
  set(_firmius_git_progress TRUE)
else()
  set(_firmius_git_progress FALSE)
endif()

# Don't re-fetch on every configure; reuse cached sources when present.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Do not update FetchContent deps automatically" FORCE)
set(FETCHCONTENT_QUIET ${FIRMIUS_FETCHCONTENT_QUIET} CACHE BOOL
    "Suppress FetchContent output" FORCE)

if(FIRMIUS_FETCHCONTENT_OFFLINE)
  set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL
      "Disallow FetchContent network access" FORCE)
endif()

if(FIRMIUS_FETCHCONTENT_VERBOSE)
  message(STATUS "FetchContent cache base: ${FETCHCONTENT_BASE_DIR}")
  if(FIRMIUS_FETCHCONTENT_OFFLINE)
    message(STATUS "FetchContent offline mode enabled")
  else()
    message(STATUS "FetchContent offline mode disabled")
  endif()
endif()

FetchContent_Declare(
  rapidjson
  GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
  GIT_TAG master
  GIT_SHALLOW TRUE
  GIT_PROGRESS ${_firmius_git_progress}
)

set(RAPIDJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RAPIDJSON_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
  GIT_SHALLOW TRUE
  GIT_PROGRESS ${_firmius_git_progress}
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v6.1.9
  GIT_SHALLOW TRUE
  GIT_PROGRESS ${_firmius_git_progress}
)

FetchContent_MakeAvailable(rapidjson googletest ftxui)

# Tree-sitter core + language parsers (compiled in at build time)
include(cmake/TreeSitter.cmake)
