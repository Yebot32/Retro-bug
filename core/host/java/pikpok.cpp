// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See rb/java/pikpok.h.

#include "rb/java/pikpok.h"

namespace rb::java {

namespace {

std::uint64_t NewString(TableJava& f, const std::string& s) {
  return static_cast<std::uint64_t>(f.NewJavaString(std::u16string(s.begin(), s.end())));
}

}  // namespace

void InstallPikPok(TableJava& java, const AndroidEnv& env) {
  java.DefineClass("com/pikpok/MabActivity", "android/app/Activity");
  // The engine's own renderer/input classes: static natives only, so the class exists to be the jclass a
  // static JNI call receives, and to name failed lookups in the trace.
  java.DefineClass("com/pikpok/MabRenderer", "java/lang/Object");
  java.DefineClass("com/pikpok/MabInput", "java/lang/Object");

  // The engine builds these Java objects itself (NewObjectA with the class's <init>(J)V — the timestamp is
  // the handle it gives the Java side) and then calls their methods: the sensors for orientation, the
  // gesture detector for pinch, the text field for the keyboard. Their bodies are the game's Java, which
  // does not run here; the methods answer without doing anything, which is what an iPad without a
  // accelerometer or a text cursor needs.
  for (const char* cls : {"com/pikpok/MabSensors", "com/pikpok/MabGesture", "com/pikpok/MabWebDownloader"}) {
    java.DefineClass(cls, "java/lang/Object");
    java.DefineMethod(cls, "<init>", "(J)V", false,
                      [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  }
  java.DefineMethod("com/pikpok/MabSensors", "Start", "()V", false,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/MabSensors", "Stop", "()V", false,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/MabSensors", "onPause", "()V", false,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/MabSensors", "onResume", "()V", false,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/MabSensors", "onDestroy", "()V", false,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/MabSensors", "SetMotionUpdateFrequency", "(F)V", false,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/MabGesture", "onScaleBegin", "(Landroid/view/ScaleGestureDetector;)Z", false,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/MabGesture", "onScale", "(Landroid/view/ScaleGestureDetector;)Z", false,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/MabGesture", "onScaleEnd", "(Landroid/view/ScaleGestureDetector;)V", false,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });

  // The engine's own Java utility class. The native calls these statics directly (JNI), so they must
  // exist with the same names and shapes; their bodies (the game's Java) do not run here, and the
  // answers below are what the shell can honestly give: the locale, that this is a tablet, the uptime
  // the engine times its animation with, and no stored values (the game's own preferences are not here).
  java.DefineClass("com/pikpok/Natives", "java/lang/Object");
  java.DefineMethod("com/pikpok/Natives", "GetDefaultLocale", "()Ljava/lang/String;", true,
                    [](TableJava& f, int, const std::vector<std::uint64_t>&) { return NewString(f, "en"); });
  java.DefineMethod("com/pikpok/Natives", "GetStoredString", "(Ljava/lang/String;)Ljava/lang/String;", true,
                    [](TableJava& f, int, const std::vector<std::uint64_t>&) { return NewString(f, ""); });
  java.DefineMethod("com/pikpok/Natives", "GetStoredInt", "(Ljava/lang/String;)I", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "GetStoredFloat", "(Ljava/lang/String;)F", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "GetStoredBool", "(Ljava/lang/String;)Z", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "GetStoredLongLong", "(Ljava/lang/String;)J", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "GetSystemInformation", "()[Ljava/lang/String;", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "GetUptimeSeconds", "()D", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "GetDeviceOdin1", "(Ljava/lang/String;)Ljava/lang/String;", true,
                    [](TableJava& f, int, const std::vector<std::uint64_t>&) { return NewString(f, ""); });
  java.DefineMethod("com/pikpok/Natives", "IsLowMemory", "()Z", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "IsTablet", "()Z", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{1}; });
  java.DefineMethod("com/pikpok/Natives", "RequestAudioFocus", "()Z", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{1}; });
  java.DefineMethod("com/pikpok/Natives", "SHA1", "(Ljava/lang/String;)Ljava/lang/String;", true,
                    [](TableJava& f, int, const std::vector<std::uint64_t>&) { return NewString(f, ""); });
  java.DefineMethod("com/pikpok/Natives", "convertToHex", "([B)Ljava/lang/String;", true,
                    [](TableJava& f, int, const std::vector<std::uint64_t>&) { return NewString(f, ""); });
  java.DefineMethod("com/pikpok/Natives", "SetStoredString", "(Ljava/lang/String;Ljava/lang/String;)V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "SetStoredInt", "(Ljava/lang/String;I)V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "SetStoredFloat", "(Ljava/lang/String;F)V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "SetStoredBool", "(Ljava/lang/String;Z)V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "SetStoredLongLong", "(Ljava/lang/String;J)V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "RemoveStoredValue", "(Ljava/lang/String;)V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "BackupStoredValues", "()V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "KeepScreenOn", "(Z)V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "OpenURL", "(Ljava/lang/String;)V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "OpenUpgradeURL", "()V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "QuitApp", "()V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/pikpok/Natives", "DisplayLoadingWheel", "(ZF)V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });

  // Dir + "/" is exactly what the dex's get*Path methods build (absolute path, then the File.separator if
  // the path does not already end with it).
  const std::string files = env.files_dir + "/";
  const std::string cache = env.cache_dir + "/";
  java.DefineMethod("com/pikpok/MabActivity", "getFilePath", "()Ljava/lang/String;", false,
                    [files](TableJava& f, int, const std::vector<std::uint64_t>&) { return NewString(f, files); });
  java.DefineMethod("com/pikpok/MabActivity", "getCachePath", "()Ljava/lang/String;", false,
                    [cache](TableJava& f, int, const std::vector<std::uint64_t>&) { return NewString(f, cache); });
  // The external files directory is for user-visible data; the app's own files directory serves the same
  // role here (the iOS container is already the app's sandbox).
  java.DefineMethod("com/pikpok/MabActivity", "getExternalFilesPath", "()Ljava/lang/String;", false,
                    [files](TableJava& f, int, const std::vector<std::uint64_t>&) { return NewString(f, files); });
  // The constructor sets both fields to "" and only an APK-expansion download replaces them.
  java.DefineMethod("com/pikpok/MabActivity", "getExpansionMainPath", "()Ljava/lang/String;", false,
                    [](TableJava& f, int, const std::vector<std::uint64_t>&) { return NewString(f, ""); });
  java.DefineMethod("com/pikpok/MabActivity", "getExpansionPatchPath", "()Ljava/lang/String;", false,
                    [](TableJava& f, int, const std::vector<std::uint64_t>&) { return NewString(f, ""); });
}

}  // namespace rb::java
