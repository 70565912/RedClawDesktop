find_package(GTest CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS json)

# include() deliberately preserves DIRECTORY TESTS and target aggregation below.
# add_subdirectory() would silently omit child tests from redclaw_tests_unit/all.
include(cmake/service-tests.cmake)
include(cmake/net-tests.cmake)
include(cmake/agent-tests.cmake)
include(cmake/protocol-tests.cmake)
include(cmake/security-tests.cmake)
include(cmake/helper-tests.cmake)
include(cmake/capture-tests.cmake)
include(cmake/render-tests.cmake)
include(cmake/session-tests.cmake)
include(cmake/input-tests.cmake)
include(cmake/integration-tests.cmake)
include(cmake/diag-tests.cmake)
include(cmake/ui-tests.cmake)

add_executable(redclaw_runtime_options_tests helper/runtime_options_tests.cpp helper/local_control_output_tests.cpp helper/input_qa_receipts_tests.cpp helper/runtime_loop_wake_tests.cpp)
target_link_libraries(redclaw_runtime_options_tests PRIVATE redclaw_runtime_options redclaw_protocol redclaw_diag redclaw_input Boost::json GTest::gtest_main)
redclaw_apply_warnings(redclaw_runtime_options_tests)
add_test(NAME redclaw_runtime_options_tests COMMAND redclaw_runtime_options_tests)

get_property(redclaw_tests_registered_names DIRECTORY PROPERTY TESTS)

set(redclaw_tests_executable_targets "")
set(redclaw_tests_integration_targets "")
set(redclaw_tests_unit_targets "")

foreach(redclaw_target_name IN LISTS redclaw_tests_registered_names)
  if(NOT TARGET ${redclaw_target_name})
    continue()
  endif()

  list(APPEND redclaw_tests_executable_targets ${redclaw_target_name})

  if(redclaw_target_name MATCHES "_integration_|_e2e_")
    list(APPEND redclaw_tests_integration_targets ${redclaw_target_name})
  else()
    list(APPEND redclaw_tests_unit_targets ${redclaw_target_name})
  endif()
endforeach()

if(NOT REDCLAW_TESTS_IN_DEFAULT_BUILD)
  foreach(redclaw_target_name IN LISTS redclaw_tests_executable_targets)
    set_property(TARGET ${redclaw_target_name} PROPERTY EXCLUDE_FROM_ALL TRUE)
    set_property(TARGET ${redclaw_target_name} PROPERTY EXCLUDE_FROM_DEFAULT_BUILD TRUE)
  endforeach()
endif()

if(redclaw_tests_executable_targets)
  add_custom_target(redclaw_tests_all)
  add_dependencies(redclaw_tests_all ${redclaw_tests_executable_targets})

  add_custom_target(redclaw_tests_unit)
  if(redclaw_tests_unit_targets)
    add_dependencies(redclaw_tests_unit ${redclaw_tests_unit_targets})
  endif()

  add_custom_target(redclaw_tests_integration)
  if(redclaw_tests_integration_targets)
    add_dependencies(redclaw_tests_integration ${redclaw_tests_integration_targets})
  endif()
endif()
