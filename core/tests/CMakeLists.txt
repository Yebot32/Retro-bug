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

# C06. Built on two purpose-made guest libraries rather than a system one, so the dependency graph and
# the constructors are the thing under test and not incidental to a big library that happens to work.
add_executable(rb_linker_tests linker_test.cpp)
target_link_libraries(rb_linker_tests PRIVATE rb_linker rb_exec rb_abi rb_loader rb_mem)
target_compile_definitions(rb_linker_tests PRIVATE
  RB_LINK_LEAF_PATH="/data/local/tmp/librbleaf.so"
  RB_LINK_MID_PATH="/data/local/tmp/librbmid.so"
  RB_LINK_IFUNC_LEAF_PATH="/data/local/tmp/librbifuncleaf.so"
  RB_LINK_IFUNC_USER_PATH="/data/local/tmp/librbifuncuser.so")
# Path the fixture is pushed to on the device by tools/run_engine_tests.sh.
target_compile_definitions(rb_guest_elf_tests PRIVATE RB_FIXTURE_PATH="/data/local/tmp/librb_fixture.so"
  RB_WEAK_FIXTURE_PATH="/data/local/tmp/librbweak.so")

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
target_link_libraries(rb_exec_tier_diff_tests PRIVATE rb_exec rb_loader rb_abi rb_syscall)
target_compile_definitions(rb_exec_tier_diff_tests PRIVATE RB_FIXTURE_PATH="/data/local/tmp/librb_fixture.so")

# C17's recorder is pure string work, so it needs neither the fixture nor a guest program — and that is
# part of the point: the component whose job is to say what a launch did must be testable without one.
add_executable(rb_diag_tests diag_test.cpp)
target_link_libraries(rb_diag_tests PRIVATE rb_diag)

# C07's monitor is a decision table, so its test needs no guest program: frames are built by hand from
# the numbers measured on hardware, and the results are checked against those same measurements.
add_executable(rb_syscall_monitor_tests syscall_monitor_test.cpp)
target_link_libraries(rb_syscall_monitor_tests PRIVATE rb_syscall rb_diag rb_exec)

# C04's policy needs no guest program and no executable memory: the allocator is injected, so the tests
# can assert the ordering (reserve writable, commit executable, release the whole reservation) that a
# platform backend will have to honour.
add_executable(rb_codecache_tests codecache_test.cpp)
target_link_libraries(rb_codecache_tests PRIVATE rb_codecache)

# C14's store needs no device, no guest program and no filesystem: the file format is a string in and a
# string out, which is what makes "unknown key reported, mistyped value refused" testable here rather
# than only in a launch. Most of the assertions are compile-time ones, so this target failing to build *is*
# several of its cases passing.
add_executable(rb_profile_tests profile_test.cpp)
target_link_libraries(rb_profile_tests PRIVATE rb_profile rb_diag)

# M1's first end-to-end lane: a real APK's native library, loaded and called on both tiers against
# values captured from the same bytes running natively on a device (tools/build-guest-apk.sh builds
# both the APK and that oracle). It needs the engine and the loader, so like the tier-diff lane it is
# not in the "runs without the DBT" group.
add_executable(rb_guest_apk_tests guest_apk_test.cpp)
target_link_libraries(rb_guest_apk_tests PRIVATE rb_exec rb_abi rb_loader rb_linker)
target_compile_definitions(rb_guest_apk_tests PRIVATE
  RB_GUEST_APK_PATH="/data/local/tmp/rb_guestprobe.apk"
  RB_GUEST_LIBM_PATH="/data/local/tmp/libm.so")

# The lane that loads the device's *own* 32-bit bionic libc.so and calls into it, with the answers
# captured from the same file running natively (tests/guest/oracle_libc.c). Its expectations about IFUNC,
# TLS relocations and the host-provided symbol set are what make C06's first feature list a measurement
# rather than a guess, so it is kept separate from rb_guest_apk_tests even though they share a rig shape.
add_executable(rb_guest_libc_tests guest_libc_test.cpp)
target_link_libraries(rb_guest_libc_tests PRIVATE rb_exec rb_abi rb_loader rb_linker)
target_compile_definitions(rb_guest_libc_tests PRIVATE
  RB_DEVICE_LIBC_PATH="/data/local/tmp/rb_device_libc.so"
  RB_IFUNC_LIBC_USER_PATH="/data/local/tmp/librbifunclibc.so"
  RB_GUEST_APK_PATH="/data/local/tmp/rb_guestprobe.apk")

# M2's persistent guest process: the device's own libc booted with constructors, the shared-globals page,
# the auxv, the static-TLS layout and bionic's real main thread (`rb::runtime::GuestProcess`). Gating: every
# row is a fact of the tablet's libc and its own native main thread. The images are pushed by
# tools/run_engine_tests.sh (build/device-libs/libc.so, build/guest-libs/{libm,libz}.so).
add_executable(rb_guest_process_tests guest_process_test.cpp)
target_link_libraries(rb_guest_process_tests PRIVATE rb_runtime)
target_compile_definitions(rb_guest_process_tests PRIVATE
  RB_DEVICE_LIBC_PATH="/data/local/tmp/rb_device_libc.so"
  RB_GUEST_LIBM_PATH="/data/local/tmp/libm.so"
  RB_GUEST_LIBZ_PATH="/data/local/tmp/libz.so"
  RB_FIXTURE_PATH="/data/local/tmp/librb_fixture.so"
  RB_STRIPPED_FIXTURE_PATH="/data/local/tmp/librb_fixture_stripped.so")

# M2 Task 3: per-thread guest contexts under one Global Guest Lock — real host threads, each bound to its own
# `rb::runtime::GuestThread`, sharing one process on both tiers (shared counters via ldrex/strex and the kuser
# cmpxchg, slice preemption, the GGL released around host calls and blocking syscalls). Gating; every row has
# a watchdog that names a hang instead of hanging the run.
add_executable(rb_guest_thread_tests guest_thread_test.cpp)
# M2 Task 4 adds guest pthreads on a GuestProcess: the device libc + libpthread_fixture.so, compared with the
# native oracle tests/corpus/oracle-pthread.txt (all pushed by tools/run_engine_tests.sh).
target_link_libraries(rb_guest_thread_tests PRIVATE rb_runtime rb_abi rb_exec rb_syscall rb_guest_thread rb_mem)
target_compile_definitions(rb_guest_thread_tests PRIVATE
  RB_DEVICE_LIBC_PATH="/data/local/tmp/rb_device_libc.so"
  RB_GUEST_LIBM_PATH="/data/local/tmp/libm.so"
  RB_GUEST_LIBZ_PATH="/data/local/tmp/libz.so"
  RB_PTHREAD_FIXTURE_PATH="/data/local/tmp/libpthread_fixture.so"
  RB_PTHREAD_ORACLE_PATH="/data/local/tmp/oracle-pthread.txt"
  RB_UNRESOLVED_FIXTURE_PATH="/data/local/tmp/librbunresolved.so")

# M3 Task 1: the forwarding JNI bridge over a fake Java, driven by tests/corpus/jni_bridge_fixture.c.
add_executable(rb_jni_forward_tests jni_forward_test.cpp)
target_link_libraries(rb_jni_forward_tests PRIVATE rb_runtime rb_abi rb_java)
target_compile_definitions(rb_jni_forward_tests PRIVATE
  RB_DEVICE_LIBC_PATH="/data/local/tmp/rb_device_libc.so"
  RB_GUEST_LIBM_PATH="/data/local/tmp/libm.so"
  RB_GUEST_LIBZ_PATH="/data/local/tmp/libz.so"
  RB_JNIBRIDGE_FIXTURE_PATH="/data/local/tmp/libjnibridge_fixture.so")

# M3 Task 3: Java calling guest natives through the inbound trampolines (arm64 hosts).
if(TARGET rb_inbound)
  add_executable(rb_inbound_tests inbound_test.cpp)
  target_link_libraries(rb_inbound_tests PRIVATE rb_inbound rb_runtime rb_abi rb_java)
  target_compile_definitions(rb_inbound_tests PRIVATE
    RB_DEVICE_LIBC_PATH="/data/local/tmp/rb_device_libc.so"
    RB_GUEST_LIBM_PATH="/data/local/tmp/libm.so"
    RB_GUEST_LIBZ_PATH="/data/local/tmp/libz.so"
    RB_JNIBRIDGE_FIXTURE_PATH="/data/local/tmp/libjnibridge_fixture.so")
endif()

# M4 Task 1: the generic AAPCS32-softfp -> AAPCS64 thunk caller and handle table (host functions called directly).
add_executable(rb_thunk_tests thunk_test.cpp)
target_link_libraries(rb_thunk_tests PRIVATE rb_abi rb_mem)

# M4 Task 3b: host file and socket pass-through (Monitor::set_host_io).
add_executable(rb_host_io_tests host_io_test.cpp)
target_link_libraries(rb_host_io_tests PRIVATE rb_syscall rb_mem)

# M4 Task 4: the OpenSL ES thunks against the device's own libOpenSLES (the module lives with the other Android
# thunks; it depends on HandleTable/ThunkCaller and the OpenSL ES headers only, not on the launcher).
add_executable(rb_sles_tests sles_test.cpp "${CMAKE_SOURCE_DIR}/android/jni/thunks_sles.cpp")
target_include_directories(rb_sles_tests PRIVATE "${CMAKE_SOURCE_DIR}/android/jni")
target_link_libraries(rb_sles_tests PRIVATE rb_abi rb_mem OpenSLES dl)

# M4 Task 2: the GL/EGL thunk table against the device's driver on an offscreen pbuffer.
add_executable(rb_gl_thunk_tests gl_thunk_test.cpp "${CMAKE_SOURCE_DIR}/android/jni/thunks.cpp")
target_include_directories(rb_gl_thunk_tests PRIVATE "${CMAKE_SOURCE_DIR}/android/jni")
target_link_libraries(rb_gl_thunk_tests PRIVATE rb_abi rb_mem rb_gfx EGL GLESv2 dl)

# The platform seam (S0 of the iOS port): pure host tests, no guest, no device, no fixtures — the
# only suite that can run on the build machine itself. Its contract is what makes the same core
# compile and behave on Linux and Darwin.
add_executable(rb_platform_tests platform_host_test.cpp)
target_link_libraries(rb_platform_tests PRIVATE rb_platform)

# S1: the APK zip reader. No guest and no device — the archive is written by the test, stored and
# deflated, then damaged in each way the reader defines a refusal for.
add_executable(rb_zip_tests zip_test.cpp)
target_link_libraries(rb_zip_tests PRIVATE rb_loader)

# S1: the platform-free boot, on the gate's own fixtures (the tablet's libc and the C06 chain). It is a
# guest lane like rb_guest_process_tests, so it exists only where the inbound trampoline pool does.
if(TARGET rb_inbound)
  add_executable(rb_host_boot_tests host_boot_test.cpp)
  target_link_libraries(rb_host_boot_tests PRIVATE rb_host rb_java)
  target_compile_definitions(rb_host_boot_tests PRIVATE
    RB_DEVICE_LIBC_PATH="/data/local/tmp/rb_device_libc.so"
    RB_GUEST_LIBM_PATH="/data/local/tmp/libm.so"
    RB_GUEST_LIBZ_PATH="/data/local/tmp/libz.so"
    RB_LINK_LEAF_PATH="/data/local/tmp/librbleaf.so"
    RB_LINK_MID_PATH="/data/local/tmp/librbmid.so"
    RB_CTOR_FAULT_PATH="/data/local/tmp/librbctorfault.so")
endif()

# S1: the iOS Java replacement. `TableJava` plus the base Android profile are pure host code — no guest,
# no device, no fixtures — so this lane runs wherever the tree builds.
add_executable(rb_table_java_tests table_java_test.cpp)
target_link_libraries(rb_table_java_tests PRIVATE rb_java)
