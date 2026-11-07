// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// PikPok's Mab engine (OREO: Twist, Lick, Dunk and its siblings). Its Java layer is an Activity plus a
// GLSurfaceView.Renderer whose natives are exported by the game library, not registered in JNI_OnLoad.
// The shell drives those natives directly, but the native side still calls the Activity's context methods
// (assets, files, display) through JNI, so the class must exist with an Activity superclass before any
// library loads — otherwise on-demand discovery would make it a bare Object and every lookup would fail.
#pragma once

#include "rb/java/table_java.h"

namespace rb::java {

/// Defines `com/pikpok/MabActivity` as an `android/app/Activity` subclass. Call after the base profile.
void InstallPikPok(TableJava& java);

}  // namespace rb::java
