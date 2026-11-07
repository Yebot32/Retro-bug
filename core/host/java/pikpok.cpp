// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See rb/java/pikpok.h.

#include "rb/java/pikpok.h"

namespace rb::java {

void InstallPikPok(TableJava& java) {
  java.DefineClass("com/pikpok/MabActivity", "android/app/Activity");
  // The engine's own renderer/input classes: static natives only, so the class exists to be the jclass a
  // static JNI call receives, and to name failed lookups in the trace.
  java.DefineClass("com/pikpok/MabRenderer", "java/lang/Object");
  java.DefineClass("com/pikpok/MabInput", "java/lang/Object");
}

}  // namespace rb::java
