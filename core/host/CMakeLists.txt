# RetroBug host boot (S1). SPDX-License-Identifier: Apache-2.0
#
# rb::host::Boot: the APK boot with its platform decisions behind callbacks (Java implementation, host
# import answers, services needing the bridge, log and report sinks). Built where the inbound trampoline
# pool is (see core/CMakeLists.txt), because the bridge it owns binds guest natives through it.
add_library(rb_host STATIC boot.cpp)
target_compile_features(rb_host PUBLIC cxx_std_20)
target_include_directories(rb_host PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(rb_host PUBLIC rb_runtime rb_inbound rb_loader rb_abi rb_syscall rb_mem)
