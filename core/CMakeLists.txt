# RetroBug core (C01–C17). SPDX-License-Identifier: Apache-2.0
#
# Platform-agnostic engine code. Two rules this file enforces rather than documents:
# nothing here may include Android/JNI/GL headers, and nothing may consume a guest address
# as a host pointer without going through rb::mem (docs/20 §3, core/README.md).

add_library(rb_core INTERFACE)
target_include_directories(rb_core INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/..")   # allows #include "core/..." from tests

target_link_libraries(rb_core INTERFACE dynarmic)

if(DEFINED RB_COMPAT_INCLUDE_DIR)
  target_include_directories(rb_core INTERFACE "${RB_COMPAT_INCLUDE_DIR}")
endif()
