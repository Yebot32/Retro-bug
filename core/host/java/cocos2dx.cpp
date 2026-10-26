// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The Cocos2d-x Java helper profile: see rb/java/cocos2dx.h.

#include "rb/java/cocos2dx.h"

#include <algorithm>
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
  NoOp(j, "org/cocos2dx/lib/Cocos2dxGLSurfaceView", "openIMEKeyboard", "()V");
  NoOp(j, "org/cocos2dx/lib/Cocos2dxGLSurfaceView", "closeIMEKeyboard", "()V");
  // Cocos2d-x 2.2.x text: the Java class rasterises the string and hands ARGB_8888 pixels to the guest's
  // nativeInitBitmapDC. CoreText does the rasterising; without it the label is correctly sized and blank.
  j.DefineMethod("org/cocos2dx/lib/Cocos2dxBitmap", "createTextBitmapShadowStroke",
                 "([BLjava/lang/String;IFFFIIIZFFFZFFFF)V", true,
                 [env](TableJava& f, int, const std::vector<std::uint64_t>& a) -> std::uint64_t {
                   if (!env.publish_text) return 0;
                   const auto text = static_cast<TableJava::Ref>(a[0]);
                   const float size = static_cast<float>(static_cast<std::int32_t>(a[2]));
                   const auto max_w = static_cast<std::int32_t>(a[7]);
                   const auto max_h = static_cast<std::int32_t>(a[8]);
                   const std::int32_t len = f.GetArrayLength(text);
                   std::vector<char> utf8(static_cast<std::size_t>(len > 0 ? len : 0));
                   if (len > 0 && !f.GetArrayRegion('B', text, 0, len, utf8.data())) return 0;
                   int w = 0, h = 0;
                   std::vector<std::uint8_t> pixels;
                   const std::string line(utf8.begin(), utf8.end());
                   if (!env.render_text || !env.render_text(line, size, max_w, max_h, w, h, pixels)) {
                     w = std::max(1, static_cast<int>(utf8.size() * size * 0.55f));
                     if (max_w > 0) w = std::min(w, max_w);
                     h = std::max(1, static_cast<int>(size * 1.35f));
                     if (max_h > 0) h = std::min(h, max_h);
                     pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
                   }
                   const auto array = f.NewArray('B', w * h * 4, 0, 0);
                   if (array == 0) return 0;
                   if (!pixels.empty()) {
                     f.SetArrayRegion('B', array, 0, static_cast<std::int32_t>(pixels.size()), pixels.data());
                   }
                   env.publish_text(w, h, array);
                   return std::uint64_t{0};
                 });
  j.DefineMethod("org/cocos2dx/lib/Cocos2dxBitmap", "getFontSizeAccordingHeight", "(I)I", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>& a) { return a[0]; });
  j.DefineMethod("org/cocos2dx/lib/Cocos2dxBitmap", "getStringWithEllipsis",
                 "(Ljava/lang/String;FF)Ljava/lang/String;", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>& a) { return a[0]; });
  NoOp(j, cls, "runOnUiThread", "(Ljava/lang/Runnable;)V");
  NoOp(j, cls, "showDialog", "(Ljava/lang/String;Ljava/lang/String;)V");
  NoOp(j, cls, "showEditTextDialog", "(Ljava/lang/String;Ljava/lang/String;IIII)V");
}

}  // namespace rb::java
