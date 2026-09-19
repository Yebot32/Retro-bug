# RetroBug tests. SPDX-License-Identifier: Apache-2.0
#
# Deliberately framework-free: a plain main() that returns non-zero on failure needs no Catch2,
# builds in seconds, and runs on the x86_64 emulator over adb — which is how tests execute at all on
# this machine (no Windows host compiler; docs/30-m0-notes.md).

add_executable(rb_interval_compat_tests interval_compat_test.cpp)
target_include_directories(rb_interval_compat_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/compat")   # this test exercises the Boost-API shims themselves

add_executable(rb_variant_compat_tests variant_compat_test.cpp)
target_include_directories(rb_variant_compat_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/compat")

add_executable(rb_guest_memory_tests guest_memory_test.cpp)
target_link_libraries(rb_guest_memory_tests PRIVATE rb_mem)

add_executable(rb_guest_elf_tests guest_elf_test.cpp)
target_link_libraries(rb_guest_elf_tests PRIVATE rb_loader)
# Path the fixture is pushed to on the device by tools/run_engine_tests.sh.
target_compile_definitions(rb_guest_elf_tests PRIVATE RB_FIXTURE_PATH="/data/local/tmp/librb_fixture.so")

# The front-end wrapper's test needs the engine by definition (C01 wraps it), and the loader to get
# real Thumb-2 bytes into guest memory. That is why it is a separate target: rb_guest_elf_tests and
# rb_guest_memory_tests stay runnable on a machine where the DBT does not build (docs/30 §Step 5b).
add_executable(rb_isa_front_end_tests isa_front_end_test.cpp)
target_link_libraries(rb_isa_front_end_tests PRIVATE rb_isa rb_loader)
target_compile_definitions(rb_isa_front_end_tests PRIVATE RB_FIXTURE_PATH="/data/local/tmp/librb_fixture.so")

add_executable(rb_exec_census_tests exec_census_test.cpp)
target_link_libraries(rb_exec_census_tests PRIVATE rb_exec rb_loader)
target_compile_definitions(rb_exec_census_tests PRIVATE RB_FIXTURE_PATH="/data/local/tmp/librb_fixture.so")

# Tier I's test is the first one that executes guest instructions, so it needs the engine through
# rb_exec; the oracle values it asserts are hardware-derived (tests/corpus/oracle-armv7.txt).
add_executable(rb_exec_interp_tests exec_interp_test.cpp)
target_link_libraries(rb_exec_interp_tests PRIVATE rb_exec rb_loader)
target_compile_definitions(rb_exec_interp_tests PRIVATE RB_FIXTURE_PATH="/data/local/tmp/librb_fixture.so")
