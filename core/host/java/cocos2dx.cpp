// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The Cocos2d-x Java helper profile: see rb/java/cocos2dx.h.

#include "rb/java/cocos2dx.h"

#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace rb::java {

namespace {

using Ref = TableJava::Ref;

std::u16string U(const std::string& s) { return std::u16string(s.begin(), s.end()); }

void StaticString(TableJava& j, const std::string& cls, const std::string& name, const std::string& sig,
                  std::function<std::string()> value) {
  j.DefineMethod(cls, name, sig, true,
                 [value = std::move(value)](TableJava& f, int, const std::vector<std::uint64_t>&) -> std::uint64_t {
                   return static_cast<std::uint64_t>(f.NewJavaString(U(value())));
                 });
}

void NoOp(TableJava& j, const std::string& cls, const std::string& name, const std::string& sig) {
  j.DefineMethod(cls, name, sig, true,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
}

}  // namespace

void InstallCocos2dx(TableJava& j, const AndroidEnv& env) {
  const std::string cls = "org/cocos2dx/lib/Cocos2dxHelper";
  j.DefineClass(cls, "java/lang/Object");

  j.DefineMethod(cls, "getAssetManager", "()Landroid/content/res/AssetManager;", true,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) -> std::uint64_t {
                   return static_cast<std::uint64_t>(f.NewInstance("android/content/res/AssetManager"));
                 });
  StaticString(j, cls, "getCocos2dxPackageName", "()Ljava/lang/String;", [env] { return env.package_name; });
  StaticString(j, cls, "getCocos2dxWritablePath", "()Ljava/lang/String;", [env] { return env.files_dir; });
  StaticString(j, cls, "getCurrentLanguage", "()Ljava/lang/String;", [] { return std::string{"en"}; });
  StaticString(j, cls, "getDeviceModel", "()Ljava/lang/String;", [env] { return env.model; });
  j.DefineMethod(cls, "getDPI", "()I", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(160); });

  // Preferences: there is no SharedPreferences yet, so a getter answers its default and a setter does nothing.
  j.DefineMethod(cls, "getStringForKey", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>& a) { return a[1]; });
  j.DefineMethod(cls, "getBoolForKey", "(Ljava/lang/String;Z)Z", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>& a) { return a[1]; });
  j.DefineMethod(cls, "getIntegerForKey", "(Ljava/lang/String;I)I", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>& a) { return a[1]; });
  j.DefineMethod(cls, "getFloatForKey", "(Ljava/lang/String;F)F", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>& a) { return a[1]; });
  j.DefineMethod(cls, "getDoubleForKey", "(Ljava/lang/String;D)D", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>& a) { return a[1]; });
  NoOp(j, cls, "setStringForKey", "(Ljava/lang/String;Ljava/lang/String;)V");
  NoOp(j, cls, "setBoolForKey", "(Ljava/lang/String;Z)V");
  NoOp(j, cls, "setIntegerForKey", "(Ljava/lang/String;I)V");
  NoOp(j, cls, "setFloatForKey", "(Ljava/lang/String;F)V");
  NoOp(j, cls, "setDoubleForKey", "(Ljava/lang/String;D)V");

  // The listener-driven and lifecycle entry points its own Java layer would call.
  for (const char* name : {"end", "onPause", "onResume", "enableAccelerometer", "disableAccelerometer",
                           "terminateProcess"}) {
    NoOp(j, cls, name, "()V");
  }
  NoOp(j, cls, "setAccelerometerInterval", "(F)V");

  // The Java classes the Cocos2d-x native code names by string. Only the statics it calls are answered;
  // the C++ renderer is driven directly by the iOS shell, so the Java view/renderer classes are empty.
  for (const char* view : {"org/cocos2dx/lib/Cocos2dxGLSurfaceView", "org/cocos2dx/lib/Cocos2dxRenderer",
                           "org/cocos2dx/lib/Cocos2dxBitmap"}) {
    j.DefineClass(view, "java/lang/Object");
  }
  j.DefineClass("org/cocos2dx/lib/Cocos2dxActivity", "android/app/Activity");
  // Cocos2d-x 3.x text: the Java class renders into a static bitmap and calls the native
  // nativeInitBitmapDC back. With no renderer here the method returns without doing so; the engine then
  // reports a failed label rather than crashing, which is enough to reach the first frame.
  j.DefineMethod("org/cocos2dx/lib/Cocos2dxBitmap", "createTextBitmapShadowStroke",
                 "([BLjava/lang/String;IFFFIIIZFFFZFFFF)V", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  NoOp(j, cls, "runOnUiThread", "(Ljava/lang/Runnable;)V");
  NoOp(j, cls, "showDialog", "(Ljava/lang/String;Ljava/lang/String;)V");
  NoOp(j, cls, "showEditTextDialog", "(Ljava/lang/String;Ljava/lang/String;IIII)V");
}

}  // namespace rb::java
