// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// PikPok's Mab engine (OREO: Twist, Lick, Dunk and its siblings). Its Java layer is an Activity plus a
// GLSurfaceView.Renderer whose natives are exported by the game library, not registered in JNI_OnLoad.
// The shell drives those natives directly, but the native side still calls back into the Activity through
// JNI: the context methods (assets, files, display), and the four path getters its dex defines —
// `getFilePath`, `getCachePath`, `getExternalFilesPath` and the two expansion paths (empty strings when
// there is no APK expansion, which is how the Activity's constructor initialises them). The class must
// exist with an Activity superclass before any library loads; on-demand discovery would make it a bare
// Object and every lookup would fail.
#pragma once

#include "rb/java/android_base.h"
#include "rb/java/table_java.h"

namespace rb::java {

/// Defines `com/pikpok/MabActivity` (an `android/app/Activity` subclass) and its path getters. Call after
/// the base profile.
void InstallPikPok(TableJava& java, const AndroidEnv& env);

}  // namespace rb::java
