// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The Cocos2d-x Java helper profile (S1): `org/cocos2dx/lib/Cocos2dxHelper` and the statics its own Java
// layer would answer. The engine's native side reaches these through JNI — `getAssetManager()` is how it
// finds the APK's assets, `getCocos2dxWritablePath()` where saves go — so they are rows on TableJava, and
// the values are the AndroidEnv the app was started with.
#pragma once

#include "rb/java/android_base.h"
#include "rb/java/table_java.h"

namespace rb::java {

/// Registers `org/cocos2dx/lib/Cocos2dxHelper`: package name, writable path, asset manager, device model,
/// DPI and language, plus the preference getters (their defaults) and setters (no-ops). A title's own Java
/// is not present, so the helper's listener-driven methods are no-ops too.
void InstallCocos2dx(TableJava& java, const AndroidEnv& env);

}  // namespace rb::java
