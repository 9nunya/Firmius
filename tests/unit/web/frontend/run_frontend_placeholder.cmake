if(NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "TEST_ROOT must be provided")
endif()

if(NOT EXISTS "${TEST_ROOT}")
  message(FATAL_ERROR "Frontend placeholder root not found: ${TEST_ROOT}")
endif()

message(STATUS "Frontend placeholder root: ${TEST_ROOT}")
