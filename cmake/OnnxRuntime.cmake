# ─── ONNX Runtime: pre-built shared library for embedding inference ────────
# Downloads the official pre-built release and creates an IMPORTED target.
# Works on:
#   - Linux x64 (libonnxruntime.so)
#   - macOS x86_64 / arm64 (libonnxruntime.dylib)
#   - Windows MSVC (onnxruntime.dll + onnxruntime.lib)
#   - Windows MinGW (onnxruntime.dll + a libonnxruntime.dll.a we generate)
#
# On MinGW we cannot link against the MSVC-built .lib that ships in the
# official archive, but the DLL itself is a stable C interface that any
# compiler can call into. We synthesise a MinGW-compatible import library
# from the DLL using `gendef` + `dlltool`, both shipped with MSYS2's
# mingw-w64 toolchain.

option(FIRMIUS_ENABLE_ONNX "Enable ONNX Runtime for embedding inference" ON)

if(NOT FIRMIUS_ENABLE_ONNX)
  set(ONNXRUNTIME_FOUND FALSE)
  return()
endif()

set(ORT_VERSION "1.20.1")
set(ORT_PLATFORM "linux-x64")

if(APPLE)
  if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
     (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64"))
    set(ORT_PLATFORM "osx-arm64")
  else()
    set(ORT_PLATFORM "osx-x86_64")
  endif()
elseif(WIN32)
  set(ORT_PLATFORM "win-x64")
endif()

set(ORT_URL
  "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-${ORT_PLATFORM}-${ORT_VERSION}.tgz")

set(ORT_SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/onnxruntime-src")
set(ORT_INCLUDE_DIR "${ORT_SOURCE_DIR}/include")
set(ORT_LIB_DIR "${ORT_SOURCE_DIR}/lib")

# Download + extract if not already present.
if(NOT EXISTS "${ORT_INCLUDE_DIR}/onnxruntime_c_api.h")
  message(STATUS "Downloading ONNX Runtime v${ORT_VERSION} for ${ORT_PLATFORM}...")
  set(ORT_TGZ "${FETCHCONTENT_BASE_DIR}/onnxruntime-${ORT_VERSION}.tgz")

  file(MAKE_DIRECTORY "${FETCHCONTENT_BASE_DIR}")
  file(DOWNLOAD "${ORT_URL}" "${ORT_TGZ}"
       STATUS _ort_dl_status
       SHOW_PROGRESS)

  list(GET _ort_dl_status 0 _ort_dl_rc)
  if(NOT _ort_dl_rc EQUAL 0)
    message(WARNING "Failed to download ONNX Runtime — embedding inference will be disabled")
    set(FIRMIUS_ENABLE_ONNX OFF)
  else()
    # Extract — ignore symlink errors (NTFS/FAT can't create them).
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xzf "${ORT_TGZ}"
      WORKING_DIRECTORY "${FETCHCONTENT_BASE_DIR}"
      RESULT_VARIABLE _ort_extract_rc
      OUTPUT_QUIET ERROR_QUIET)

    set(ORT_EXTRACTED_DIR "${FETCHCONTENT_BASE_DIR}/onnxruntime-${ORT_PLATFORM}-${ORT_VERSION}")
    if(EXISTS "${ORT_EXTRACTED_DIR}")
      file(RENAME "${ORT_EXTRACTED_DIR}" "${ORT_SOURCE_DIR}")
      file(REMOVE "${ORT_TGZ}")

      # Symlink fallback for NTFS/FAT extraction.
      file(GLOB _ort_versioned_libs "${ORT_LIB_DIR}/libonnxruntime.so.*.*.*")
      if(_ort_versioned_libs)
        list(GET _ort_versioned_libs 0 _ort_real_lib)
        get_filename_component(_ort_real_lib "${_ort_real_lib}" ABSOLUTE)
        if(NOT EXISTS "${ORT_LIB_DIR}/libonnxruntime.so")
          file(COPY_FILE "${_ort_real_lib}" "${ORT_LIB_DIR}/libonnxruntime.so")
        endif()
        if(NOT EXISTS "${ORT_LIB_DIR}/libonnxruntime.so.1")
          file(COPY_FILE "${_ort_real_lib}" "${ORT_LIB_DIR}/libonnxruntime.so.1")
        endif()
      endif()
    else()
      message(WARNING "Failed to extract ONNX Runtime — embedding inference will be disabled")
      set(FIRMIUS_ENABLE_ONNX OFF)
    endif()
  endif()
endif()

# Windows MinGW: synthesise an import library from the DLL the first time we
# see the archive. MSYS2's mingw-w64 toolchain ships gendef + dlltool.
if(FIRMIUS_ENABLE_ONNX AND WIN32 AND NOT MSVC AND
   EXISTS "${ORT_INCLUDE_DIR}/onnxruntime_c_api.h" AND
   NOT EXISTS "${ORT_LIB_DIR}/libonnxruntime.dll.a")

  find_program(GENDEF_EXECUTABLE gendef)
  find_program(DLLTOOL_EXECUTABLE dlltool)

  if(NOT GENDEF_EXECUTABLE OR NOT DLLTOOL_EXECUTABLE)
    message(WARNING "ONNX Runtime: gendef/dlltool not found on PATH; "
                    "MinGW import library cannot be generated. "
                    "Install mingw-w64-x86_64-tools-git in MSYS2.")
    set(FIRMIUS_ENABLE_ONNX OFF)
  else()
    message(STATUS "ONNX Runtime: generating MinGW import library from onnxruntime.dll")
    execute_process(
      COMMAND "${GENDEF_EXECUTABLE}" "${ORT_LIB_DIR}/onnxruntime.dll"
      WORKING_DIRECTORY "${ORT_LIB_DIR}"
      RESULT_VARIABLE _ort_gendef_rc
      OUTPUT_QUIET ERROR_QUIET)
    if(NOT _ort_gendef_rc EQUAL 0)
      message(WARNING "ONNX Runtime: gendef failed (rc=${_ort_gendef_rc})")
      set(FIRMIUS_ENABLE_ONNX OFF)
    else()
      execute_process(
        COMMAND "${DLLTOOL_EXECUTABLE}"
                -d "${ORT_LIB_DIR}/onnxruntime.def"
                -l "${ORT_LIB_DIR}/libonnxruntime.dll.a"
                -D onnxruntime.dll
        WORKING_DIRECTORY "${ORT_LIB_DIR}"
        RESULT_VARIABLE _ort_dlltool_rc
        OUTPUT_QUIET ERROR_QUIET)
      if(NOT _ort_dlltool_rc EQUAL 0)
        message(WARNING "ONNX Runtime: dlltool failed (rc=${_ort_dlltool_rc})")
        set(FIRMIUS_ENABLE_ONNX OFF)
      endif()
    endif()
  endif()
endif()

if(FIRMIUS_ENABLE_ONNX AND EXISTS "${ORT_INCLUDE_DIR}/onnxruntime_c_api.h")
  add_library(onnxruntime SHARED IMPORTED GLOBAL)

  if(WIN32)
    set(ORT_DLL_FILE "${ORT_LIB_DIR}/onnxruntime.dll")
    if(MSVC)
      set(ORT_IMPLIB_FILE "${ORT_LIB_DIR}/onnxruntime.lib")
    else()
      set(ORT_IMPLIB_FILE "${ORT_LIB_DIR}/libonnxruntime.dll.a")
    endif()
    set_target_properties(onnxruntime PROPERTIES
      IMPORTED_LOCATION "${ORT_DLL_FILE}"
      IMPORTED_IMPLIB "${ORT_IMPLIB_FILE}"
      INTERFACE_INCLUDE_DIRECTORIES "${ORT_INCLUDE_DIR}")
    # The DLL must sit next to the .exe at runtime. Stash the path so the
    # consumer (firmius / firmiusd executables) can copy it post-build.
    set(ONNXRUNTIME_RUNTIME_DLL "${ORT_DLL_FILE}" CACHE INTERNAL "Path to onnxruntime.dll for POST_BUILD copy")
  else()
    file(GLOB ORT_LIB_FILES "${ORT_LIB_DIR}/libonnxruntime*.so*")
    if(NOT ORT_LIB_FILES)
      file(GLOB ORT_LIB_FILES "${ORT_LIB_DIR}/libonnxruntime*.dylib")
    endif()
    list(GET ORT_LIB_FILES 0 ORT_LIB_FILE)

    set_target_properties(onnxruntime PROPERTIES
      IMPORTED_LOCATION "${ORT_LIB_FILE}"
      INTERFACE_INCLUDE_DIRECTORIES "${ORT_INCLUDE_DIR}"
      IMPORTED_NO_SONAME TRUE)

    set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH};${ORT_LIB_DIR}")
    set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH};${ORT_LIB_DIR}")
  endif()

  message(STATUS "ONNX Runtime v${ORT_VERSION} found at ${ORT_SOURCE_DIR}")
  set(ONNXRUNTIME_FOUND TRUE)
else()
  set(ONNXRUNTIME_FOUND FALSE)
  message(STATUS "ONNX Runtime not available — embedding inference disabled")
endif()
