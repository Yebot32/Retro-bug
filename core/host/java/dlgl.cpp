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

  // The engine tracks through static Java methods. KitKat's native wrappers pass one String argument;
  // without these GetStaticMethodID fails and leaves a pending exception that the game never clears.
  const std::string tracker = "com/doubleleft/GATracker";
  NoOp(j, tracker, "init", "(Ljava/lang/String;)V");
  NoOp(j, tracker, "trackEvent", "(Ljava/lang/String;)V");
  NoOp(j, tracker, "trackPageView", "(Ljava/lang/String;)V");

  // Google Play Games is optional on this host: answer as signed out and accept every later event.
  const std::string games = "com/doubleleft/GooglePlayServices";
  NoOp(j, games, "init", "(Ljava/lang/String;)V");
  NoOp(j, games, "reportAchievement", "(Ljava/lang/String;)V");
  NoOp(j, games, "sendScore", "(Ljava/lang/String;)V");
  NoOp(j, games, "signIn", "(Ljava/lang/String;)V");
  NoOp(j, games, "signOut", "(Ljava/lang/String;)V");

  // The Activity methods the native extras screens call. They are OS actions (open a video, set a
  // wallpaper, confirm a score reset), not part of gameplay; answer them without leaving the app.
  const std::string activity = "com/doubleleft/androidkitkat/AndroidKitKatActivity";
  NoOp(j, activity, "openVideo", "(Ljava/lang/String;)V");
  NoOp(j, activity, "resetScoreAlert", "(Ljava/lang/String;)V");
  NoOp(j, activity, "setWallpaper", "(Ljava/lang/String;)V");

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
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewInstance("android/content/SharedPreferences"));
                 });

  // SharedPreferences: a real object whose getters answer their defaults and whose editor accepts writes.
  // A null here is the kind of thing an engine stores and dereferences later without a check.
  const std::string prefs = "android/content/SharedPreferences";
  const std::string editor = "android/content/SharedPreferences$Editor";
  j.DefineClass(prefs, "java/lang/Object");
  j.DefineClass(editor, "java/lang/Object");
  j.DefineMethod(prefs, "getFloat", "(Ljava/lang/String;F)F", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>& a) { return a[1]; });
  j.DefineMethod(prefs, "getInt", "(Ljava/lang/String;I)I", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>& a) { return a[1]; });
  j.DefineMethod(prefs, "getLong", "(Ljava/lang/String;J)J", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>& a) { return a[1]; });
  j.DefineMethod(prefs, "getBoolean", "(Ljava/lang/String;Z)Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>& a) { return a[1]; });
  j.DefineMethod(prefs, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>& a) { return a[1]; });
  j.DefineMethod(prefs, "contains", "(Ljava/lang/String;)Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod(prefs, "edit", "()Landroid/content/SharedPreferences$Editor;", false,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewInstance("android/content/SharedPreferences$Editor"));
                 });
  j.DefineMethod(editor, "putFloat", "(Ljava/lang/String;F)Landroid/content/SharedPreferences$Editor;", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.MakeRef(self));
                 });
  j.DefineMethod(editor, "putInt", "(Ljava/lang/String;I)Landroid/content/SharedPreferences$Editor;", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.MakeRef(self));
                 });
  j.DefineMethod(editor, "putLong", "(Ljava/lang/String;J)Landroid/content/SharedPreferences$Editor;", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.MakeRef(self));
                 });
  j.DefineMethod(editor, "putBoolean", "(Ljava/lang/String;Z)Landroid/content/SharedPreferences$Editor;", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.MakeRef(self));
                 });
  j.DefineMethod(editor, "putString",
                 "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.MakeRef(self));
                 });
  j.DefineMethod(editor, "commit", "()Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(1); });
  NoOpVirtual(j, editor, "apply", "()V");

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
