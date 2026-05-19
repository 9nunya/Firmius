add_executable(firmius)

target_sources(firmius PRIVATE
  src/main.cpp
)

target_link_libraries(firmius PRIVATE
  firmius::tui
  firmius::server
)

target_include_directories(firmius PRIVATE
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/packages/cli/include>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/packages/shared/include>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/packages/core/include>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/packages/server/include>
  $<INSTALL_INTERFACE:include>

)

if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC))
  target_compile_options(firmius PRIVATE -Wall -Wextra -Werror)
elseif(MSVC)
  target_compile_options(firmius PRIVATE /W4 /WX)
endif()

# ─── Split debug symbols from binary (where supported) ──────────────────────
# Extract debug symbols to a separate .debug file and strip the main binary.
# This reduces the installed binary size by ~93% while preserving debug info.
if(CMAKE_OBJCOPY AND CMAKE_STRIP)
  add_custom_command(TARGET firmius POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:firmius> $<TARGET_FILE:firmius>.debug
    COMMAND ${CMAKE_STRIP} --strip-debug $<TARGET_FILE:firmius>
    COMMENT "Splitting debug symbols from firmius binary..."
    VERBATIM
  )
else()
  message(STATUS "Skipping split debug symbols for firmius (objcopy/strip unavailable)")
endif()

# ─── Windows: copy onnxruntime.dll next to firmius.exe ──────────────────────
# On POSIX the .so is found via RPATH; on Windows the loader only searches the
# directory of the executable + PATH. Copy the DLL into the binary output dir
# at build time so `firmius.exe` finds it without further setup.
if(WIN32 AND ONNXRUNTIME_RUNTIME_DLL AND EXISTS "${ONNXRUNTIME_RUNTIME_DLL}")
  add_custom_command(TARGET firmius POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_RUNTIME_DLL}"
            "$<TARGET_FILE_DIR:firmius>/"
    COMMENT "Copying onnxruntime.dll next to firmius.exe..."
    VERBATIM
  )
endif()
