# RetroBug iOS adapter (S1). SPDX-License-Identifier: Apache-2.0
#
# EGL on EAGL and the GL entry-point source the guest's imports resolve to. Everything here is iOS-only:
# OpenGLES.framework is not part of the macOS SDK, and an iOS app is the only consumer.
enable_language(OBJCXX)

add_library(rb_ios STATIC egl_shim.mm gl_symbols.mm ios_host.cpp cocos2dx.cpp)
target_compile_features(rb_ios PUBLIC cxx_std_20)
target_compile_options(rb_ios PRIVATE -fobjc-arc)
target_include_directories(rb_ios PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/..")
# `rb_gfx` is defined by core/, which the top-level CMakeLists adds after this directory; CMake resolves the
# name at generate time, so the later definition is the one linked.
target_link_libraries(rb_ios PUBLIC rb_runtime rb_gfx rb_abi
  "-framework OpenGLES" "-framework QuartzCore" "-framework Foundation")
