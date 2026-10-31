# rb_aotgen — the AOT discovery CLI and its test. SPDX-License-Identifier: Apache-2.0
#
# Pure ELF parsing: no engine, no Dynarmic, no platform headers. That is what lets the reader and its
# test build on every host, including the lanes that have no extracted game bytes and skip the corpus
# half of the test. The emitter (AOT task 3) adds `emit.cpp` and the Dynarmic link under
# `if(RB_AARCH64_HOST)`; nothing here may gain that dependency, because the discovery half must stay
# buildable and testable on an x86_64 host.
add_executable(rb_aotgen main.cpp code_map.cpp)
add_executable(rb_aotgen_tests code_map_test.cpp code_map.cpp)
