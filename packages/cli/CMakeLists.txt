add_executable(firmius)

target_sources(firmius PRIVATE
  src/main.cpp
)

target_link_libraries(firmius PRIVATE
  firmius::tui
  firmius::web
)

target_include_directories(firmius PRIVATE
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/packages/cli/include>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/packages/shared/include>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/packages/tui/include>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/packages/web/include>
  $<INSTALL_INTERFACE:include>

)

target_compile_options(firmius PRIVATE
  -Wall -Wextra -Werror
)

# ─── Split debug symbols from binary ─────────────────────────────────────────
# Extract debug symbols to a separate .debug file and strip the main binary.
# This reduces the installed binary size by ~93% while preserving debug info.
add_custom_command(TARGET firmius POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:firmius> $<TARGET_FILE:firmius>.debug
  COMMAND ${CMAKE_STRIP} --strip-debug $<TARGET_FILE:firmius>
  COMMENT "Splitting debug symbols from firmius binary..."
  VERBATIM
)
