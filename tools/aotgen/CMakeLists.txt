# rb_aotgen — the AOT discovery CLI and its test. SPDX-License-Identifier: Apache-2.0
#
# Pure ELF parsing: no engine, no Dynarmic, no platform headers. That is what lets the reader and its
# test build on every host, including the lanes that have no extracted game bytes and skip the corpus
# half of the test. The emitter (AOT task 3) adds `emit.cpp` and the Dynarmic link under
# `if(RB_AARCH64_HOST)`; nothing here may gain that dependency, because the discovery half must stay
# buildable and testable on an x86_64 host.
add_executable(rb_aotgen main.cpp code_map.cpp)
add_executable(rb_aotgen_tests code_map_test.cpp code_map.cpp)

# The emission half needs Dynarmic's AArch64 back end (`EmitArm64`), which exists only when the host
# architecture is arm64. `RB_AARCH64_HOST` is CMake's answer for Android arm64 and Apple arm64; on an
# x86_64 host the flag is absent and `main.cpp` builds the discovery CLI alone. Android is a *test
# host* here even though CMake calls the configuration cross-compiling, which is why the top-level
# CMakeLists adds this directory for `ANDROID` as well.
if(RB_AARCH64_HOST)
  target_sources(rb_aotgen PRIVATE emit.cpp)
  target_compile_definitions(rb_aotgen PRIVATE RB_AOTGEN_EMIT=1)
  target_link_libraries(rb_aotgen PRIVATE dynarmic merry::oaknut merry::mcl fmt::fmt tsl::robin_map)
  if(DEFINED RB_COMPAT_INCLUDE_DIR)
    target_include_directories(rb_aotgen PRIVATE "${RB_COMPAT_INCLUDE_DIR}")
  endif()
endif()
