# RetroBug iOS adapter (S1). SPDX-License-Identifier: Apache-2.0
#
# EGL on EAGL and the GL entry-point source the guest's imports resolve to. Everything here is iOS-only:
# OpenGLES.framework is not part of the macOS SDK, and an iOS app is the only consumer.
enable_language(OBJCXX)

add_library(rb_ios STATIC egl_shim.mm gl_symbols.mm ios_host.cpp cocos2dx_bootstrap.cpp dlgl_bootstrap.cpp ios_session.cpp text_render.mm)
target_compile_features(rb_ios PUBLIC cxx_std_20)
target_compile_options(rb_ios PRIVATE -fobjc-arc)
target_include_directories(rb_ios PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/..")
# `rb_gfx` is defined by core/, which the top-level CMakeLists adds after this directory; CMake resolves the
# name at generate time, so the later definition is the one linked.
target_link_libraries(rb_ios PUBLIC rb_runtime rb_java rb_gfx rb_audio rb_abi
  "-framework OpenGLES" "-framework QuartzCore" "-framework CoreText" "-framework CoreGraphics" "-framework Foundation")

# A per-game AOT profile (AOT task 5): the emitted assembly, its per-library tables and the profile table.
# The sources live under build/aot/<game>/ and are never in git — they are translation derived from the
# game's bytes; rb_aotgen --profile writes them.
#
# They are an OBJECT library fed to the archive merge as plain object files, not sources of `rb_ios`: the
# first AOT IPA built them, but `libtool -static` merging members out of `librb_ios.a` dropped every `.S`
# member, so the app link could not find a single translated block. Passing the objects directly is the
# path libtool is designed for. `rb_ios` still gets the define, because `ios_session.cpp` references
# `rb_aot_builtin_profile` and must compile that reference in.
set(RB_AOT_MERGE_OBJECTS "")
set(RB_AOT_MERGE_DEPENDS "")
if(RB_AOT_PROFILE_DIR)
  file(GLOB RB_AOT_PROFILE_SOURCES "${RB_AOT_PROFILE_DIR}/*.S" "${RB_AOT_PROFILE_DIR}/*_table.cpp"
       "${RB_AOT_PROFILE_DIR}/*_profile.cpp")
  if(NOT RB_AOT_PROFILE_SOURCES)
    message(FATAL_ERROR "RB_AOT_PROFILE_DIR=${RB_AOT_PROFILE_DIR} has no generated sources")
  endif()
  add_library(rb_aot_profile OBJECT ${RB_AOT_PROFILE_SOURCES})
  target_compile_features(rb_aot_profile PRIVATE cxx_std_20)
  # aot.h includes the vendored A32JitState and the guard types; inherit the engine's include path.
  target_link_libraries(rb_aot_profile PRIVATE rb_exec)
  set(RB_AOT_MERGE_OBJECTS "$<TARGET_OBJECTS:rb_aot_profile>")
  set(RB_AOT_MERGE_DEPENDS rb_aot_profile)
  target_compile_definitions(rb_ios PRIVATE RB_AOT_PROFILE=1)
  message(STATUS "AOT profile: ${RB_AOT_PROFILE_SOURCES}")
endif()

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
          $<TARGET_FILE:rb_java> $<TARGET_FILE:rb_gfx> $<TARGET_FILE:rb_audio> $<TARGET_FILE:rb_inbound> $<TARGET_FILE:rb_runtime>
          $<TARGET_FILE:rb_abi> $<TARGET_FILE:rb_syscall> $<TARGET_FILE:rb_guest_thread> $<TARGET_FILE:rb_exec>
          $<TARGET_FILE:rb_isa> $<TARGET_FILE:rb_linker> $<TARGET_FILE:rb_loader> $<TARGET_FILE:rb_mem>
          $<TARGET_FILE:rb_platform> $<TARGET_FILE:rb_diag> $<TARGET_FILE:rb_crypto> $<TARGET_FILE:dynarmic> $<TARGET_FILE:mcl>
          $<TARGET_FILE:fmt> ${RB_AOT_MERGE_OBJECTS}
  DEPENDS rb_ios rb_host rb_java rb_gfx rb_audio rb_inbound rb_runtime rb_abi rb_syscall rb_guest_thread rb_exec
          rb_isa rb_linker rb_loader rb_mem rb_platform rb_diag rb_crypto dynarmic mcl fmt ${RB_AOT_MERGE_DEPENDS}
  COMMENT "Merging the engine into librbengine.a")
add_custom_target(rb_engine_ios DEPENDS "${RB_IOS_ENGINE_OUT}")
