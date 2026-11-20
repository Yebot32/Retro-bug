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
