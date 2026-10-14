// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// librbstub.so: the 64-bit stand-in a repackaged game loads instead of its 32-bit library. The repackager
// copies it to `lib/arm64-v8a/<libX.so>` once per 32-bit library, so the game's own
// `System.loadLibrary("X")` finds it. Built without a DT_SONAME, so each copy is known to the dynamic
// linker by its own file name. Its `JNI_OnLoad` works out which library it stands in for (its own file
// name) and the game's APK, and hands both to `rb_runtime_boot` in librbguest.so.

#include <android/log.h>
#include <dlfcn.h>
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
  // Our own file's basename: the copy's name is the name of the 32-bit library it stands in for.
  std::string lib;
  Dl_info self{};
  if (dladdr(reinterpret_cast<void*>(&JNI_OnLoad), &self) != 0 && self.dli_fname != nullptr) {
    lib = self.dli_fname;
    const auto slash = lib.rfind('/');
    if (slash != std::string::npos) lib.erase(0, slash + 1);
  } else {
    __android_log_print(ANDROID_LOG_WARN, kTag, "stub: dladdr could not name this library");
  }
  const std::string apk = ApkPath(vm);
  return rb_runtime_boot(vm, apk.c_str(), lib.c_str());
}
