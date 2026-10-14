# RetroBug core (C01–C17). SPDX-License-Identifier: Apache-2.0
#
# Platform-agnostic engine code. Two rules this file enforces rather than documents:
# nothing here may include Android/JNI/GL headers, and nothing may consume a guest address
# as a host pointer without going through rb::mem (docs/20 §3, core/README.md).

# rb::mem and rb::loader have no engine dependency — docs/20 §3 lists their ↑ as "—" and C03/C05
# respectively. Keeping them separate from rb_core means the memory-model and loader suites can be
# built and run on a target without compiling the DBT at all, which matters now that the AArch64
# backend needs its own work against compat/ (docs/30 §Step 5).
add_library(rb_mem STATIC mem/guest_memory.cpp)
# `GuestMemory::Audit` reports into `rb::diag`'s auditor — the memory manager is one of the components
# the self-audit checks, and the auditor is dependency-free by design so the arrow can only ever point
# this way.
target_link_libraries(rb_mem PUBLIC rb_diag)
add_library(rb_loader STATIC loader/guest_elf.cpp)

foreach(t rb_mem rb_loader)
  target_include_directories(${t} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/..")   # allows #include "core/..." from tests
  target_compile_features(${t} PUBLIC cxx_std_20)
endforeach()

# The guest page granule is a compile-time constant by AD-7, and identity addressing must never be
# assumed; both are enforced here rather than by convention.
target_compile_definitions(rb_mem PUBLIC RB_GUEST_PAGE_SHIFT=12)
target_link_libraries(rb_loader PUBLIC rb_mem)

# rb::linker (C06) sits on C05 and adds no engine dependency: the bytes arrive through a callback and
# initializers run through one, so it neither opens a file nor executes guest code by itself.
add_library(rb_linker STATIC linker/guest_linker.cpp)
target_include_directories(rb_linker PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(rb_linker PUBLIC cxx_std_20)
target_link_libraries(rb_linker PUBLIC rb_loader rb_mem)

# rb::isa is the one core component that does depend on the vendored engine — it *is* the wrapper
# around it (C01's ↑ in docs/20 §2 is Dynarmic itself), so it links dynarmic.
add_library(rb_isa STATIC isa/front_end.cpp)
target_compile_features(rb_isa PUBLIC cxx_std_20)
# dynarmic links its own header dependencies PRIVATE (external/dynarmic/src/dynarmic/CMakeLists.txt:509),
# which means anything including dynarmic's public headers has to supply them again: reaching the IR
# pulls in <mcl/stdint.hpp>, <fmt/...>, <tsl/robin_*.h> and <boost/variant.hpp>. This is that same set
# declared where it is actually needed — re-declaring it here rather than patching the vendored copy
# is the point (AD-14).
#
# Boost is the odd one out: the stub FindBoost creates Boost::boost as a directory-scoped IMPORTED
# target inside dynarmic's own subtree, so it is not visible here — hence the include directory
# directly, which is also how rb_core gets it.
target_link_libraries(rb_isa PUBLIC rb_mem dynarmic merry::mcl fmt::fmt tsl::robin_map)
if(DEFINED RB_COMPAT_INCLUDE_DIR)
  target_include_directories(rb_isa PUBLIC "${RB_COMPAT_INCLUDE_DIR}")
endif()

# rb::exec: the tier seam. Its census walks the graph rb::isa produces, and its Tier I interpreter runs
# on that same IR, so it depends on C01 either way.
add_library(rb_exec STATIC exec/census.cpp exec/interp.cpp exec/jit.cpp)
target_compile_features(rb_exec PUBLIC cxx_std_20)
target_link_libraries(rb_exec PUBLIC rb_isa rb_mem)

# rb::abi (C08): the call bridge. It consumes an executor rather than knowing which tier it is — the
# marshalling in abi/call.cpp is register arithmetic over MachineState, and the only executor coupling
# is Run/resume around a host call or a guest syscall, so Tier J inherits the bridge unchanged
# (docs/20 §C08).
add_library(rb_abi STATIC abi/call.cpp abi/jni_env.cpp)
target_compile_features(rb_abi PUBLIC cxx_std_20)
target_link_libraries(rb_abi PUBLIC rb_exec rb_mem rb_syscall)

# rb::diag (C17) has no engine dependency and must not gain one: it is the component that reports on the
# others, so a report that could not be produced because something else failed to build would be the
# worst possible failure mode for it. Header-only dependency is <string>/<vector>.
add_library(rb_diag STATIC diag/diag.cpp diag/audit.cpp)
target_compile_features(rb_diag PUBLIC cxx_std_20)
target_include_directories(rb_diag PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# rb::syscall (C07): the monitor. It consumes an executor's state and a diag recorder, and owns no memory
# of its own — which is the shape that keeps "no syscall reaches the host kernel except through the
# allowlist" reviewable in one file rather than grep'd across the engine.
add_library(rb_syscall STATIC syscall/monitor.cpp)
target_compile_features(rb_syscall PUBLIC cxx_std_20)
target_link_libraries(rb_syscall PUBLIC rb_exec rb_diag)

# rb::codecache (C04): the block-cache policy. It reaches no executable memory of its own — the
# allocator is injected, so the three backends (anon RWX, memfd dual alias, none) are the platform's
# answer to the U-13 probe and this is only what to do about it. That boundary is why the tests can
# assert W^X ordering without an mmap.
add_library(rb_codecache STATIC codecache/codecache.cpp)
target_compile_features(rb_codecache PUBLIC cxx_std_20)
target_link_libraries(rb_codecache PUBLIC rb_exec rb_diag)

# rb::profile (C14): the one store for per-app settings. It reaches no guest memory, no executor and no
# filesystem — the launcher hands it text it has already read and hands back the lines it could not
# apply. That narrow surface is what lets the closed-lookup rule be a compile-time fact: the store's only
# entry points are the two key types, so there is nothing for a caller to route around.
add_library(rb_profile STATIC profile/profile.cpp)
target_compile_features(rb_profile PUBLIC cxx_std_20)
target_link_libraries(rb_profile PUBLIC rb_diag)

# rb_core is the aggregate the engine-facing components will hang off.
add_library(rb_core INTERFACE)
target_link_libraries(rb_core INTERFACE rb_mem rb_loader rb_linker rb_isa rb_exec rb_abi rb_diag
                                        rb_syscall rb_codecache rb_profile dynarmic)

if(DEFINED RB_COMPAT_INCLUDE_DIR)
  target_include_directories(rb_core INTERFACE "${RB_COMPAT_INCLUDE_DIR}")
endif()
