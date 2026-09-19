# RetroBug core (C01–C17). SPDX-License-Identifier: Apache-2.0
#
# Platform-agnostic engine code. Two rules this file enforces rather than documents:
# nothing here may include Android/JNI/GL headers, and nothing may consume a guest address
# as a host pointer without going through rb::mem (docs/20 §3, core/README.md).

add_library(rb_core STATIC
  mem/guest_memory.cpp
  loader/guest_elf.cpp)

target_include_directories(rb_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/..")   # allows #include "core/..." from tests

target_compile_features(rb_core PUBLIC cxx_std_20)
# C03 must not depend on host page size being 4 KiB, and forbids the identity assumption
# outright; see docs/10 AD-6/AD-7.
target_compile_definitions(rb_core PUBLIC RB_GUEST_PAGE_SHIFT=12)

target_link_libraries(rb_core PUBLIC dynarmic)

if(DEFINED RB_COMPAT_INCLUDE_DIR)
  target_include_directories(rb_core INTERFACE "${RB_COMPAT_INCLUDE_DIR}")
endif()
