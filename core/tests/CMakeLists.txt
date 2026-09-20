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

# C08's bridge is mostly register arithmetic, so most of its cases need no guest program at all; the
# ones that do run the real fixture and assert the hardware-oracle values.
add_executable(rb_abi_jni_tests abi_jni_test.cpp)
target_link_libraries(rb_abi_jni_tests PRIVATE rb_abi rb_exec rb_loader)
target_compile_definitions(rb_abi_jni_tests PRIVATE RB_FIXTURE_PATH="/data/local/tmp/librb_fixture.so")

add_executable(rb_abi_call_tests abi_call_test.cpp)
target_link_libraries(rb_abi_call_tests PRIVATE rb_abi rb_exec rb_loader)
target_compile_definitions(rb_abi_call_tests PRIVATE RB_FIXTURE_PATH="/data/local/tmp/librb_fixture.so")

# The back-end equivalence test (docs/20 §C02): one fixture, two tiers, every field of MachineState
# compared, and both compared to the hardware oracle. It is the M0 exit test's second half.
add_executable(rb_exec_tier_diff_tests exec_tier_diff_test.cpp)
target_link_libraries(rb_exec_tier_diff_tests PRIVATE rb_exec rb_loader)
target_compile_definitions(rb_exec_tier_diff_tests PRIVATE RB_FIXTURE_PATH="/data/local/tmp/librb_fixture.so")

# C17's recorder is pure string work, so it needs neither the fixture nor a guest program — and that is
# part of the point: the component whose job is to say what a launch did must be testable without one.
add_executable(rb_diag_tests diag_test.cpp)
target_link_libraries(rb_diag_tests PRIVATE rb_diag)
