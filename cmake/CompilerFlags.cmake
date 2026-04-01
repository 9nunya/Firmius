if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # GCC 15+ requires explicit <cstdint> include for older dependencies like drogon
  if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "15")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include cstdint")
  endif()
  
  # Maximum optimization flags for release builds
  add_compile_options(-O3 -march=native -mtune=native)
  
  # Additional aggressive optimizations (GCC/Clang) - C++ only
  # Note: -ftree-vectorize and -ftree-slp-vectorize are enabled by default at -O3
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-ffunction-sections>)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fdata-sections>)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-finline-functions>)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-funroll-loops>)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-ffunction-sections>)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fdata-sections>)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-finline-functions>)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-funroll-loops>)
  endif()
  
  # Link-time optimization (LTO) for cross-module inlining
  add_link_options(
    -ffunction-sections
    -fdata-sections
    -Wl,--gc-sections        # Remove unused sections
    -Wl,--as-needed          # Only link needed libraries
  )
  
  add_compile_options(-Wall -Wextra -Werror)

  # Disable werror for drogon/trantor (uses deprecated c-ares APIs, unused params)
  add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=deprecated-declarations>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=unused-parameter>
  )
elseif(MSVC)
  add_compile_options(/O2 /GL /Gy /GS-)
endif()

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
