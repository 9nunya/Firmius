add_library(firmius_web STATIC)
add_library(firmius::web ALIAS firmius_web)

set(FIRMIUS_WEB_FRONTEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/frontend)
find_program(FIRMIUS_WEB_NPM_EXECUTABLE NAMES npm REQUIRED)

add_custom_target(firmius_web_frontend_install
  COMMAND ${FIRMIUS_WEB_NPM_EXECUTABLE} install --no-bin-links
  WORKING_DIRECTORY ${FIRMIUS_WEB_FRONTEND_DIR}
  COMMENT "Installing web frontend dependencies"
 )

add_custom_target(firmius_web_frontend_build
  COMMAND ${FIRMIUS_WEB_NPM_EXECUTABLE} run build
  WORKING_DIRECTORY ${FIRMIUS_WEB_FRONTEND_DIR}
  COMMENT "Building web frontend placeholder bundle"
 )
add_dependencies(firmius_web_frontend_build firmius_web_frontend_install)

add_custom_target(firmius_web_frontend_test
  COMMAND ${FIRMIUS_WEB_NPM_EXECUTABLE} test
  WORKING_DIRECTORY ${FIRMIUS_WEB_FRONTEND_DIR}
  COMMENT "Running web frontend placeholder tests"
 )
add_dependencies(firmius_web_frontend_test firmius_web_frontend_install)

target_sources(firmius_web PRIVATE
  src/WebApp.cpp
)

target_sources(firmius_web PUBLIC
  FILE_SET HEADERS
  BASE_DIRS include
  FILES
    include/web/WebApp.hpp
)

target_include_directories(firmius_web PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(firmius_web PUBLIC
  firmius::core
  firmius::shared
)

if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC))
  target_compile_options(firmius_web PRIVATE -Wall -Wextra -Werror)
elseif(MSVC)
  target_compile_options(firmius_web PRIVATE /W4 /WX)
endif()

add_executable(firmius_web_runner)
target_sources(firmius_web_runner PRIVATE src/main.cpp)
target_link_libraries(firmius_web_runner PRIVATE firmius::web)
target_include_directories(firmius_web_runner PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
