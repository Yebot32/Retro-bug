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

# The host architectures that can run a guest and reach the AArch64 inbound trampolines: Android arm64 and
# Apple arm64 (macOS, iOS). `CMAKE_SYSTEM_PROCESSOR` is empty for an iOS cross-build, where the architecture
# lives in `CMAKE_OSX_ARCHITECTURES` instead — so both are consulted in one place and the component
# CMakeLists ask `RB_AARCH64_HOST` rather than repeating a check that one of them would miss.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
  set(RB_AARCH64_HOST ON)
else()
  set(RB_AARCH64_HOST OFF)
endif()

# The host-side launcher's JNI library. OFF by default, and that is the whole of the guard:
# `tools/run_engine_tests.sh` configures with `-DRB_BUILD_TESTS=ON` and never mentions this, so the option
# stays OFF there, the block below is never reached, and no test target gains a dependency on it.
option(RB_BUILD_LAUNCHER "Build the host-side launcher's JNI library (rbguest) for an installed APK" OFF)

# ---------------------------------------------------------------- engine dependency
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)   # ARMv7 guest only (AD-1)
set(DYNARMIC_TESTS OFF)
set(DYNARMIC_NO_WARNING_FLAGS ON)
# `CACHE ... FORCE`, not a plain `set`: under policy CMP0077 OLD (which this project's
# `cmake_minimum_required` selects) an `option()` in dynarmic's own CMakeLists overrides a normal
# variable of the same name, and `DYNARMIC_USE_PRECOMPILED_HEADERS` defaults to ON there. The plain
# `set` below therefore lost, silently: the option read OFF here and ON in every generated cache
# (`build/tests-x86_64`, `build/tests-arm64-v8a`, `build/launcher-arm64` all said BOOL=ON), which is a
# build configuration that does not match its source. A cache entry with FORCE is what wins the race.
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "dynarmic precompiled headers (RetroBug: OFF)" FORCE)
set(BUILD_SHARED_LIBS OFF)

if(NOT RB_USE_REAL_BOOST)
  # cmake/FindBoost.cmake answers dynarmic's find_package(Boost REQUIRED) with the compat/
  # shims, and the include dir is propagated through Boost::boost rather than repeated here.
  list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  set(RB_COMPAT_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/compat")
endif()

add_subdirectory(external/dynarmic EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------- RetroBug core
add_subdirectory(platform)
add_subdirectory(core)

if(RB_BUILD_TESTS)
  add_subdirectory(core/tests)
endif()

# The host-side launcher: an installable app's `librbguest.so`, which runs a guest library through the
# engine and reports the answer (android/jni/launcher.cpp).
#
# Declared here rather than under `core/`, because `core/CMakeLists.txt` states the rule that excludes it:
# "nothing here may include Android/JNI/GL headers", and this target's whole job is the JNI half of an
# Android app. It is also why it is not a `core/tests` target — it is not a test, it is the thing a phone
# installs.
#
# `EXCLUDE_FROM_ALL` is a second, independent guard rather than decoration: even with the option ON, a
# plain `ninja` in a shared build dir does not build it, so the suites `tools/run_engine_tests.sh` pushes
# cannot start depending on this existing. `tools/build_launcher_apk.sh` names the target explicitly,
# which is the only way it gets built.
if(RB_BUILD_LAUNCHER)
  add_library(rbguest SHARED EXCLUDE_FROM_ALL android/jni/launcher.cpp android/jni/gl_forward.cpp
              android/jni/runtime_boot.cpp android/jni/host_slots.cpp android/jni/thunks.cpp android/jni/thunks_android.cpp android/jni/thunks_sles.cpp
              android/jni/art_java.cpp android/jni/stub_entry_aarch64.S)
  # The same set `rb_guest_apk_tests` links (`core/tests/CMakeLists.txt`), because this target does the
  # same sequence: memory, loader, linker, executors, bridge. `rb_mem` is already implied by the others,
  # and named anyway so that a reader of this line sees which layer every address in the launcher goes
  # through. `-llog` for the two milestone lines that outlive a native crash; `-lz` (the NDK's system
  # zlib) inflates the guest APK's deflated assets for the class model's `readFile`. Static-lib link order is
  # CMake's business here, not ours: these are all CMake targets with PUBLIC dependencies.
  target_link_libraries(rbguest PRIVATE rb_host rb_inbound rb_runtime rb_linker rb_loader rb_exec rb_abi rb_mem rb_gfx -llog -lz -lEGL -lGLESv2 -landroid -ljnigraphics)
  # The APK is `arm64-v8a` — RetroBug is the host — and the guest is `armeabi-v7a` *bytes*, never a
  # library this process asks the system loader for. Nothing here restricts the ABI, so an
  # `-DANDROID_ABI=x86_64` configure of this target builds and does not run: the emulator cannot execute
  # the AArch64 code Tier J emits.

  # The 64-bit stub a repackaged game loads in place of each 32-bit library (android/jni/stub.cpp): its
  # JNI_OnLoad hands the game's APK and its own file name to `rb_runtime_boot` in librbguest.so. No
  # DT_SONAME, so each copy (libABK.so, ...) is known to the dynamic linker by its file name; -Bsymbolic so
  # each copy's `&JNI_OnLoad` is its own, never an earlier-loaded copy's.
  add_library(rbstub SHARED EXCLUDE_FROM_ALL android/jni/stub.cpp)
  target_link_libraries(rbstub PRIVATE rbguest -llog -ldl)
  target_link_options(rbstub PRIVATE -Wl,-Bsymbolic)
  set_target_properties(rbstub PROPERTIES NO_SONAME ON)
endif()
