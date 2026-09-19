# RetroBug — top-level build. SPDX-License-Identifier: Apache-2.0
#
# Host tests are not buildable on this machine (no Windows compiler); see docs/30-m0-notes.md.
# The supported local configuration is an Android cross-build, with unit-test binaries executed
# on the x86_64 emulator via adb, e.g.
#
#   cmake -S . -B build/arm64 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-26
#
# compat/ goes on the include path ahead of everything so external/dynarmic's eight Boost
# includes resolve to the shims; no file under external/ is edited (AD-14). Set
# RB_USE_REAL_BOOST=ON to link actual Boost and ignore that directory.
cmake_minimum_required(VERSION 3.22)
project(retrobug LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RB_USE_REAL_BOOST "Use a real Boost installation instead of compat/" OFF)

# ---------------------------------------------------------------- engine dependency
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)   # ARMv7 guest only (AD-1)
set(DYNARMIC_TESTS OFF)
set(DYNARMIC_NO_WARNING_FLAGS ON)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF)
set(BUILD_SHARED_LIBS OFF)

if(NOT RB_USE_REAL_BOOST)
  # cmake/FindBoost.cmake answers dynarmic's find_package(Boost REQUIRED) with the compat/
  # shims, and the include dir is propagated through Boost::boost rather than repeated here.
  list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  set(RB_COMPAT_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/compat")
endif()

add_subdirectory(external/dynarmic EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------- RetroBug core
add_subdirectory(core)

if(RB_BUILD_TESTS)
  add_subdirectory(core/tests)
endif()
