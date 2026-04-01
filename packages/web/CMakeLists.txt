add_library(firmius_web)
add_library(firmius::web ALIAS firmius_web)

find_program(BUN_EXECUTABLE bun REQUIRED)

set(FIRMIUS_WEB_FRONTEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/frontend)
set(FIRMIUS_WEB_FRONTEND_DIST ${CMAKE_CURRENT_BINARY_DIR}/frontend_dist)
set(FIRMIUS_WEB_FRONTEND_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/share/firmius/web)
set(FIRMIUS_WEB_THEMES_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/share/firmius/themes)
set(FIRMIUS_WEB_FRONTEND_WORKDIR /tmp/firmius-web-frontend)
set(FIRMIUS_WEB_FRONTEND_TMP /tmp/firmius-web-frontend-tmp)
set(FIRMIUS_WEB_FRONTEND_CACHE /tmp/firmius-web-frontend-cache)

file(GLOB_RECURSE FIRMIUS_WEB_FRONTEND_SOURCES
  ${FIRMIUS_WEB_FRONTEND_DIR}/src/*
  ${FIRMIUS_WEB_FRONTEND_DIR}/index.html
  ${FIRMIUS_WEB_FRONTEND_DIR}/package.json
  ${FIRMIUS_WEB_FRONTEND_DIR}/rollup.config.js
)

add_custom_command(
  OUTPUT ${FIRMIUS_WEB_FRONTEND_DIST}/.stamp
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${FIRMIUS_WEB_FRONTEND_WORKDIR}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${FIRMIUS_WEB_FRONTEND_WORKDIR}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${FIRMIUS_WEB_FRONTEND_TMP}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${FIRMIUS_WEB_FRONTEND_CACHE}
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIRMIUS_WEB_FRONTEND_DIR} ${FIRMIUS_WEB_FRONTEND_WORKDIR}
  COMMAND ${CMAKE_COMMAND} -E env BUN_TMPDIR=${FIRMIUS_WEB_FRONTEND_TMP} ${BUN_EXECUTABLE} install --cwd=${FIRMIUS_WEB_FRONTEND_WORKDIR} --backend=copyfile --cache-dir=${FIRMIUS_WEB_FRONTEND_CACHE}
  COMMAND ${CMAKE_COMMAND} -E env BUN_TMPDIR=${FIRMIUS_WEB_FRONTEND_TMP} ${BUN_EXECUTABLE} run --cwd=${FIRMIUS_WEB_FRONTEND_WORKDIR} build
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${FIRMIUS_WEB_FRONTEND_DIST}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${FIRMIUS_WEB_FRONTEND_DIST}
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIRMIUS_WEB_FRONTEND_WORKDIR}/dist ${FIRMIUS_WEB_FRONTEND_DIST}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FIRMIUS_WEB_FRONTEND_WORKDIR}/index.html ${FIRMIUS_WEB_FRONTEND_DIST}/index.html
  COMMAND ${CMAKE_COMMAND} -E touch ${FIRMIUS_WEB_FRONTEND_DIST}/.stamp
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS ${FIRMIUS_WEB_FRONTEND_SOURCES}
  COMMENT "Building Firmius web frontend with Bun + Rollup"
)

add_custom_target(firmius_web_frontend ALL DEPENDS ${FIRMIUS_WEB_FRONTEND_DIST}/.stamp)

target_link_libraries(firmius_web PRIVATE
  firmius::core
  drogon
)

target_include_directories(firmius_web PRIVATE
  ${drogon_SOURCE_DIR}/lib/src
  ${drogon_SOURCE_DIR}/lib/inc
  ${trantor_SOURCE_DIR}
)

target_sources(firmius_web PRIVATE
  src/WebRunner.cpp
  src/WebState.cpp
  src/Router.cpp
  src/routes/ThreadsRoute.cpp
)

add_dependencies(firmius_web firmius_web_frontend)

target_compile_definitions(firmius_web PRIVATE
  FIRMIUS_WEB_FRONTEND_DIST="${FIRMIUS_WEB_FRONTEND_DIST}"
  FIRMIUS_WEB_FRONTEND_INSTALL_DIR="${FIRMIUS_WEB_FRONTEND_INSTALL_DIR}"
  FIRMIUS_WEB_THEMES_INSTALL_DIR="${FIRMIUS_WEB_THEMES_INSTALL_DIR}"
  FIRMIUS_SOURCE_THEMES_DIR="${PROJECT_SOURCE_DIR}/themes"
)

target_include_directories(firmius_web PRIVATE src include)

install(DIRECTORY ${FIRMIUS_WEB_FRONTEND_DIST}/ DESTINATION share/firmius/web)
