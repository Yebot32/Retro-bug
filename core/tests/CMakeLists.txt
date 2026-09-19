# RetroBug tests. SPDX-License-Identifier: Apache-2.0
#
# Deliberately framework-free: a plain main() that returns non-zero on failure needs no Catch2,
# builds in seconds, and runs on the x86_64 emulator over adb — which is how tests execute at all on
# this machine (no Windows host compiler; docs/30-m0-notes.md).

add_executable(rb_interval_compat_tests interval_compat_test.cpp)
target_include_directories(rb_interval_compat_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/compat")   # this test exercises the Boost-API shims themselves
