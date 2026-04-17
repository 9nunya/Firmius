if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "15")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include cstdint")
  endif()
endif()

if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC))
  # Maximum optimization flags for release builds.
  add_compile_options(-O3)

  # CPU tuning flags are valid on GCC/Clang non-Windows toolchains.
  if(NOT WIN32)
    add_compile_options(-march=native -mtune=native)
  endif()

  # Additional aggressive optimizations (GCC/Clang) - C++ only.
  # Note: -ftree-vectorize and -ftree-slp-vectorize are enabled by default at -O3.
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-ffunction-sections>)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fdata-sections>)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-finline-functions>)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-funroll-loops>)

  # Link options gated by platform linker support.
  add_link_options(-ffunction-sections -fdata-sections)
  if(APPLE)
    add_link_options(-Wl,-dead_strip)
  elseif(UNIX)
    add_link_options(
      -Wl,--gc-sections        # Remove unused sections
      -Wl,--as-needed          # Only link needed libraries
    )
  endif()

  add_compile_options(-Wall -Wextra -Werror)
elseif(MSVC)
  add_compile_options(/O2 /GL /Gy /GS-)
endif()

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
