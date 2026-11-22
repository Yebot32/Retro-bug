// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "unity_bootstrap.h"

#include <filesystem>
#include <utility>

#include "rb/host/boot.h"

namespace rb::platform::ios {
namespace {
constexpr char kPlayer[] = "com/unity3d/player/UnityPlayer";
constexpr char kActivity[] = "com/unity3d/player/UnityPlayerActivity";
constexpr char kActivitySig[] = "Landroid/app/Activity;";
constexpr char kSurface[] = "android/view/Surface";
}

void UnityBootstrap::InstallJava(java::TableJava& java, const std::string& apk) {
  java.DefineClass("android/view/View", "java/lang/Object");
  java.DefineClass("android/view/ViewGroup", "android/view/View");
  java.DefineClass("android/widget/FrameLayout", "android/view/ViewGroup");
  java.DefineClass(kPlayer, "android/widget/FrameLayout");
  java.DefineClass(kActivity, "android/app/Activity");
  java.DefineClass(kSurface, "java/lang/Object");
  java.DefineField(kPlayer, "currentActivity", kActivitySig, true);
  const auto apk_path = std::filesystem::path(apk).u16string();
  for (const char* method : {"getPackageCodePath", "getPackageResourcePath"}) {
    java.DefineMethod("android/content/Context", method, "()Ljava/lang/String;", false,
                      [apk_path](java::TableJava& vm, int, const auto&) { return vm.NewString(apk_path); });
  }
}

UnityBootstrap::UnityBootstrap(host::Boot& boot, java::TableJava& java) : boot_(boot), java_(java) {}

UnityBootstrap::~UnityBootstrap() {
  if (receiver_ != 0) java_.DeleteGlobalRef(receiver_);
  if (activity_ != 0) java_.DeleteGlobalRef(activity_);
  if (surface_ != 0) java_.DeleteGlobalRef(surface_);
}

bool UnityBootstrap::Has(const char* name, const char* signature) const {
  for (const auto& binding : java_.natives(kPlayer)) {
    if (binding.name == name && binding.sig == signature) return true;
  }
  return false;
}

bool UnityBootstrap::Call(const char* name, const char* signature, const std::vector<std::uint64_t>& args,
                          std::string* why, std::uint64_t* result) {
  const auto method = std::string(name) + signature;
  std::string error;
  const auto value = boot_.CallNative(method, receiver_, args, &error);
  if (error.empty() && java_.ExceptionCheck()) error = "pending Java exception " + java_.PendingClass();
  // Inbound reports a guest fault separately from the method lookup error.
  if (error.empty() && boot_.process()->monitor().group_exited()) error = "guest process exited";
  if (!error.empty()) {
    *why = "UnityPlayer." + method + ": " + error;
    return false;
  }
  if (result != nullptr) *result = value;
  return true;
}

bool UnityBootstrap::Start(const std::string& apk, std::string* why) {
  if (receiver_ != 0) {
    return Attach(why);
  }
  for (const auto& [name, sig] : {std::pair{"initJni", "(Landroid/content/Context;)V"},
                                 {"nativeRecreateGfxState", "(ILandroid/view/Surface;)V"},
                                 {"nativeRender", "()Z"}, {"nativeResume", "()V"},
                                 {"nativePause", "()Z"}, {"nativeFocusChanged", "(Z)V"}}) {
    if (!Has(name, sig)) {
      *why = std::string("libunity.so did not register UnityPlayer.") + name + sig;
      return false;
    }
  }
  const auto pin = [this](const char* name) {
    const auto local = java_.NewInstance(name);
    const auto global = java_.NewGlobalRef(local);
    java_.DeleteLocalRef(local);
    return global;
  };
  receiver_ = pin(kPlayer);
  activity_ = pin(kActivity);
  surface_ = pin(kSurface);
  const auto player = java_.FindClass(kPlayer);
  const auto activity_field = java_.GetFieldID(player, "currentActivity", kActivitySig, true);
  java_.SetField('L', true, player, activity_field, activity_);
  java_.DeleteLocalRef(player);

  boot_.Live("unity: initJni with UnityPlayerActivity");
  if (!Call("initJni", "(Landroid/content/Context;)V", {activity_}, why)) return false;
  if (Has("nativeFile", "(Ljava/lang/String;)V")) {
    boot_.Live("unity: register the APK as a data source");
    const auto path = java_.NewString(std::filesystem::path(apk).u16string());
    const bool accepted = Call("nativeFile", "(Ljava/lang/String;)V", {path}, why);
    java_.DeleteLocalRef(path);
    if (!accepted) return false;
  }
  return Attach(why);
}

bool UnityBootstrap::Attach(std::string* why) {
  boot_.Live("unity: attach primary render surface");
  if (!Call("nativeRecreateGfxState", "(ILandroid/view/Surface;)V", {0, surface_}, why)) return false;
  if (Has("nativeSendSurfaceChangedEvent", "()V") && !Call("nativeSendSurfaceChangedEvent", "()V", {}, why)) {
    return false;
  }
  return Resume(why);
}

bool UnityBootstrap::Render(std::string* why) {
  std::uint64_t keep_running = 0;
  if (!Call("nativeRender", "()Z", {}, why, &keep_running)) return false;
  if (keep_running != 0) return true;
  *why = "UnityPlayer.nativeRender returned false";
  return false;
}

bool UnityBootstrap::Pause(std::string* why) {
  std::uint64_t should_quit = 0;
  if (!Call("nativeFocusChanged", "(Z)V", {0}, why) ||
      !Call("nativePause", "()Z", {}, why, &should_quit)) return false;
  if (should_quit == 0) return true;
  *why = "UnityPlayer.nativePause requested shutdown";
  return false;
}

bool UnityBootstrap::Resume(std::string* why) {
  return Call("nativeFocusChanged", "(Z)V", {1}, why) && Call("nativeResume", "()V", {}, why);
}

}  // namespace rb::platform::ios
