// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See rb/java/dlgl.h.

#include "rb/java/dlgl.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rb::java {

namespace {

void NoOp(TableJava& j, const std::string& cls, const std::string& name, const std::string& sig) {
  j.DefineMethod(cls, name, sig, true,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
}

void NoOpVirtual(TableJava& j, const std::string& cls, const std::string& name, const std::string& sig) {
  j.DefineMethod(cls, name, sig, false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
}

}  // namespace

void InstallDlgl(TableJava& j, const AndroidEnv& env) {
  (void)env;
  // The classes its JNI_OnLoad names before RegisterNatives: a missing class turns registration into a
  // NoClassDefFoundError and the bootstrap reports it by name.
  for (const char* cls : {"com/doubleleft/DLGLSurface", "com/doubleleft/DLGLRenderer",
                          "com/doubleleft/AdmobPlugin", "com/doubleleft/GATracker",
                          "com/doubleleft/GooglePlayServices", "com/doubleleft/androidkitkat/AndroidKitKatActivity",
                          "android/media/AudioTrack", "android/content/SharedPreferences"}) {
    j.DefineClass(cls, "java/lang/Object");
  }

  // LocalStorage: the engine's saves. Defaults until a real per-game store lands; a save is a no-op.
  const std::string storage = "com/doubleleft/LocalStorage";
  j.DefineMethod(storage, "loadFloat", "(Ljava/lang/String;)F", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod(storage, "loadString", "(Ljava/lang/String;)Ljava/lang/String;", true,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewJavaString(u""));
                 });
  NoOp(j, storage, "saveFloat", "(Ljava/lang/String;F)V");
  NoOp(j, storage, "saveString", "(Ljava/lang/String;Ljava/lang/String;)V");
  j.DefineMethod(storage, "getPreferences", "()Landroid/content/SharedPreferences;", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });

  // AudioTrack, the engine's non-OpenSL fallback: create, accept every buffer, report playing.
  const std::string track = "android/media/AudioTrack";
  j.DefineMethod(track, "<init>", "(IIIIII)V", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod(track, "getMinBufferSize", "(III)I", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(4096); });
  j.DefineMethod(track, "write", "([BII)I", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>& a) { return a[2]; });
  NoOpVirtual(j, track, "play", "()V");
  NoOpVirtual(j, track, "stop", "()V");
  NoOpVirtual(j, track, "release", "()V");
  NoOpVirtual(j, track, "flush", "()V");
  j.DefineMethod(track, "getPlayState", "()I", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(3); });
  j.DefineMethod(track, "getState", "()I", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(1); });
}

}  // namespace rb::java
