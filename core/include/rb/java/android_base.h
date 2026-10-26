// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The base Android Java profile for iOS (S1): the classes a native title's guest code reaches through JNI
// that do not depend on a particular game — System, SystemClock, Log, Build, Locale, File, Context/Activity.
// Each is a row set on `rb::java::TableJava`; a title that needs more adds rows in its own profile, never a
// branch in the engine.
//
// This is deliberately small. The rows that matter are discovered from a title's recorded guest→Java calls
// (`ForwardingJni::SetCallTrace`), not guessed: a class missing here is a named `NoClassDefFoundError` in the
// boot report, which is the list to add from.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "rb/java/table_java.h"

namespace rb::java {

/// What the base profile needs to know about the device the app is running on.
struct AndroidEnv {
  std::string package_name = "com.retrobug.game";
  std::string apk_path;  ///< the APK the game was imported from (ApplicationInfo.sourceDir)
  std::string files_dir;
  std::string cache_dir;
  std::string model = "iPad";
  std::string manufacturer = "Apple";
  std::string brand = "Apple";
  std::string device = "iPad";
  std::string fingerprint = "retrobug/ios";
  std::string release = "14";
  std::int32_t sdk_int = 34;
  /// Wall-clock milliseconds (System.currentTimeMillis) and monotonic milliseconds (SystemClock.uptimeMillis).
  std::function<std::int64_t()> now_ms;
  std::function<std::int64_t()> uptime_ms;
  /// What android.util.Log receives (priority, tag, text). Empty: dropped.
  std::function<void(int priority, const std::string& tag, const std::string& text)> log;
  /// Text rendering: the Cocos2d-x Java class computes a bitmap and hands the pixels to the guest's
  /// nativeInitBitmapDC. `pixels` is a TableJava byte[] reference of width*height RGBA bytes.
  std::function<void(int width, int height, std::uintptr_t pixels)> publish_text;
};

/// Registers `java/lang/Object`, `java/lang/String`, `java/lang/System`, `java/lang/Integer`,
/// `android/os/SystemClock`, `android/os/Build` (and `Build$VERSION`), `android/util/Log`, `java/util/Locale`,
/// `java/io/File`, `android/content/Context` and `android/app/Activity`.
void InstallAndroidBase(TableJava& java, AndroidEnv env);

}  // namespace rb::java
