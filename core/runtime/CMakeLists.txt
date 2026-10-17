# RetroBug runtime (M2). SPDX-License-Identifier: Apache-2.0
#
# rb::runtime: the persistent guest process — the composition of the engine components (memory, loader,
# bridge, monitor, both tiers) into one booted bionic process with a real main thread. It sits above every
# other core component and below the launcher, so nothing in core may depend on it.
add_library(rb_runtime STATIC guest_process.cpp)
target_compile_features(rb_runtime PUBLIC cxx_std_20)
target_link_libraries(rb_runtime PUBLIC rb_linker rb_loader rb_exec rb_abi rb_syscall rb_guest_thread rb_mem)

# M3: Java calling guest natives — the inbound trampoline pool (AArch64 assembly) and its dispatcher. arm64 hosts
# only, which is every device RetroBug targets.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  add_library(rb_inbound STATIC inbound.cpp inbound_aarch64.S)
  target_compile_features(rb_inbound PUBLIC cxx_std_20)
  target_link_libraries(rb_inbound PUBLIC rb_runtime rb_abi)
endif()
