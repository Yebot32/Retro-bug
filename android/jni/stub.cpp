// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// librbstub.so: the 64-bit stand-in a repackaged game loads instead of its 32-bit library. The repackager
// copies it to `lib/arm64-v8a/<libX.so>` once per 32-bit library, so the game's own
// `System.loadLibrary("X")` finds it. Built without a DT_SONAME, so each copy is known to the dynamic
// linker by its own file name. Its `JNI_OnLoad` works out which library it stands in for (its own file
// name) and the game's APK, and hands both to `rb_runtime_boot` in librbguest.so.

#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <jni.h>

#include <string>

#include "runtime_boot.h"

namespace {

constexpr const char* kTag = "RBRUNTIME";

/// `ActivityThread.currentApplication().getApplicationInfo().sourceDir`, or empty with a logged reason.
std::string ApkPath(JavaVM* vm) {
  JNIEnv* env = nullptr;
  if (vm == nullptr || vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "stub: no JNIEnv on this thread");
    return {};
  }
  const auto fail = [&](const char* what) -> std::string {
    if (env->ExceptionCheck()) env->ExceptionClear();
    __android_log_print(ANDROID_LOG_WARN, kTag, "stub: could not read the APK path: %s", what);
    return {};
  };
  jclass at = env->FindClass("android/app/ActivityThread");
  if (at == nullptr) return fail("FindClass android.app.ActivityThread");
  jmethodID current = env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;");
  if (current == nullptr) return fail("ActivityThread.currentApplication");
  jobject app = env->CallStaticObjectMethod(at, current);
  if (env->ExceptionCheck() || app == nullptr) return fail("currentApplication() returned no Application");
  jclass ctx = env->GetObjectClass(app);
  jmethodID get_info =
      env->GetMethodID(ctx, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
  if (get_info == nullptr) return fail("Context.getApplicationInfo");
  jobject info = env->CallObjectMethod(app, get_info);
  if (env->ExceptionCheck() || info == nullptr) return fail("getApplicationInfo() returned null");
  jclass info_class = env->GetObjectClass(info);
  jfieldID source_dir = env->GetFieldID(info_class, "sourceDir", "Ljava/lang/String;");
  if (source_dir == nullptr) return fail("ApplicationInfo.sourceDir");
  auto dir = static_cast<jstring>(env->GetObjectField(info, source_dir));
  if (dir == nullptr) return fail("sourceDir is null");
  const char* chars = env->GetStringUTFChars(dir, nullptr);
  if (chars == nullptr) return fail("GetStringUTFChars(sourceDir)");
  std::string out = chars;
  env->ReleaseStringUTFChars(dir, chars);
  env->DeleteLocalRef(dir);
  env->DeleteLocalRef(info_class);
  env->DeleteLocalRef(info);
  env->DeleteLocalRef(ctx);
  env->DeleteLocalRef(app);
  env->DeleteLocalRef(at);
  return out;
}

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  // Our own path names both what we stand in for (the basename) and the APK: `<sourceDir>/lib/arm64/libX.so`, or
  // `<sourceDir>/base.apk!/lib/arm64-v8a/libX.so` when loaded straight from the APK. The hidden ActivityThread
  // API is only the fallback for a path of neither shape.
  std::string lib, apk;
  Dl_info self{};
  if (dladdr(reinterpret_cast<void*>(&JNI_OnLoad), &self) != 0 && self.dli_fname != nullptr) {
    const std::string path = self.dli_fname;
    const auto slash = path.rfind('/');
    lib = slash == std::string::npos ? path : path.substr(slash + 1);
    if (const auto bang = path.find("!/"); bang != std::string::npos) {
      apk = path.substr(0, bang);
    } else if (const auto at = path.rfind("/lib/"); at != std::string::npos) {
      apk = path.substr(0, at) + "/base.apk";
    }
  } else {
    __android_log_print(ANDROID_LOG_WARN, kTag, "stub: dladdr could not name this library");
  }
  if (apk.empty() || ::access(apk.c_str(), R_OK) != 0) apk = ApkPath(vm);
  return rb_runtime_boot(vm, apk.c_str(), lib.c_str());
}
