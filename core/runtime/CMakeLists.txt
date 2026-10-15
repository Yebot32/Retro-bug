# RetroBug runtime (M2). SPDX-License-Identifier: Apache-2.0
#
# rb::runtime: the persistent guest process — the composition of the engine components (memory, loader,
# bridge, monitor, both tiers) into one booted bionic process with a real main thread. It sits above every
# other core component and below the launcher, so nothing in core may depend on it.
add_library(rb_runtime STATIC guest_process.cpp)
target_compile_features(rb_runtime PUBLIC cxx_std_20)
target_link_libraries(rb_runtime PUBLIC rb_linker rb_loader rb_exec rb_abi rb_syscall rb_mem)
