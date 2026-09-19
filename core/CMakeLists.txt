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

# rb_core is the aggregate the engine-facing components will hang off.
add_library(rb_core INTERFACE)
target_link_libraries(rb_core INTERFACE rb_mem rb_loader dynarmic)

if(DEFINED RB_COMPAT_INCLUDE_DIR)
  target_include_directories(rb_core INTERFACE "${RB_COMPAT_INCLUDE_DIR}")
endif()
