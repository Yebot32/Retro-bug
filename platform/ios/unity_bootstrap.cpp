// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "unity_bootstrap.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <utility>

#include "rb/host/boot.h"

namespace rb::platform::ios {
namespace {
constexpr char kPlayer[] = "com/unity3d/player/UnityPlayer";
constexpr char kActivity[] = "com/unity3d/player/UnityPlayerActivity";
constexpr char kActivitySig[] = "Landroid/app/Activity;";
constexpr char kSurface[] = "android/view/Surface";
constexpr char kIntent[] = "android/content/Intent";
constexpr char kBundle[] = "android/os/Bundle";
constexpr char kManager[] = "android/content/pm/PackageManager";
constexpr char kPackageInfo[] = "android/content/pm/PackageInfo";

using Java = java::TableJava;

void PutObject(Java& vm, int object, const char* field, Java::Ref value) {
  vm.PutField(object, field, vm.ObjectOf(value));
}

void PutString(Java& vm, int object, const char* field, const std::u16string& text) {
  const auto value = vm.NewString(text);
  PutObject(vm, object, field, value);
  vm.DeleteLocalRef(value);
}

void InstallIntent(Java& vm) {
  vm.DefineClass(kIntent, "java/lang/Object");
  vm.DefineField(kIntent, "action", "Ljava/lang/String;", false);
  vm.DefineField(kIntent, "package", "Ljava/lang/String;", false);
  vm.DefineMethod(kIntent, "<init>", "()V", false, [](Java&, int, const auto&) { return 0u; });
  vm.DefineMethod(kIntent, "<init>", "(Ljava/lang/String;)V", false, [](Java& j, int self, const auto& args) {
    PutObject(j, self, "action", args.at(0));
    return 0u;
  });
  vm.DefineMethod("android/app/Activity", "getIntent", "()Landroid/content/Intent;", false,
                  [](Java& j, int, const auto&) {
    const auto intent = j.NewInstance(kIntent);
    PutString(j, j.ObjectOf(intent), "action", u"android.intent.action.MAIN");
    return intent;
  });
  vm.DefineMethod("java/lang/Object", "toString", "()Ljava/lang/String;", false,
                  [](Java& j, int self, const auto&) {
    char identity[16];
    std::snprintf(identity, sizeof identity, "%x", static_cast<unsigned>(self));
    auto name = j.ObjectClassName(self);
    std::replace(name.begin(), name.end(), '/', '.');
    const std::string text = name + "@" + identity;
    return j.NewString(std::u16string(text.begin(), text.end()));
  });
  vm.DefineMethod(kIntent, "toString", "()Ljava/lang/String;", false, [](Java& j, int self, const auto&) {
    auto action = j.StringOf(static_cast<int>(j.FieldOf(self, "action")));
    auto package = j.StringOf(static_cast<int>(j.FieldOf(self, "package")));
    std::u16string text = u"Intent {";
    if (!action.empty()) text += u" act=" + action;
    if (!package.empty()) text += u" pkg=" + package;
    return j.NewString(text + u" }");
  });
  for (const auto& [name, field] : {std::pair{"getAction", "action"}, {"getPackage", "package"}}) {
    vm.DefineMethod(kIntent, name, "()Ljava/lang/String;", false, [field](Java& j, int self, const auto&) {
      return j.MakeRef(static_cast<int>(j.FieldOf(self, field)));
    });
  }
  vm.DefineMethod(kIntent, "getExtras", "()Landroid/os/Bundle;", false,
                  [](Java&, int, const auto&) { return 0u; });
  vm.DefineMethod(kIntent, "setPackage", "(Ljava/lang/String;)Landroid/content/Intent;", false,
                  [](Java& j, int self, const auto& args) {
    PutObject(j, self, "package", args.at(0));
    return j.MakeRef(self);
  });
}

void InstallPackageManager(Java& vm, const ApkIdentity& identity, const std::u16string& apk_path) {
  const auto empty = [](Java&, int, const auto&) { return 0u; };
  vm.DefineClass(kBundle, "java/lang/Object");
  vm.DefineClass(kManager, "java/lang/Object");
  vm.DefineClass(kPackageInfo, "java/lang/Object");
  vm.DefineClass("android/app/ApplicationErrorReport", "java/lang/Object");
  vm.DefineField(kPackageInfo, "packageName", "Ljava/lang/String;", false);
  vm.DefineField(kPackageInfo, "versionName", "Ljava/lang/String;", false);
  vm.DefineField(kPackageInfo, "versionCode", "I", false);
  vm.DefineField(kPackageInfo, "applicationInfo", "Landroid/content/pm/ApplicationInfo;", false);
  vm.DefineField("android/content/pm/PackageItemInfo", "metaData", "Landroid/os/Bundle;", false);
  vm.DefineMethod("android/content/Context", "getPackageManager", "()Landroid/content/pm/PackageManager;",
                  false, [](Java& j, int, const auto&) { return j.NewInstance(kManager); });
  // A sideloaded APK on iOS has neither a Play installer nor an Android error-report receiver.
  vm.DefineMethod(kManager, "getInstallerPackageName", "(Ljava/lang/String;)Ljava/lang/String;", false, empty);
  vm.DefineMethod(kManager, "resolveActivity", "(Landroid/content/Intent;I)Landroid/content/pm/ResolveInfo;",
                  false, empty);
  vm.DefineMethod(kManager, "hasSystemFeature", "(Ljava/lang/String;)Z", false, empty);
  vm.DefineMethod("android/app/ApplicationErrorReport", "getErrorReportReceiver",
                  "(Landroid/content/Context;Ljava/lang/String;I)Landroid/content/ComponentName;", true, empty);
  vm.DefineMethod(kBundle, "getBoolean", "(Ljava/lang/String;Z)Z", false,
                  [](Java&, int, const auto& args) { return args.at(1); });
  vm.DefineMethod(kBundle, "getBoolean", "(Ljava/lang/String;)Z", false, empty);
  const std::u16string package(identity.package.begin(), identity.package.end());
  const auto application_info = [package, apk_path](Java& j) {
    const auto info = j.NewInstance("android/content/pm/ApplicationInfo");
    int object = j.ObjectOf(info);
    PutString(j, object, "packageName", package);
    PutString(j, object, "sourceDir", apk_path);
    return info;
  };
  vm.DefineMethod("android/content/Context", "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;",
                  false, [application_info](Java& j, int, const auto&) { return application_info(j); });
  const auto get_application = [package, application_info](Java& j, int, const auto& args) -> Java::Ref {
    if (j.StringOf(j.ObjectOf(args.at(0))) != package) {
      j.SetPending("android/content/pm/PackageManager$NameNotFoundException", "package is not imported");
      return 0;
    }
    return application_info(j);
  };
  vm.DefineMethod(kManager, "getApplicationInfo", "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;",
                  false, get_application);
  vm.DefineMethod(kManager, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", false,
                  [get_application, package, identity](Java& j, int self, const auto& args) -> Java::Ref {
    const auto application = get_application(j, self, args);
    if (application == 0) return 0;
    const auto info = j.NewInstance(kPackageInfo);
    const auto object = j.ObjectOf(info);
    PutString(j, object, "packageName", package);
    PutString(j, object, "versionName", identity.version_name);
    j.PutField(object, "versionCode", identity.version_code);
    PutObject(j, object, "applicationInfo", application);
    j.DeleteLocalRef(application);
    return info;
  });
}

void InstallPlayerCallbacks(Java& vm, const UnityBootstrap::JavaServices& services) {
  const auto empty = [](Java&, int, const auto&) { return 0u; };
  // Unity caches these method IDs together. For services the shell cannot supply, report zero devices,
  // unavailable, or no result according to the method's return type.
  for (const auto& [name, sig] : {
           std::pair{"Location_IsServiceEnabledByUser", "()Z"}, {"getNumCameras", "()I"},
           {"initCamera", "(IIII)[I"}, {"isCameraFrontFacing", "(I)Z"}, {"getCameraOrientation", "(I)I"},
           {"installPresentationDisplay", "(I)Z"}, {"getSplashMode", "()I"},
           {"executeGLThreadJobs", "()V"}, {"disableLogger", "()V"},
           {"startActivityIndicator", "()V"}, {"stopActivityIndicator", "()V"}, {"setFullscreen", "(Z)V"}}) {
    vm.DefineMethod(kPlayer, name, sig, false, empty);
  }
  for (const auto& [name, sig] : {
           std::pair{"showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;)V"},
           {"hideSoftInput", "()V"}, {"setSoftInputStr", "(Ljava/lang/String;)V"},
           {"showVideoPlayer", "(Ljava/lang/String;IIIZII)V"}, {"Location_SetDesiredAccuracy", "(F)V"},
           {"Location_SetDistanceFilter", "(F)V"}, {"Location_StartUpdatingLocation", "()V"},
           {"Location_StopUpdatingLocation", "()V"}, {"closeCamera", "(I)V"}, {"restartFMODAudioDevice", "()V"}}) {
    vm.DefineMethod(kPlayer, name, sig, false,
                    [name, log = services.log, reported = std::make_shared<std::atomic<bool>>(false)]
                    (Java&, int, const auto&) {
      if (log && !reported->exchange(true, std::memory_order_relaxed)) {
        log(std::string("unity: Android service unavailable: UnityPlayer.") + name);
      }
      return 0u;
    });
  }
  vm.DefineMethod(kPlayer, "kill", "()V", false, [shutdown = services.request_shutdown](Java&, int, const auto&) {
    if (shutdown) shutdown();
    return 0u;
  });
  vm.DefineMethod(kPlayer, "loadLibrary", "(Ljava/lang/String;)Z", false,
                  [load = services.load_library](Java& j, int, const auto& args) {
    const auto name = j.StringOf(j.ObjectOf(args.at(0)));
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](char16_t c) {
          return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') ||
                 c == u'_' || c == u'-' || c == u'.';
        })) return false;
    return load && load(std::string(name.begin(), name.end()));
  });
}

void InstallPowerManager(Java& vm) {
  constexpr char context[] = "android/content/Context";
  constexpr char manager[] = "android/os/PowerManager";
  constexpr char lock[] = "android/os/PowerManager$WakeLock";
  vm.DefineClass(manager, "java/lang/Object");
  vm.DefineClass(lock, "java/lang/Object");
  vm.DefineField(context, "POWER_SERVICE", "Ljava/lang/String;", true);
  vm.DefineField(context, "MODE_PRIVATE", "I", true);  // Android's value is zero.
  const auto cls = vm.FindClass(context);
  const auto power_field = vm.GetFieldID(cls, "POWER_SERVICE", "Ljava/lang/String;", true);
  const auto power = vm.NewString(u"power");
  vm.SetField('L', true, cls, power_field, power);
  vm.DeleteLocalRef(power);
  vm.DeleteLocalRef(cls);
  vm.DefineMethod(context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", false,
                  [](Java& j, int, const auto& args) {
    return j.StringOf(j.ObjectOf(args.at(0))) == u"power" ? j.NewInstance("android/os/PowerManager") : 0;
  });
  vm.DefineMethod(manager, "newWakeLock", "(ILjava/lang/String;)Landroid/os/PowerManager$WakeLock;", false,
                  [](Java& j, int, const auto&) { return j.NewInstance("android/os/PowerManager$WakeLock"); });
  vm.DefineMethod(lock, "isHeld", "()Z", false,
                  [](Java& j, int self, const auto&) { return j.FieldOf(self, "held") != 0; });
  for (const auto& [name, held] : {std::pair{"acquire", true}, {"release", false}}) {
    vm.DefineMethod(lock, name, "()V", false, [held](Java& j, int self, const auto&) {
      j.PutField(self, "held", held);
      return 0u;
    });
  }
}
}

void UnityBootstrap::InstallJava(java::TableJava& java, const std::string& apk, const ApkIdentity& identity,
                                 const JavaServices& services) {
  java.DefineClass("android/view/View", "java/lang/Object");
  java.DefineClass("android/view/ViewGroup", "android/view/View");
  java.DefineClass("android/widget/FrameLayout", "android/view/ViewGroup");
  java.DefineClass(kPlayer, "android/widget/FrameLayout");
  java.DefineClass(kActivity, "android/app/Activity");
  java.DefineClass(kSurface, "java/lang/Object");
  java.DefineClass("com/unity3d/player/ReflectionHelper", "java/lang/Object");
  InstallIntent(java);
  InstallPlayerCallbacks(java, services);
  InstallPowerManager(java);
  java.DefineField(kPlayer, "currentActivity", kActivitySig, true);
  const auto apk_path = std::filesystem::path(apk).u16string();
  InstallPackageManager(java, identity, apk_path);
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
