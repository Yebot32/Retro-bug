// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The in-process runtime boot: what a repackaged game's 64-bit stub library (`stub.cpp`) calls from its
// `JNI_OnLoad`, inside the game's own process. Exported from librbguest.so.
#pragma once

#include <jni.h>

namespace rb::runtime {
class GuestProcess;
}

/// Boots the 32-bit library `lib_name` from the repackaged APK at `apk_path` into this process's one
/// guest process (created, with its libc/libm/libz scope from `assets/rb32sys/`, on the first call; reused
/// after): loads `assets/rb32/<lib_name>` and the `assets/rb32/` siblings its DT_NEEDED names, runs the
/// constructors and the guest `JNI_OnLoad` on Tier J. Boot-only: no game entry point is driven. Logs every
/// report line under logcat tag RBRUNTIME and writes the boot's report to
/// `/data/user/<uid/100000>/<pkg>/files/rb-report.txt`. Returns JNI_VERSION_1_6. A library is marked booted
/// only on success (a later call for it is then a no-op); every failure, host exceptions included, is a
/// named report line, never a crash.
extern "C" __attribute__((visibility("default"))) int rb_runtime_boot(JavaVM* vm, const char* apk_path,
                                                                      const char* lib_name);

/// The live guest process `rb_runtime_boot` made (null before a scope booted) — M3 reaches it here.
__attribute__((visibility("default"))) rb::runtime::GuestProcess* rb_runtime_process();
