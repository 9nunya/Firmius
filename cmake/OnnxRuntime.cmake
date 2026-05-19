# ─── ONNX Runtime: pre-built shared library for embedding inference ────────
# Downloads the official pre-built release and creates an IMPORTED target.

option(FIRMIUS_ENABLE_ONNX "Enable ONNX Runtime for embedding inference" ON)

# MinGW cannot link against MSVC-built .lib files shipped in the official package.
if(WIN32 AND NOT MSVC)
  message(STATUS "ONNX Runtime: disabled on MinGW (MSVC-built .lib not compatible with MinGW gcc)")
  set(ONNXRUNTIME_FOUND FALSE)
  return()
endif()

if(FIRMIUS_ENABLE_ONNX)
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
  set(ORT_HASH_URL
    "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-${ORT_PLATFORM}-${ORT_VERSION}.tgz.sha256")

  set(ORT_SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/onnxruntime-src")
  set(ORT_INCLUDE_DIR "${ORT_SOURCE_DIR}/include")
  set(ORT_LIB_DIR "${ORT_SOURCE_DIR}/lib")

  # Download + extract if not already present
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
      # Extract — ignore symlink errors (NTFS/FAT can't create them)
      execute_process(
        COMMAND tar xzf "${ORT_TGZ}"
        WORKING_DIRECTORY "${FETCHCONTENT_BASE_DIR}"
        RESULT_VARIABLE _ort_extract_rc)

      # The archive extracts to onnxruntime-<platform>-<version>/
      # Rename to onnxruntime-src for stable paths
      set(ORT_EXTRACTED_DIR "${FETCHCONTENT_BASE_DIR}/onnxruntime-${ORT_PLATFORM}-${ORT_VERSION}")
      if(EXISTS "${ORT_EXTRACTED_DIR}")
        file(RENAME "${ORT_EXTRACTED_DIR}" "${ORT_SOURCE_DIR}")
        file(REMOVE "${ORT_TGZ}")

        # On filesystems that don't support symlinks (NTFS/FAT), tar exits
        # non-zero because it can't create libonnxruntime.so → .so.1 symlinks.
        # Fix by copying the versioned .so to the unversioned names.
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

  if(FIRMIUS_ENABLE_ONNX AND EXISTS "${ORT_INCLUDE_DIR}/onnxruntime_c_api.h")
    # Create IMPORTED library target
    add_library(onnxruntime SHARED IMPORTED GLOBAL)

    # Find the actual .so file
    file(GLOB ORT_LIB_FILES "${ORT_LIB_DIR}/libonnxruntime*.so*")
    if(NOT ORT_LIB_FILES)
      file(GLOB ORT_LIB_FILES "${ORT_LIB_DIR}/libonnxruntime*.dylib")
    endif()
    if(NOT ORT_LIB_FILES)
      file(GLOB ORT_LIB_FILES "${ORT_LIB_DIR}/onnxruntime*.dll")
    endif()

    list(GET ORT_LIB_FILES 0 ORT_LIB_FILE)

    set_target_properties(onnxruntime PROPERTIES
      IMPORTED_LOCATION "${ORT_LIB_FILE}"
      INTERFACE_INCLUDE_DIRECTORIES "${ORT_INCLUDE_DIR}"
      IMPORTED_NO_SONAME TRUE)

    # Add RPATH so the binary can find the shared lib at runtime
    set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH};${ORT_LIB_DIR}")
    set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH};${ORT_LIB_DIR}")

    message(STATUS "ONNX Runtime v${ORT_VERSION} found at ${ORT_SOURCE_DIR}")
    set(ONNXRUNTIME_FOUND TRUE)
  else()
    set(ONNXRUNTIME_FOUND FALSE)
    message(STATUS "ONNX Runtime not available — embedding inference disabled")
  endif()
else()
  set(ONNXRUNTIME_FOUND FALSE)
endif()
