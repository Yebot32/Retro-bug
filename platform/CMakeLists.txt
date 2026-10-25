# RetroBug platform layer (SPDX-License-Identifier: Apache-2.0)
#
# The host-OS operations declared in `core/include/rb/platform/host.h`, one implementation per platform.
# `core/` links this and never includes an OS header for these operations.
#
# Linux and Android share `posix/` (the code they always ran, moved here). Darwin (macOS/iOS) is a second
# implementation: where it has no equivalent it fails the call, and the caller refuses by name.
add_library(rb_platform STATIC)

if(APPLE)
  target_sources(rb_platform PRIVATE darwin/host_darwin.cpp)
  # iOS itself has no child processes and no executable anonymous memory; the syscall layer is the same
  # Darwin code for both, with capabilities decided by the caller (ExecutablePagesSupported etc.).
  target_compile_definitions(rb_platform PUBLIC RB_PLATFORM_DARWIN=1)
  if(IOS)
    target_compile_definitions(rb_platform PUBLIC RB_PLATFORM_IOS=1)
  endif()
else()
  target_sources(rb_platform PRIVATE posix/host_posix.cpp)
endif()

target_include_directories(rb_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../core/include")
target_compile_features(rb_platform PUBLIC cxx_std_20)

# The iOS adapter (S1): EGL on EAGL and the GL symbol source. The OpenGLES framework exists only in the iOS
# SDK, so this directory is configured for an iOS build and never for the macOS host.
if(IOS)
  add_subdirectory(ios)
endif()
