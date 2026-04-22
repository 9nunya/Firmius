# Tree-sitter core library and language parsers
# All parsers are compiled statically into the binary at build time.

include(FetchContent)

# Keep Tree-sitter downloads in the same FetchContent cache base.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Do not update FetchContent deps automatically" FORCE)

option(FIRMIUS_FETCHCONTENT_VERBOSE "Verbose FetchContent download logging" OFF)
if(FIRMIUS_FETCHCONTENT_VERBOSE)
  set(_firmius_git_progress TRUE)
else()
  set(_firmius_git_progress FALSE)
endif()

# ─── Tree-sitter core ────────────────────────────────────────────────────────
# SOURCE_SUBDIR trick: point to a nonexistent dir so FetchContent only
# downloads the source but does NOT run add_subdirectory (we build our own
# static lib targets from the C files directly).
FetchContent_Declare(
  tree_sitter
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
  GIT_TAG        v0.24.6
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

# ─── Language parsers ────────────────────────────────────────────────────────
FetchContent_Declare(
  ts_parser_c
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
  GIT_TAG        v0.23.5
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_cpp
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
  GIT_TAG        v0.23.4
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_java
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-java.git
  GIT_TAG        v0.23.5
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_rust
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-rust.git
  GIT_TAG        v0.23.2
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_python
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-python.git
  GIT_TAG        v0.23.6
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_javascript
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-javascript.git
  GIT_TAG        v0.23.1
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_typescript
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-typescript.git
  GIT_TAG        v0.23.2
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_json
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-json.git
  GIT_TAG        v0.24.8
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_yaml
  GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-yaml.git
  GIT_TAG        v0.7.0
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_toml
  GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-toml.git
  GIT_TAG        v0.7.0
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_cmake
  GIT_REPOSITORY https://github.com/uyha/tree-sitter-cmake.git
  GIT_TAG        v0.7.2
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_lua
  GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-lua.git
  GIT_TAG        v0.5.0
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_luau
  GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-luau.git
  GIT_TAG        v1.2.0
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_markdown
  GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-markdown.git
  GIT_TAG        v0.5.3
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

FetchContent_Declare(
  ts_parser_bash
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-bash.git
  GIT_TAG        v0.23.3
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   ${_firmius_git_progress}
  SOURCE_SUBDIR  _none
)

# Download all sources (SOURCE_SUBDIR _none prevents add_subdirectory)
FetchContent_MakeAvailable(
  tree_sitter
  ts_parser_c
  ts_parser_cpp
  ts_parser_java
  ts_parser_rust
  ts_parser_python
  ts_parser_javascript
  ts_parser_typescript
  ts_parser_json
  ts_parser_yaml
  ts_parser_toml
  ts_parser_cmake
  ts_parser_lua
  ts_parser_luau
  ts_parser_markdown
  ts_parser_bash
)

# ─── Tree-sitter core static library ────────────────────────────────────────
add_library(tree_sitter_core STATIC
  ${tree_sitter_SOURCE_DIR}/lib/src/lib.c
)
target_include_directories(tree_sitter_core
  PUBLIC  ${tree_sitter_SOURCE_DIR}/lib/include
  PRIVATE ${tree_sitter_SOURCE_DIR}/lib/src
)
target_compile_options(tree_sitter_core PRIVATE -w)
set_target_properties(tree_sitter_core PROPERTIES C_STANDARD 11)

# ─── Helper: add a tree-sitter parser static library ────────────────────────
function(add_ts_parser TARGET_NAME SOURCE_DIR)
  set(PARSER_SOURCES ${SOURCE_DIR}/src/parser.c)

  # Add scanner.c if present
  if(EXISTS "${SOURCE_DIR}/src/scanner.c")
    list(APPEND PARSER_SOURCES "${SOURCE_DIR}/src/scanner.c")
  endif()

  # Extra source files (passed as ARGN)
  foreach(EXTRA_SRC ${ARGN})
    list(APPEND PARSER_SOURCES "${SOURCE_DIR}/src/${EXTRA_SRC}")
  endforeach()

  add_library(${TARGET_NAME} STATIC ${PARSER_SOURCES})
  target_include_directories(${TARGET_NAME} PRIVATE
    ${SOURCE_DIR}/src
    ${tree_sitter_SOURCE_DIR}/lib/include
  )
  # Generated parser code produces many warnings — suppress them all
  target_compile_options(${TARGET_NAME} PRIVATE -w)
  set_target_properties(${TARGET_NAME} PROPERTIES C_STANDARD 11)
endfunction()

# ─── Build each parser ──────────────────────────────────────────────────────
add_ts_parser(ts_lang_c          "${ts_parser_c_SOURCE_DIR}")
add_ts_parser(ts_lang_cpp        "${ts_parser_cpp_SOURCE_DIR}")
add_ts_parser(ts_lang_java       "${ts_parser_java_SOURCE_DIR}")
add_ts_parser(ts_lang_rust       "${ts_parser_rust_SOURCE_DIR}")
add_ts_parser(ts_lang_python     "${ts_parser_python_SOURCE_DIR}")
add_ts_parser(ts_lang_javascript "${ts_parser_javascript_SOURCE_DIR}")
add_ts_parser(ts_lang_json       "${ts_parser_json_SOURCE_DIR}")
add_ts_parser(ts_lang_toml       "${ts_parser_toml_SOURCE_DIR}")
add_ts_parser(ts_lang_cmake      "${ts_parser_cmake_SOURCE_DIR}")
add_ts_parser(ts_lang_lua        "${ts_parser_lua_SOURCE_DIR}")
add_ts_parser(ts_lang_luau       "${ts_parser_luau_SOURCE_DIR}")
add_ts_parser(ts_lang_markdown   "${ts_parser_markdown_SOURCE_DIR}/tree-sitter-markdown")
add_ts_parser(ts_lang_bash       "${ts_parser_bash_SOURCE_DIR}")

# TypeScript has its parser in typescript/src/ subdirectory
add_library(ts_lang_typescript STATIC
  ${ts_parser_typescript_SOURCE_DIR}/typescript/src/parser.c
  ${ts_parser_typescript_SOURCE_DIR}/typescript/src/scanner.c
)
target_include_directories(ts_lang_typescript PRIVATE
  ${ts_parser_typescript_SOURCE_DIR}/typescript/src
  ${tree_sitter_SOURCE_DIR}/lib/include
)
target_compile_options(ts_lang_typescript PRIVATE -w)
set_target_properties(ts_lang_typescript PROPERTIES C_STANDARD 11)

# YAML has extra schema source files
add_ts_parser(ts_lang_yaml "${ts_parser_yaml_SOURCE_DIR}"
  schema.core.c schema.json.c
)

# ─── Aggregate interface target ─────────────────────────────────────────────
add_library(tree_sitter_all INTERFACE)
target_link_libraries(tree_sitter_all INTERFACE
  tree_sitter_core
  ts_lang_c
  ts_lang_cpp
  ts_lang_java
  ts_lang_rust
  ts_lang_python
  ts_lang_javascript
  ts_lang_typescript
  ts_lang_json
  ts_lang_yaml
  ts_lang_toml
  ts_lang_cmake
  ts_lang_lua
  ts_lang_luau
  ts_lang_markdown
  ts_lang_bash
)
