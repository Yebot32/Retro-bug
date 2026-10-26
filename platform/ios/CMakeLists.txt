# RetroBug iOS adapter (S1). SPDX-License-Identifier: Apache-2.0
#
# EGL on EAGL and the GL entry-point source the guest's imports resolve to. Everything here is iOS-only:
# OpenGLES.framework is not part of the macOS SDK, and an iOS app is the only consumer.
enable_language(OBJCXX)

add_library(rb_ios STATIC egl_shim.mm gl_symbols.mm ios_host.cpp cocos2dx_bootstrap.cpp ios_session.cpp text_render.mm)
target_compile_features(rb_ios PUBLIC cxx_std_20)
target_compile_options(rb_ios PRIVATE -fobjc-arc)
target_include_directories(rb_ios PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/..")
# `rb_gfx` is defined by core/, which the top-level CMakeLists adds after this directory; CMake resolves the
# name at generate time, so the later definition is the one linked.
target_link_libraries(rb_ios PUBLIC rb_runtime rb_java rb_gfx rb_abi
  "-framework OpenGLES" "-framework QuartzCore" "-framework CoreText" "-framework CoreGraphics" "-framework Foundation")

# One archive for the Xcode app to link: `libtool -static` merges the component libraries (a static library
# carries no link interface, so the app cannot inherit them). Built only when the app is being packaged —
# the iOS compile job asks for `rb_ios` and does not pay for this.
set(RB_IOS_ENGINE_OUT "${CMAKE_BINARY_DIR}/ios-engine/librbengine.a")
if(NOT CMAKE_LIBTOOL)
  set(CMAKE_LIBTOOL /usr/bin/libtool)
endif()
add_custom_command(
  OUTPUT "${RB_IOS_ENGINE_OUT}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/ios-engine"
  COMMAND ${CMAKE_LIBTOOL} -static -o "${RB_IOS_ENGINE_OUT}" $<TARGET_FILE:rb_ios> $<TARGET_FILE:rb_host>
          $<TARGET_FILE:rb_java> $<TARGET_FILE:rb_gfx> $<TARGET_FILE:rb_inbound> $<TARGET_FILE:rb_runtime>
          $<TARGET_FILE:rb_abi> $<TARGET_FILE:rb_syscall> $<TARGET_FILE:rb_guest_thread> $<TARGET_FILE:rb_exec>
          $<TARGET_FILE:rb_isa> $<TARGET_FILE:rb_linker> $<TARGET_FILE:rb_loader> $<TARGET_FILE:rb_mem>
          $<TARGET_FILE:rb_platform> $<TARGET_FILE:rb_diag> $<TARGET_FILE:dynarmic> $<TARGET_FILE:mcl>
          $<TARGET_FILE:fmt>
  DEPENDS rb_ios rb_host rb_java rb_gfx rb_inbound rb_runtime rb_abi rb_syscall rb_guest_thread rb_exec
          rb_isa rb_linker rb_loader rb_mem rb_platform rb_diag dynarmic mcl fmt
  COMMENT "Merging the engine into librbengine.a")
add_custom_target(rb_engine_ios DEPENDS "${RB_IOS_ENGINE_OUT}")
