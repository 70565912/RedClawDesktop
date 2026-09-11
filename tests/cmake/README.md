# Test registration

These manifests are included by `tests/CMakeLists.txt`, not separate CMake directories.
Keep source paths relative to `tests/`; add tests to the owning manifest and keep
aggregate target construction in the parent. Apply warnings and explicit dependencies
to new executables. The startup-options regression is registered directly in the parent.

When moving registrations, compare `ctest --show-only=json-v1` names, commands and
properties before/after configure, excluding backtrace source locations. Build the
aggregate targets to confirm moved tests were not silently omitted. Do not add a second
hand-maintained list of test executable names. `redclaw_net_test_support` is test-only.
