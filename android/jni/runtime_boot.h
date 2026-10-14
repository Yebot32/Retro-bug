// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The in-process runtime boot: what a repackaged game's 64-bit stub library (`stub.cpp`) calls from its
// `JNI_OnLoad`, inside the game's own process. Exported from librbguest.so.
#pragma once

#include <jni.h>

/// Boots the 32-bit library `lib_name` from the repackaged APK at `apk_path`: reads
/// `assets/rb32/<lib_name>` and `assets/rb32sys/{libc,libm,libz,libstdc++}.so`, runs the engine's
/// `Inspect` on them (Tier J, headless), logs every report line under logcat tag RBRUNTIME and writes the
/// whole report to `/data/data/<pkg>/files/rb-report.txt`. Returns JNI_VERSION_1_6. Idempotent per
/// `lib_name` within a process; a missing APK or entry is a named report line, never a crash.
extern "C" __attribute__((visibility("default"))) int rb_runtime_boot(JavaVM* vm, const char* apk_path,
                                                                      const char* lib_name);
