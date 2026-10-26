// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The KitKat Challenge Java surface (DLGE engine): the classes JNI_OnLoad registers natives on, the
// local-storage statics its C++ uses for saves, and the AudioTrack fallback its audio engine probes. It
// draws its text in-engine, so there are no graphics classes here.
#pragma once

#include "rb/java/android_base.h"
#include "rb/java/table_java.h"

namespace rb::java {

/// Registers `com/doubleleft/{DLGLSurface,DLGLRenderer,AdmobPlugin,GATracker,GooglePlayServices,
/// androidkitkat/AndroidKitKatActivity}` (the classes the native code names), `LocalStorage` (in-memory
/// defaults), and a silent `android/media/AudioTrack`.
void InstallDlgl(TableJava& java, const AndroidEnv& env);

}  // namespace rb::java
