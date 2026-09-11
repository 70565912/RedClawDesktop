# Registered in the parent tests directory; relative sources stay anchored there.
add_executable(redclaw_helper_runtime_profile_tests
  helper/runtime_profile_tests.cpp
)

target_link_libraries(redclaw_helper_runtime_profile_tests PRIVATE
  redclaw_helper
)

redclaw_apply_warnings(redclaw_helper_runtime_profile_tests)

add_test(
  NAME redclaw_helper_runtime_profile_tests
  COMMAND redclaw_helper_runtime_profile_tests
)

if(WIN32)
  add_executable(redclaw_helper_direct_frame_shared_snapshot_tests
    helper/direct_frame_shared_snapshot_tests.cpp
  )

  target_link_libraries(redclaw_helper_direct_frame_shared_snapshot_tests PRIVATE
    GTest::gtest_main
    redclaw_helper
  )

  redclaw_apply_warnings(redclaw_helper_direct_frame_shared_snapshot_tests)

  add_test(
    NAME redclaw_helper_direct_frame_shared_snapshot_tests
    COMMAND redclaw_helper_direct_frame_shared_snapshot_tests
  )
endif()

add_executable(redclaw_helper_bootstrap_tests
  helper/helper_bootstrap_tests.cpp
)

target_link_libraries(redclaw_helper_bootstrap_tests PRIVATE
  redclaw_helper
)

redclaw_apply_warnings(redclaw_helper_bootstrap_tests)

add_test(
  NAME redclaw_helper_bootstrap_tests
  COMMAND redclaw_helper_bootstrap_tests
)

add_executable(redclaw_helper_capabilities_tests
  helper/helper_capabilities_tests.cpp
)

target_link_libraries(redclaw_helper_capabilities_tests PRIVATE
  redclaw_helper
)

redclaw_apply_warnings(redclaw_helper_capabilities_tests)

add_test(
  NAME redclaw_helper_capabilities_tests
  COMMAND redclaw_helper_capabilities_tests
)
