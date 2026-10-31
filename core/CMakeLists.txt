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
target_link_libraries(rb_mem PUBLIC rb_platform)
# `GuestMemory::Audit` reports into `rb::diag`'s auditor — the memory manager is one of the components
# the self-audit checks, and the auditor is dependency-free by design so the arrow can only ever point
# this way.
target_link_libraries(rb_mem PUBLIC rb_diag)
add_library(rb_loader STATIC loader/guest_elf.cpp loader/zip.cpp loader/apk_assets.cpp)
target_link_libraries(rb_loader PUBLIC rb_platform)
# The zip reader inflates the APK's deflated entries. The NDK's and Apple's SDKs both ship zlib; Android
# exposes it as a plain system library, so the fallback is the target name rather than a vendored copy.
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  target_link_libraries(rb_loader PUBLIC ZLIB::ZLIB)
else()
  target_link_libraries(rb_loader PUBLIC z)
endif()

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
add_library(rb_exec STATIC exec/census.cpp exec/interp.cpp exec/jit.cpp exec/aot.cpp)
target_compile_features(rb_exec PUBLIC cxx_std_20)
target_link_libraries(rb_exec PUBLIC rb_isa rb_mem)

# rb::runtime's thread context (M2): `GuestThread` (one guest thread's registers, stack and per-thread
# kernel state) and its host-thread binding, plus the header-only Global Guest Lock (rb/runtime/ggl.h). It
# lives below the bridge and the monitor because both read a thread's fields; the rest of rb::runtime
# (GuestProcess) composes everything and stays on top. Also the futex table (M2 Task 4): the monitor parks
# futex waiters on it, and GuestProcess owns one per process.
add_library(rb_guest_thread STATIC runtime/guest_thread.cpp runtime/futex_table.cpp)
target_compile_features(rb_guest_thread PUBLIC cxx_std_20)
target_link_libraries(rb_guest_thread PUBLIC rb_exec)

# rb::abi (C08): the call bridge. It consumes an executor rather than knowing which tier it is — the
# marshalling in abi/call.cpp is register arithmetic over MachineState, and the only executor coupling
# is Run/resume around a host call or a guest syscall, so Tier J inherits the bridge unchanged
# (docs/20 §C08).
add_library(rb_abi STATIC abi/call.cpp abi/jni_env.cpp abi/jni_args.cpp abi/jni_forward.cpp abi/thunk.cpp abi/guest_log.cpp)
target_compile_features(rb_abi PUBLIC cxx_std_20)
target_link_libraries(rb_abi PUBLIC rb_exec rb_mem rb_syscall rb_guest_thread)

# rb::audio (S1): the guest-side OpenSL ES object model a host with no audio backend hands a title.
# It owns no engine or platform types: memory, slot binding, data imports and logging are injected.
add_library(rb_audio STATIC audio/silent_sles.cpp)
target_compile_features(rb_audio PUBLIC cxx_std_20)
target_include_directories(rb_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rb_audio PUBLIC rb_abi rb_mem)

# rb::java (S1): the table-driven Java the core lanes already exercised as `FakeJava`, promoted so the iOS
# app can answer a guest's JNI calls with no VM. Classes, methods and fields are registered as data; a profile
# (android_base, unity_player) is the part that knows which classes a title expects.
add_library(rb_java STATIC host/java/table_java.cpp host/java/android_base.cpp host/java/cocos2dx.cpp
    host/java/dlgl.cpp host/java/blockyroads.cpp)
target_compile_features(rb_java PUBLIC cxx_std_20)
target_include_directories(rb_java PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(rb_java PUBLIC rb_abi)

# rb::diag (C17) has no engine dependency and must not gain one: it is the component that reports on the
# others, so a report that could not be produced because something else failed to build would be the
# worst possible failure mode for it. Header-only dependency is <string>/<vector>.
add_library(rb_diag STATIC diag/diag.cpp diag/audit.cpp)
target_compile_features(rb_diag PUBLIC cxx_std_20)
target_include_directories(rb_diag PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# rb::syscall (C07): the monitor. It consumes an executor's state and a diag recorder, and owns no memory
# of its own — which is the shape that keeps "no syscall reaches the host kernel except through the
# allowlist" reviewable in one file rather than grep'd across the engine.
add_library(rb_syscall STATIC syscall/monitor.cpp syscall/host_io.cpp)
target_link_libraries(rb_syscall PUBLIC rb_platform)
target_compile_features(rb_syscall PUBLIC cxx_std_20)
target_link_libraries(rb_syscall PUBLIC rb_exec rb_diag rb_guest_thread)

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

# rb::gfx (S1): the import→signature table for GLES/EGL/libandroid entry points. It is data — which host
# answers a name (a device driver on Android, the EAGL adapter on iOS) is the platform's — and both platforms'
# host-slot layers resolve through it, so a title's imports are the same list on both.
add_library(rb_gfx STATIC gfx/thunk_table.cpp)
target_compile_features(rb_gfx PUBLIC cxx_std_20)
target_include_directories(rb_gfx PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/..")

# rb_core is the aggregate the engine-facing components will hang off.
add_library(rb_core INTERFACE)
target_link_libraries(rb_core INTERFACE rb_mem rb_loader rb_linker rb_isa rb_exec rb_abi rb_diag
                                        rb_syscall rb_guest_thread rb_codecache rb_profile rb_gfx rb_audio dynarmic)

if(DEFINED RB_COMPAT_INCLUDE_DIR)
  target_include_directories(rb_core INTERFACE "${RB_COMPAT_INCLUDE_DIR}")
endif()

# rb::runtime (M2) composes the components above; it lives in its own directory because it is the one
# layer allowed to depend on all of them.
add_subdirectory(runtime)

# rb::host (S1): the in-process boot with every platform decision behind callbacks. It owns an `Inbound`,
# which is the AArch64 trampoline pool, so it exists on the architectures that can host a guest (Android
# arm64 and iOS arm64) and not on hosts that cannot execute Tier J or reach the trampolines at all.
if(RB_AARCH64_HOST)
  add_subdirectory(host)
endif()
