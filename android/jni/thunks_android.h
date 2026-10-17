// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The libandroid and libjnigraphics thunks that need the JNI bridge (M4 Task 3): AAssetManager_fromJava,
// ANativeWindow_fromSurface and AndroidBitmap_* take a Java object, which the guest holds as a bridge handle and
// the host needs as the calling thread's real jobject; and the asset/bitmap calls that hand back host memory
// (AAsset_getBuffer, AndroidBitmap_lockPixels, AAssetDir_getNextFileName) or 64-bit offsets through pointers.
// Registered as HostSlots specials once the bridge is up; the plain ones are table entries in thunks.cpp.
#pragma once

#include <jni.h>

#include "host_slots.h"
#include "rb/abi/jni_forward.h"

namespace rb::launcher {

void RegisterAndroidThunks(HostSlots& slots, abi::ForwardingJni& fwd, JavaVM* vm);

}  // namespace rb::launcher
