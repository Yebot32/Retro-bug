// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "unity_bootstrap.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <variant>
#include <utility>

#include "rb/host/boot.h"
#include "rb/loader/apk_assets.h"

namespace rb::platform::ios {
struct UnityJavaTasks {
  using Clock = std::chrono::steady_clock;
  struct Task { int handler, runnable; Clock::time_point due; };
  std::mutex mutex;
  std::deque<Task> pending;

  bool Post(int handler, int runnable, std::int64_t delay) {
    if (runnable == 0) return false;
    std::lock_guard<std::mutex> lock(mutex);
    if (pending.size() >= 1024) return false;
    // Android delays are signed milliseconds. Clamp to what the host clock can represent.
    const auto now = Clock::now();
    const auto max = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now).count();
    const auto due = now + std::chrono::milliseconds(std::clamp<std::int64_t>(delay, 0, max));
    const auto position = std::upper_bound(pending.begin(), pending.end(), due,
                                           [](auto time, const Task& task) { return time < task.due; });
    pending.insert(position, {handler, runnable, due});
    return true;
  }
};

namespace {
constexpr char kPlayer[] = "com/unity3d/player/UnityPlayer";
constexpr char kActivity[] = "com/unity3d/player/UnityPlayerActivity";
constexpr char kActivitySig[] = "Landroid/app/Activity;";
constexpr char kSurface[] = "android/view/Surface";
constexpr char kIntent[] = "android/content/Intent";
constexpr char kBundle[] = "android/os/Bundle";
constexpr char kManager[] = "android/content/pm/PackageManager";
constexpr char kPackageInfo[] = "android/content/pm/PackageInfo";
constexpr char kPrefs[] = "android/content/SharedPreferences";
constexpr char kEditor[] = "android/content/SharedPreferences$Editor";
constexpr char kMotion[] = "android/view/MotionEvent";
constexpr char kDisplayManager[] = "android/hardware/display/DisplayManager";
constexpr char kDisplay[] = "android/view/Display";
constexpr char kRunnable[] = "java/lang/Runnable";
constexpr char kBridge[] = "bitter/jnibridge/JNIBridge";
constexpr char kAssetStream[] = "android/content/res/AssetManager$AssetInputStream";
constexpr char kBridgeInvoke[] = "invoke(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;";

using Java = java::TableJava;

void InstallLegacyFmod(Java& vm, const UnityBootstrap::JavaServices& services) {
  constexpr char device[] = "org/fmod/FMODAudioDevice";
  vm.DefineClass(device, "java/lang/Object");
  const auto inactive = [](Java&, int, const auto&) { return 0u; };
  for (const char* name : {"<init>", "close", "stop", "stopAudioRecord"}) {
    vm.DefineMethod(device, name, "()V", false, inactive);
  }
  const auto logged = std::make_shared<bool>(false);
  vm.DefineMethod(device, "start", "()V", false, [log = services.log, logged](Java&, int, const auto&) {
    if (!std::exchange(*logged, true) && log) log("audio: legacy FMOD Java device has no iOS PCM bridge");
    return 0u;
  });
  vm.DefineMethod(device, "isRunning", "()Z", false, inactive);
  vm.DefineMethod(device, "startAudioRecord", "(III)I", false,
                  [](Java&, int, const auto&) { return static_cast<std::uint32_t>(-1); });
}

void InstallApkAssetStreams(Java& vm, const std::string& apk) {
  struct Stream {
    std::vector<std::uint8_t> bytes;
    std::size_t position = 0;
  };
  struct AssetState {
    std::unique_ptr<loader::ApkArchive> archive;
    std::map<int, Stream> streams;
  };
  const auto state = std::make_shared<AssetState>();
  vm.DefineClass("java/io/InputStream", "java/lang/Object");
  vm.DefineClass(kAssetStream, "java/io/InputStream");
  const auto open = [state, apk](Java& j, int, const auto& args) -> Java::Ref {
    const auto name = std::filesystem::path(j.GetStringChars(args.at(0))).generic_string();
    const auto entry = "assets/" + name;
    std::string error;
    if (!state->archive) state->archive = loader::ApkArchive::Open(apk, &error);
    const auto size = state->archive ? state->archive->EntrySize(entry) : std::nullopt;
    if (size && *size <= 64u * 1024 * 1024) {
      if (auto bytes = state->archive->Read(entry, &error)) {
        const auto ref = j.NewInstance(kAssetStream);
        state->streams.emplace(j.ObjectOf(ref), Stream{std::move(*bytes)});
        return ref;
      }
    }
    j.SetPending("java/io/FileNotFoundException", name);
    return 0;
  };
  vm.DefineClass("java/io/FileNotFoundException", "java/io/IOException");
  vm.DefineMethod("android/content/res/AssetManager", "open", "(Ljava/lang/String;)Ljava/io/InputStream;", false, open);
  vm.DefineMethod("android/content/res/AssetManager", "open", "(Ljava/lang/String;I)Ljava/io/InputStream;", false, open);
  vm.DefineMethod(kAssetStream, "read", "()I", false, [state](Java&, int self, const auto&) -> std::int32_t {
    const auto it = state->streams.find(self);
    if (it == state->streams.end() || it->second.position == it->second.bytes.size()) return -1;
    return it->second.bytes[it->second.position++];
  });
  const auto read_array = [state](Java& j, int self, const auto& args) -> std::int32_t {
    const auto length = j.GetArrayLength(args.at(0));
    const auto offset = args.size() == 1 ? 0 : static_cast<std::int32_t>(args.at(1));
    const auto count = args.size() == 1 ? length : static_cast<std::int32_t>(args.at(2));
    if (offset < 0 || count < 0 || offset > length || count > length - offset) {
      j.SetPending("java/lang/IndexOutOfBoundsException", "invalid asset read range");
      return -1;
    }
    if (count == 0) return 0;
    const auto it = state->streams.find(self);
    if (it == state->streams.end()) return -1;
    auto& source = it->second;
    const auto size = std::min<std::size_t>(count, source.bytes.size() - source.position);
    if (size == 0 || !j.SetArrayRegion('B', args.at(0), offset, size, source.bytes.data() + source.position)) return -1;
    source.position += size;
    return static_cast<std::int32_t>(size);
  };
  vm.DefineMethod(kAssetStream, "read", "([B)I", false, read_array);
  vm.DefineMethod(kAssetStream, "read", "([BII)I", false, read_array);
  vm.DefineMethod(kAssetStream, "available", "()I", false, [state](Java&, int self, const auto&) {
    const auto it = state->streams.find(self);
    return it == state->streams.end() ? 0 : it->second.bytes.size() - it->second.position;
  });
  vm.DefineMethod(kAssetStream, "close", "()V", false, [state](Java&, int self, const auto&) {
    state->streams.erase(self);
    return 0u;
  });
}

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
  vm.DefineField(kIntent, "ACTION_MAIN", "Ljava/lang/String;", true);
  const auto intent_class = vm.FindClass(kIntent);
  const auto action_main = vm.NewString(u"android.intent.action.MAIN");
  vm.SetField('L', true, intent_class, vm.GetFieldID(intent_class, "ACTION_MAIN", "Ljava/lang/String;", true),
              action_main);
  vm.DeleteLocalRef(action_main);
  vm.DeleteLocalRef(intent_class);
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

void InstallPackageManager(Java& vm, const ApkIdentity& identity, const std::u16string& apk_path,
                           const UnityBootstrap::JavaServices& services) {
  const auto empty = [](Java&, int, const auto&) { return 0u; };
  vm.DefineClass(kBundle, "java/lang/Object");
  vm.DefineClass(kManager, "java/lang/Object");
  vm.DefineField(kManager, "GET_META_DATA", "I", true);
  const auto manager = vm.FindClass(kManager);
  vm.SetField('I', true, manager, vm.GetFieldID(manager, "GET_META_DATA", "I", true), 0x80);
  for (const auto& [name, value] : {std::pair{"PERMISSION_GRANTED", 0u}, {"PERMISSION_DENIED", 0xffffffffu}}) {
    vm.DefineField(kManager, name, "I", true);
    vm.SetField('I', true, manager, vm.GetFieldID(manager, name, "I", true), value);
  }
  vm.DefineField(kManager, "FEATURE_AUDIO_LOW_LATENCY", "Ljava/lang/String;", true);
  const auto low_latency = vm.NewString(u"android.hardware.audio.low_latency");
  vm.SetField('L', true, manager, vm.GetFieldID(manager, "FEATURE_AUDIO_LOW_LATENCY", "Ljava/lang/String;", true),
              low_latency);
  vm.DeleteLocalRef(low_latency);
  vm.DeleteLocalRef(manager);
  vm.DefineClass(kPackageInfo, "java/lang/Object");
  vm.DefineClass("android/app/ApplicationErrorReport", "java/lang/Object");
  vm.DefineField(kPackageInfo, "packageName", "Ljava/lang/String;", false);
  vm.DefineField(kPackageInfo, "versionName", "Ljava/lang/String;", false);
  vm.DefineField(kPackageInfo, "versionCode", "I", false);
  vm.DefineField(kPackageInfo, "applicationInfo", "Landroid/content/pm/ApplicationInfo;", false);
  vm.DefineField("android/content/pm/PackageItemInfo", "metaData", "Landroid/os/Bundle;", false);
  for (const auto* name : {"publicSourceDir", "dataDir", "nativeLibraryDir"}) {
    vm.DefineField("android/content/pm/ApplicationInfo", name, "Ljava/lang/String;", false);
  }
  vm.DefineField("android/content/pm/ApplicationInfo", "targetSdkVersion", "I", false);
  vm.DefineField("android/content/pm/ApplicationInfo", "splitPublicSourceDirs", "[Ljava/lang/String;", false);
  vm.DefineMethod("android/content/Context", "getPackageManager", "()Landroid/content/pm/PackageManager;",
                  false, [](Java& j, int, const auto&) { return j.NewInstance(kManager); });
  // A sideloaded APK on iOS has neither a Play installer nor an Android error-report receiver.
  vm.DefineMethod(kManager, "getInstallerPackageName", "(Ljava/lang/String;)Ljava/lang/String;", false, empty);
  vm.DefineMethod(kManager, "resolveActivity", "(Landroid/content/Intent;I)Landroid/content/pm/ResolveInfo;",
                  false, empty);
  vm.DefineMethod(kManager, "hasSystemFeature", "(Ljava/lang/String;)Z", false, empty);
  vm.DefineMethod("android/app/ApplicationErrorReport", "getErrorReportReceiver",
                  "(Landroid/content/Context;Ljava/lang/String;I)Landroid/content/ComponentName;", true, empty);
  const auto metadata = std::make_shared<decltype(identity.metadata)>(identity.metadata);
  const auto lookup = [metadata](Java& j, int self, const auto& args) -> const decltype(metadata)::element_type::mapped_type* {
    if (!j.FieldOf(self, "manifest")) return nullptr;
    const auto key = j.GetStringChars(args.at(0));
    const auto value = metadata->find(key);
    return value == metadata->end() ? nullptr : &value->second;
  };
  vm.DefineMethod(kBundle, "containsKey", "(Ljava/lang/String;)Z", false,
                  [lookup](Java& j, int self, const auto& args) { return lookup(j, self, args) != nullptr; });
  for (const auto* sig : {"(Ljava/lang/String;)Z", "(Ljava/lang/String;Z)Z"}) {
    vm.DefineMethod(kBundle, "getBoolean", sig, false,
                    [lookup](Java& j, int self, const auto& args) -> std::uint64_t {
      const auto* value = lookup(j, self, args);
      if (value != nullptr) {
        if (const auto* boolean = std::get_if<bool>(value)) return *boolean;
      }
      return args.size() > 1 ? args.at(1) : 0;
    });
  }
  for (const auto* sig : {"(Ljava/lang/String;)I", "(Ljava/lang/String;I)I"}) {
    vm.DefineMethod(kBundle, "getInt", sig, false,
                    [lookup](Java& j, int self, const auto& args) -> std::uint64_t {
      const auto* value = lookup(j, self, args);
      if (value != nullptr) {
        if (const auto* integer = std::get_if<std::int32_t>(value)) return static_cast<std::uint32_t>(*integer);
      }
      return args.size() > 1 ? args.at(1) : 0;
    });
  }
  vm.DefineMethod(kBundle, "getString", "(Ljava/lang/String;)Ljava/lang/String;", false,
                  [lookup](Java& j, int self, const auto& args) -> Java::Ref {
    const auto* value = lookup(j, self, args);
    const auto* string = value != nullptr ? std::get_if<std::u16string>(value) : nullptr;
    return string != nullptr ? j.NewString(*string) : 0;
  });
  const std::u16string package(identity.package.begin(), identity.package.end());
  const auto data_dir = std::filesystem::path(services.data_dir).u16string();
  const auto lib_dir = std::filesystem::path(services.native_library_dir).u16string();
  const auto application_info = [package, apk_path, data_dir, lib_dir, metadata,
                                 target_sdk = identity.target_sdk](Java& j, bool include_metadata) {
    const auto info = j.NewInstance("android/content/pm/ApplicationInfo");
    int object = j.ObjectOf(info);
    PutString(j, object, "packageName", package);
    PutString(j, object, "sourceDir", apk_path);
    PutString(j, object, "publicSourceDir", apk_path);
    PutString(j, object, "dataDir", data_dir);
    PutString(j, object, "nativeLibraryDir", lib_dir);
    j.PutField(object, "targetSdkVersion", target_sdk);
    if (include_metadata && !metadata->empty()) {
      const auto bundle = j.NewInstance(kBundle);
      j.PutField(j.ObjectOf(bundle), "manifest", 1);
      PutObject(j, object, "metaData", bundle);
      j.DeleteLocalRef(bundle);
    }
    return info;
  };
  vm.DefineMethod("android/content/Context", "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;",
                  false, [application_info](Java& j, int, const auto&) { return application_info(j, true); });
  const auto get_application = [package, application_info](Java& j, int, const auto& args) -> Java::Ref {
    if (j.StringOf(j.ObjectOf(args.at(0))) != package) {
      j.SetPending("android/content/pm/PackageManager$NameNotFoundException", "package is not imported");
      return 0;
    }
    return application_info(j, (args.at(1) & 0x80) != 0);
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
           {"startActivityIndicator", "()V"}, {"stopActivityIndicator", "()V"}, {"setFullscreen", "(Z)V"},
           {"showVideoPlayer", "(Ljava/lang/String;IIIZII)Z"}, {"loadGoogleVR", "(ZZZ)J"},
           {"setGoogleVREnabled", "(Z)V"}, {"unloadGoogleVR", "()V"}, {"addPhoneCallListener", "()V"}}) {
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
                  [load = services.load_library, log = services.log](Java& j, int, const auto& args) {
    const auto name = j.StringOf(j.ObjectOf(args.at(0)));
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](char16_t c) {
          return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') ||
                 c == u'_' || c == u'-' || c == u'.';
        })) {
      if (log) log("unity: loadLibrary received an empty or invalid library name");
      return false;
    }
    return load && load(std::string(name.begin(), name.end()));
  });
}

void InstallSystemServices(Java& vm, const UnityBootstrap::JavaServices& services) {
  constexpr char context[] = "android/content/Context";
  constexpr char manager[] = "android/os/PowerManager";
  constexpr char lock[] = "android/os/PowerManager$WakeLock";
  vm.DefineClass(manager, "java/lang/Object");
  vm.DefineClass(lock, "java/lang/Object");
  vm.DefineField(manager, "FULL_WAKE_LOCK", "I", true);
  const auto power_class = vm.FindClass(manager);
  vm.SetField('I', true, power_class, vm.GetFieldID(power_class, "FULL_WAKE_LOCK", "I", true), 0x1a);
  vm.DeleteLocalRef(power_class);
  vm.DefineField(context, "POWER_SERVICE", "Ljava/lang/String;", true);
  vm.DefineField(context, "MODE_PRIVATE", "I", true);  // Android's value is zero.
  const auto cls = vm.FindClass(context);
  const auto power_field = vm.GetFieldID(cls, "POWER_SERVICE", "Ljava/lang/String;", true);
  const auto power = vm.NewString(u"power");
  vm.SetField('L', true, cls, power_field, power);
  vm.DeleteLocalRef(power);
  vm.DeleteLocalRef(cls);
  vm.DefineField(context, "DISPLAY_SERVICE", "Ljava/lang/String;", true);
  vm.DefineField(context, "TELEPHONY_SERVICE", "Ljava/lang/String;", true);
  vm.DefineField(context, "LOCATION_SERVICE", "Ljava/lang/String;", true);
  const auto display_context = vm.FindClass(context);
  const auto display_key = vm.NewString(u"display");
  vm.SetField('L', true, display_context,
              vm.GetFieldID(display_context, "DISPLAY_SERVICE", "Ljava/lang/String;", true), display_key);
  vm.DeleteLocalRef(display_key);
  const auto phone_key = vm.NewString(u"phone");
  vm.SetField('L', true, display_context,
              vm.GetFieldID(display_context, "TELEPHONY_SERVICE", "Ljava/lang/String;", true), phone_key);
  vm.DeleteLocalRef(phone_key);
  const auto location_key = vm.NewString(u"location");
  vm.SetField('L', true, display_context,
              vm.GetFieldID(display_context, "LOCATION_SERVICE", "Ljava/lang/String;", true), location_key);
  vm.DeleteLocalRef(location_key);
  vm.DeleteLocalRef(display_context);
  constexpr char activity_info[] = "android/content/pm/ActivityInfo";
  const auto orientation = vm.FindClass(activity_info);
  for (const auto& [name, value] : {std::pair{"SCREEN_ORIENTATION_REVERSE_LANDSCAPE", 8},
                                   {"SCREEN_ORIENTATION_REVERSE_PORTRAIT", 9}}) {
    vm.DefineField(activity_info, name, "I", true);
    vm.SetField('I', true, orientation, vm.GetFieldID(orientation, name, "I", true), value);
  }
  vm.DeleteLocalRef(orientation);
  vm.DefineClass(kDisplayManager, "java/lang/Object");
  vm.DefineClass("android/telephony/TelephonyManager", "java/lang/Object");
  for (const auto* method : {"getNetworkOperatorName", "getNetworkOperator", "getNetworkCountryIso",
                            "getSimCountryIso", "getSimOperatorName"}) {
    vm.DefineMethod("android/telephony/TelephonyManager", method, "()Ljava/lang/String;", false,
                    [](Java& j, int, const auto&) { return j.NewString(u""); });
  }
  vm.DefineMethod("android/telephony/TelephonyManager", "getPhoneType", "()I", false,
                  [](Java&, int, const auto&) { return 0u; });
  vm.DefineClass(kDisplay, "java/lang/Object");
  vm.DefineClass("android/view/WindowManager", "java/lang/Object");
  vm.DefineClass("android/graphics/Point", "java/lang/Object");
  vm.DefineClass("android/util/DisplayMetrics", "java/lang/Object");
  for (const char* field : {"x", "y"}) vm.DefineField("android/graphics/Point", field, "I", false);
  for (const char* field : {"widthPixels", "heightPixels", "densityDpi"}) {
    vm.DefineField("android/util/DisplayMetrics", field, "I", false);
  }
  for (const char* field : {"density", "scaledDensity", "xdpi", "ydpi"}) {
    vm.DefineField("android/util/DisplayMetrics", field, "F", false);
  }
  const auto info = [display = services.display] {
    return display ? display() : UnityBootstrap::DisplayInfo{};
  };
  for (const char* owner : {"android/graphics/Point", "android/util/DisplayMetrics"}) {
    vm.DefineMethod(owner, "<init>", "()V", false, [](Java&, int, const auto&) { return 0u; });
  }
  for (const auto& [method, member] : {
           std::pair{"getRotation", &UnityBootstrap::DisplayInfo::rotation},
           {"getOrientation", &UnityBootstrap::DisplayInfo::rotation},
           {"getWidth", &UnityBootstrap::DisplayInfo::width},
           {"getHeight", &UnityBootstrap::DisplayInfo::height}}) {
    vm.DefineMethod(kDisplay, method, "()I", false,
                    [info, member](Java&, int, const auto&) { return info().*member; });
  }
  vm.DefineMethod(kDisplay, "getRefreshRate", "()F", false, [info](Java&, int, const auto&) {
    return std::bit_cast<std::uint32_t>(info().refresh_rate);
  });
  for (const auto* method : {"getSize", "getRealSize"}) {
    vm.DefineMethod(kDisplay, method, "(Landroid/graphics/Point;)V", false, [info](Java& j, int, const auto& args) {
      const auto size = info();
      const auto point = j.ObjectOf(args.at(0));
      j.PutField(point, "x", size.width);
      j.PutField(point, "y", size.height);
      return 0u;
    });
  }
  for (const auto* method : {"getMetrics", "getRealMetrics"}) {
    vm.DefineMethod(kDisplay, method, "(Landroid/util/DisplayMetrics;)V", false, [info](Java& j, int, const auto& args) {
      const auto data = info();
      const auto metrics = j.ObjectOf(args.at(0));
      j.PutField(metrics, "widthPixels", data.width);
      j.PutField(metrics, "heightPixels", data.height);
      j.PutField(metrics, "densityDpi", data.density_dpi);
      const auto dpi = static_cast<float>(data.density_dpi);
      for (const auto* name : {"density", "scaledDensity"}) {
        j.PutField(metrics, name, std::bit_cast<std::uint32_t>(dpi / 160.f));
      }
      for (const auto* name : {"xdpi", "ydpi"}) j.PutField(metrics, name, std::bit_cast<std::uint32_t>(dpi));
      return 0u;
    });
  }
  vm.DefineMethod("android/view/WindowManager", "getDefaultDisplay", "()Landroid/view/Display;", false,
                  [](Java& j, int, const auto&) { return j.NewInstance(kDisplay); });
  vm.DefineMethod("android/app/Activity", "getWindowManager", "()Landroid/view/WindowManager;", false,
                  [](Java& j, int, const auto&) { return j.NewInstance("android/view/WindowManager"); });
  vm.DefineMethod(kDisplayManager, "getDisplay", "(I)Landroid/view/Display;", false,
                  [](Java& j, int, const auto& args) { return args.at(0) == 0 ? j.NewInstance(kDisplay) : 0; });
  vm.DefineMethod(kDisplayManager, "getDisplays", "()[Landroid/view/Display;", false,
                  [](Java& j, int, const auto&) {
    const auto cls = j.FindClass(kDisplay);
    const auto primary = j.NewInstance(kDisplay);
    const auto all = j.NewArray('L', 1, cls, primary);
    j.DeleteLocalRef(cls);
    j.DeleteLocalRef(primary);
    return all;
  });
  vm.DefineMethod(kDisplayManager, "getDisplays", "(Ljava/lang/String;)[Landroid/view/Display;", false,
                  [](Java& j, int, const auto&) {
    const auto cls = j.FindClass(kDisplay);
    const auto all = j.NewArray('L', 0, cls, 0);
    j.DeleteLocalRef(cls);
    return all;
  });
  vm.DefineMethod(kDisplay, "getDisplayId", "()I", false, [](Java&, int, const auto&) { return 0u; });
  vm.DefineMethod(context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", false,
                  [](Java& j, int, const auto& args) {
    const auto service = j.StringOf(j.ObjectOf(args.at(0)));
    if (service == u"power") return j.NewInstance("android/os/PowerManager");
    if (service == u"display") return j.NewInstance(kDisplayManager);
    if (service == u"window") return j.NewInstance("android/view/WindowManager");
    if (service == u"phone") return j.NewInstance("android/telephony/TelephonyManager");
    return Java::Ref{0};
  });
  for (const auto* method : {"checkCallingOrSelfPermission", "checkSelfPermission"}) {
    vm.DefineMethod(context, method, "(Ljava/lang/String;)I", false,
                    [](Java&, int, const auto&) { return static_cast<std::uint32_t>(-1); });
  }
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

void InstallPreferences(Java& vm) {
  // The Unity Java profile keeps its preference groups separate within each running title.
  struct Scalar { char type; std::uint64_t bits; };
  using Value = std::variant<Scalar, std::u16string>;
  using Group = std::map<std::u16string, Value>;
  struct State {
    struct Edit {
      bool clear = false;
      std::map<std::u16string, std::optional<Value>> changes;
    };
    std::mutex mutex;
    std::map<std::u16string, Group> groups;
    std::map<int, Edit> editors;
    std::map<int, Group> snapshots;
  };
  const auto state = std::make_shared<State>();
  const auto text = [](Java& j, Java::Ref ref) { return j.StringOf(j.ObjectOf(ref)); };
  const auto group = [](Java& j, int self) { return j.StringOf(static_cast<int>(j.FieldOf(self, "group"))); };
  constexpr char map[] = "java/util/Map";
  constexpr char hash_map[] = "java/util/HashMap";
  vm.DefineClass(map, "java/lang/Object");
  vm.DefineClass(hash_map, map);
  for (const auto& [name, method, sig] : {std::tuple{"Boolean", "booleanValue", "Z"},
       {"Long", "longValue", "J"}, {"Float", "floatValue", "F"}}) {
    const std::string boxed = std::string("java/lang/") + name;
    vm.DefineClass(boxed, "java/lang/Object");
    vm.DefineMethod(boxed, method, std::string("()") + sig, false,
                    [](Java& j, int self, const auto&) { return j.FieldOf(self, "value"); });
  }
  vm.DefineMethod(kPrefs, "getAll", "()Ljava/util/Map;", false, [state, group](Java& j, int self, const auto&) {
    const auto pref_name = group(j, self);
    const auto snapshot = j.NewInstance("java/util/HashMap");
    const auto key = j.ObjectOf(snapshot);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (const auto values = state->groups.find(pref_name); values != state->groups.end()) {
      state->snapshots[key] = values->second;
    }
    return snapshot;
  });
  const auto map_size = [state](int self) {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto snapshot = state->snapshots.find(self);
    return snapshot == state->snapshots.end() ? std::size_t{0} : snapshot->second.size();
  };
  vm.DefineMethod(map, "size", "()I", false, [map_size](Java&, int self, const auto&) { return map_size(self); });
  vm.DefineMethod(map, "isEmpty", "()Z", false, [map_size](Java&, int self, const auto&) { return map_size(self) == 0; });
  const auto lookup = [state, text](Java& j, int self, const auto& args) -> std::optional<Value> {
    const auto key = text(j, args.at(0));
    std::lock_guard<std::mutex> lock(state->mutex);
    if (const auto snap = state->snapshots.find(self); snap != state->snapshots.end()) {
      if (const auto entry = snap->second.find(key); entry != snap->second.end()) return entry->second;
    }
    return std::nullopt;
  };
  vm.DefineMethod(map, "containsKey", "(Ljava/lang/Object;)Z", false,
                  [lookup](Java& j, int self, const auto& args) { return lookup(j, self, args).has_value(); });
  vm.DefineMethod(map, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", false,
                  [lookup](Java& j, int self, const auto& args) -> Java::Ref {
    const auto value = lookup(j, self, args);
    if (!value) return 0;
    if (const auto* string = std::get_if<std::u16string>(&*value)) return j.NewString(*string);
    const auto scalar = std::get<Scalar>(*value);
    const auto type = scalar.type == 'Z' ? "Boolean" : scalar.type == 'J' ? "Long" :
                      scalar.type == 'F' ? "Float" : "Integer";
    const auto boxed = j.NewInstance(std::string("java/lang/") + type);
    j.PutField(j.ObjectOf(boxed), "value", scalar.bits);
    return boxed;
  });
  vm.DefineMethod("android/content/Context", "getSharedPreferences",
                  "(Ljava/lang/String;I)Landroid/content/SharedPreferences;", false,
                  [](Java& j, int, const auto& args) {
    const auto prefs = j.NewInstance(kPrefs);
    PutObject(j, j.ObjectOf(prefs), "group", args.at(0));
    return prefs;
  });
  vm.DefineMethod(kPrefs, "edit", "()Landroid/content/SharedPreferences$Editor;", false,
                  [](Java& j, int self, const auto&) {
    const auto editor = j.NewInstance(kEditor);
    j.PutField(j.ObjectOf(editor), "group", j.FieldOf(self, "group"));
    return editor;
  });
  vm.DefineMethod(kPrefs, "contains", "(Ljava/lang/String;)Z", false,
                  [state, text, group](Java& j, int self, const auto& args) {
    const auto name = group(j, self), key = text(j, args.at(0));
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->groups.find(name);
    return found != state->groups.end() && found->second.contains(key);
  });
  constexpr const char* primitives[][2] = {{"Int", "I"}, {"Boolean", "Z"}, {"Float", "F"}, {"Long", "J"}};
  const auto install_value = [&](const std::string& name, const std::string& sig, bool string) {
    vm.DefineMethod(kPrefs, "get" + name, "(Ljava/lang/String;" + sig + ")" + sig, false,
                    [state, text, group, string, type = sig[0]](Java& j, int self, const auto& args) -> std::uint64_t {
      const auto name = group(j, self), key = text(j, args.at(0));
      std::optional<Value> value;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->groups.find(name);
        if (found != state->groups.end()) {
          if (const auto item = found->second.find(key); item != found->second.end()) value = item->second;
        }
      }
      if (!value) return args.at(1);
      if (string && std::holds_alternative<std::u16string>(*value)) return j.NewString(std::get<std::u16string>(*value));
      if (!string) {
        if (const auto* scalar = std::get_if<Scalar>(&*value); scalar && scalar->type == type) return scalar->bits;
      }
      j.SetPending("java/lang/ClassCastException", "SharedPreferences value has the wrong type");
      return 0;
    });
    vm.DefineMethod(kEditor, "put" + name, "(Ljava/lang/String;" + sig + ")Landroid/content/SharedPreferences$Editor;",
                    false, [state, text, string, type = sig[0]](Java& j, int self, const auto& args) {
      const auto key = text(j, args.at(0));
      const auto raw = args.at(1);
      std::optional<Value> value;
      if (!string) value = Scalar{type, raw};
      else if (raw != 0) value = text(j, raw);
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->editors[self].changes[key] = std::move(value);
      }
      return j.MakeRef(self);
    });
  };
  for (const auto& [name, sig] : primitives) install_value(name, sig, false);
  install_value("String", "Ljava/lang/String;", true);
  vm.DefineMethod(kEditor, "remove", "(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", false,
                  [state, text](Java& j, int self, const auto& args) {
    const auto key = text(j, args.at(0));
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->editors[self].changes[key] = std::nullopt;
    }
    return j.MakeRef(self);
  });
  vm.DefineMethod(kEditor, "clear", "()Landroid/content/SharedPreferences$Editor;", false,
                  [state](Java& j, int self, const auto&) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->editors[self].clear = true;
    }
    return j.MakeRef(self);
  });
  const auto commit = [state, group](Java& j, int self, const auto&) {
    const auto name = group(j, self);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (const auto edit = state->editors.find(self); edit != state->editors.end()) {
      auto& values = state->groups[name];
      if (edit->second.clear) values.clear();
      for (auto& [key, value] : edit->second.changes) {
        if (value) values[key] = std::move(*value);
        else values.erase(key);
      }
      state->editors.erase(edit);
    }
    return 1u;
  };
  vm.DefineMethod(kEditor, "commit", "()Z", false, commit);
  vm.DefineMethod(kEditor, "apply", "()V", false, commit);
}

void InstallServiceConnection(Java& vm) {
  vm.DefineClass(kBridge, "java/lang/Object");
  vm.DefineClass(kRunnable, "java/lang/Object");
  vm.DefineClass("android/content/ServiceConnection", "java/lang/Object");
  const auto proxy = [](Java& j, Java::Ref requested, std::uint64_t handle) -> Java::Ref {
    const auto name = j.ClassName(j.ObjectOf(requested));
    if (name != "android/content/ServiceConnection" && name != kRunnable) return 0;
    const auto result = j.NewInstance(name);
    j.PutField(j.ObjectOf(result), "nativeHandle", handle);
    return result;
  };
  // Android services are unavailable. Unity can still create its ServiceConnection before bindService returns false.
  vm.DefineMethod(kBridge, "newInterfaceProxy", "(JLjava/lang/Class;)Ljava/lang/Object;", true,
                  [proxy](Java& j, int, const auto& args) { return proxy(j, args.at(1), args.at(0)); });
  vm.DefineMethod(kBridge, "newInterfaceProxy", "(J[Ljava/lang/Class;)Ljava/lang/Object;", true,
                  [proxy](Java& j, int, const auto& args) -> Java::Ref {
    if (j.GetArrayLength(args.at(1)) != 1) return 0;
    const auto requested = j.GetObjectArrayElement(args.at(1), 0);
    const auto result = proxy(j, requested, args.at(0));
    j.DeleteLocalRef(requested);
    return result;
  });
  const auto empty = [](Java&, int, const auto&) { return 0u; };
  vm.DefineMethod(kBridge, "disableInterfaceProxy", "(Ljava/lang/Object;)V", true,
                  [](Java& j, int, const auto& args) {
    j.PutField(j.ObjectOf(args.at(0)), "nativeHandle", 0);
    return 0u;
  });
  vm.DefineMethod(kRunnable, "run", "()V", false, [](Java& j, int self, const auto&) {
    const auto handle = j.FieldOf(self, "nativeHandle");
    if (handle == 0) return 0u;
    const auto cls = j.FindClass(kRunnable);
    const auto method = j.GetMethodID(cls, "run", "()V", false);
    const auto reflected = j.ToReflected(cls, method, false, false);
    const auto result = j.CallNative(kBridge, kBridgeInvoke, {handle, cls, reflected, 0});
    j.DeleteLocalRef(result);
    j.DeleteLocalRef(reflected);
    j.DeleteLocalRef(cls);
    return 0u;
  });
  vm.DefineMethod("android/content/Context", "bindService",
                  "(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z", false, empty);
  vm.DefineMethod("android/content/Context", "unbindService", "(Landroid/content/ServiceConnection;)V", false, empty);
}

void InstallMainLooper(Java& vm, const std::shared_ptr<UnityJavaTasks>& tasks) {
  constexpr char looper[] = "android/os/Looper";
  constexpr char handler[] = "android/os/Handler";
  vm.DefineClass(looper, "java/lang/Object");
  vm.DefineClass(handler, "java/lang/Object");
  const auto main_ref = vm.NewInstance(looper);
  const auto main = vm.ObjectOf(main_ref);
  vm.DeleteLocalRef(main_ref);
  for (const char* method : {"getMainLooper", "myLooper"}) {
    vm.DefineMethod(looper, method, "()Landroid/os/Looper;", true,
                    [main](Java& j, int, const auto&) { return j.MakeRef(main); });
  }
  vm.DefineMethod(handler, "<init>", "()V", false, [main](Java& j, int self, const auto&) {
    j.PutField(self, "looper", main);
    return 0u;
  });
  vm.DefineMethod(handler, "<init>", "(Landroid/os/Looper;)V", false,
                  [](Java& j, int self, const auto& args) { PutObject(j, self, "looper", args.at(0)); return 0u; });
  vm.DefineMethod(handler, "getLooper", "()Landroid/os/Looper;", false,
                  [](Java& j, int self, const auto&) { return j.MakeRef(j.FieldOf(self, "looper")); });
  vm.DefineMethod(handler, "post", "(Ljava/lang/Runnable;)Z", false, [tasks](Java& j, int self, const auto& args) {
    return tasks->Post(self, j.ObjectOf(args.at(0)), 0);
  });
  vm.DefineMethod(handler, "postDelayed", "(Ljava/lang/Runnable;J)Z", false,
                  [tasks](Java& j, int self, const auto& args) {
    return tasks->Post(self, j.ObjectOf(args.at(0)), static_cast<std::int64_t>(args.at(1)));
  });
  vm.DefineMethod(handler, "removeCallbacks", "(Ljava/lang/Runnable;)V", false,
                  [tasks](Java& j, int self, const auto& args) {
    const auto runnable = j.ObjectOf(args.at(0));
    std::lock_guard<std::mutex> lock(tasks->mutex);
    std::erase_if(tasks->pending, [&](const auto& task) { return task.handler == self && task.runnable == runnable; });
    return 0u;
  });
  vm.DefineMethod("android/app/Activity", "runOnUiThread", "(Ljava/lang/Runnable;)V", false,
                  [tasks](Java& j, int, const auto& args) {
    tasks->Post(0, j.ObjectOf(args.at(0)), 0);
    return 0u;
  });
}

void InstallMotionEvent(Java& vm) {
  vm.DefineClass("android/view/InputEvent", "java/lang/Object");
  vm.DefineClass(kMotion, "android/view/InputEvent");
  for (const auto& [name, field, sig] : {
           std::tuple{"getAction", "action", "()I"}, {"getPointerCount", "count", "()I"},
           {"getEventTime", "time", "()J"}, {"getDownTime", "down", "()J"}}) {
    vm.DefineMethod(kMotion, name, sig, false,
                    [field](Java& j, int self, const auto&) { return j.FieldOf(self, field); });
  }
  for (const auto& [name, field, sig] : {
           std::tuple{"getPointerId", "id", "(I)I"}, {"getX", "x", "(I)F"}, {"getY", "y", "(I)F"},
           {"getHistoricalX", "x", "(II)F"}, {"getHistoricalY", "y", "(II)F"}}) {
    vm.DefineMethod(kMotion, name, sig, false, [field](Java& j, int self, const auto& args) {
      return j.FieldOf(self, std::string(field) + std::to_string(args.at(0)));
    });
  }
  const auto zero = [](Java&, int, const auto&) { return 0u; };
  for (const char* name : {"getHistorySize", "getMetaState", "getButtonState", "getDeviceId"}) {
    vm.DefineMethod(kMotion, name, "()I", false, zero);
  }
  vm.DefineMethod("android/view/InputEvent", "getSource", "()I", false,
                  [](Java&, int, const auto&) { return 0x1002u; });  // SOURCE_TOUCHSCREEN
  vm.DefineMethod(kMotion, "getToolType", "(I)I", false,
                  [](Java&, int, const auto&) { return 1u; });  // TOOL_TYPE_FINGER
  vm.DefineMethod(kMotion, "getHistoricalEventTime", "(I)J", false, zero);
  vm.DefineMethod(kMotion, "getAxisValue", "(I)F", false, zero);
  vm.DefineMethod(kMotion, "getAxisValue", "(II)F", false, zero);
  struct Pool {
    std::mutex mutex;
    std::set<int> free, borrowed;
  };
  const auto pool = std::make_shared<Pool>();
  vm.DefineMethod(kMotion, "obtain", "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;", true,
                  [pool](Java& j, int, const auto& args) -> Java::Ref {
    const int original = j.ObjectOf(args.at(0));
    if (original == 0) return 0;
    int copy = 0;
    {
      std::lock_guard<std::mutex> lock(pool->mutex);
      if (!pool->free.empty()) {
        copy = *pool->free.begin();
        pool->free.erase(pool->free.begin());
      }
    }
    const auto event = copy != 0 ? j.MakeRef(copy) : j.NewInstance(kMotion);
    copy = j.ObjectOf(event);
    {
      std::lock_guard<std::mutex> lock(pool->mutex);
      pool->borrowed.insert(copy);
    }
    for (const char* field : {"action", "time", "down", "count"}) j.PutField(copy, field, j.FieldOf(original, field));
    const auto count = std::min<std::uint64_t>(j.FieldOf(original, "count"), 32);
    for (std::uint64_t i = 0; i < count; ++i) {
      for (const char* prefix : {"id", "x", "y"}) {
        const auto field = prefix + std::to_string(i);
        j.PutField(copy, field, j.FieldOf(original, field));
      }
    }
    return event;
  });
  vm.DefineMethod(kMotion, "recycle", "()V", false, [pool](Java&, int self, const auto&) {
    std::lock_guard<std::mutex> lock(pool->mutex);
    if (pool->borrowed.erase(self) != 0) pool->free.insert(self);
    return 0u;
  });
}
}

std::shared_ptr<UnityJavaTasks> UnityBootstrap::InstallJava(java::TableJava& java, const std::string& apk, const ApkIdentity& identity,
                                 const JavaServices& services) {
  java.DefineClass("android/view/View", "java/lang/Object");
  java.DefineClass("android/view/ViewGroup", "android/view/View");
  java.DefineClass("android/widget/FrameLayout", "android/view/ViewGroup");
  java.DefineClass(kPlayer, "android/widget/FrameLayout");
  java.DefineClass(kActivity, "android/app/Activity");
  java.DefineClass(kSurface, "java/lang/Object");
  java.DefineClass("com/unity3d/player/ReflectionHelper", "java/lang/Object");
  InstallIntent(java);
  InstallLegacyFmod(java, services);
  InstallApkAssetStreams(java, apk);
  const auto class_name = [](Java& j, int, const auto& args) {
    auto name = std::filesystem::path(j.GetStringChars(args.at(0))).generic_string();
    std::replace(name.begin(), name.end(), '.', '/');
    return j.FindClass(name);
  };
  java.DefineMethod("java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;", true, class_name);
  java.DefineMethod("java/lang/Class", "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;",
                    true, class_name);
  java.DefineMethod("android/content/Context", "getObbDirs", "()[Ljava/io/File;", false,
                    [](Java& j, int self, const auto&) {
    const auto context = j.FindClass("android/content/Context");
    const auto file_class = j.FindClass("java/io/File");
    const auto method = j.GetMethodID(context, "getObbDir", "()Ljava/io/File;", false);
    const auto receiver = j.MakeRef(self);
    const auto path = method ? j.Call('L', false, receiver, 0, method, {}) : 0;
    const auto dirs = j.NewArray('L', path ? 1 : 0, file_class, path);
    j.DeleteLocalRef(receiver);
    j.DeleteLocalRef(path);
    j.DeleteLocalRef(context);
    j.DeleteLocalRef(file_class);
    return dirs;
  });
  InstallPlayerCallbacks(java, services);
  InstallSystemServices(java, services);
  InstallPreferences(java);
  InstallServiceConnection(java);
  auto tasks = std::make_shared<UnityJavaTasks>();
  InstallMainLooper(java, tasks);
  InstallMotionEvent(java);
  java.DefineField(kPlayer, "currentActivity", kActivitySig, true);
  java.DefineMethod(kPlayer, "initializeGoogleAr", "()Z", false,
                    [](Java&, int, const auto&) { return 0u; });
  const auto apk_path = std::filesystem::path(apk).u16string();
  InstallPackageManager(java, identity, apk_path, services);
  for (const char* method : {"getPackageCodePath", "getPackageResourcePath"}) {
    java.DefineMethod("android/content/Context", method, "()Ljava/lang/String;", false,
                      [apk_path](java::TableJava& vm, int, const auto&) { return vm.NewString(apk_path); });
  }
  return tasks;
}

UnityBootstrap::UnityBootstrap(host::Boot& boot, java::TableJava& java, std::shared_ptr<UnityJavaTasks> tasks)
    : boot_(boot), java_(java), tasks_(std::move(tasks)) {}

bool UnityBootstrap::RunJavaTasks(java::TableJava& java, const std::shared_ptr<UnityJavaTasks>& tasks,
                                  std::string* why) {
  if (!tasks) return true;
  for (unsigned i = 0; i < 32; ++i) {
    int runnable;
    {
      std::lock_guard<std::mutex> lock(tasks->mutex);
      if (tasks->pending.empty() || tasks->pending.front().due > UnityJavaTasks::Clock::now()) break;
      runnable = tasks->pending.front().runnable;
      tasks->pending.pop_front();
    }
    java.PushLocalFrame(8);
    const auto object = java.MakeRef(runnable);
    const auto type = java.GetObjectClass(object);
    const auto run = java.GetMethodID(type, "run", "()V", false);
    if (run) java.Call('V', false, object, 0, run, {});
    java.PopLocalFrame(0);
    if (java.ExceptionCheck()) {
      *why = "Unity Android callback: " + java.PendingClass() + ": " + java.PendingMessage();
      return false;
    }
  }
  return true;
}

UnityBootstrap::~UnityBootstrap() {
  if (receiver_ != 0) java_.DeleteGlobalRef(receiver_);
  if (activity_ != 0) java_.DeleteGlobalRef(activity_);
  if (surface_ != 0) java_.DeleteGlobalRef(surface_);
  if (touch_event_ != 0) java_.DeleteGlobalRef(touch_event_);
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
  if (error.empty() && java_.ExceptionCheck()) {
    error = "pending Java exception " + java_.PendingClass();
    const auto message = java_.PendingMessage();
    if (!message.empty()) error += ": " + message;
  }
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
  touch_available_ = Has("nativeInjectEvent", "(Landroid/view/InputEvent;)Z");
  if (touch_available_) touch_event_ = pin(kMotion);
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
  if (!RunJavaTasks(java_, tasks_, why)) return false;
  std::uint64_t keep_running = 0;
  if (!Call("nativeRender", "()Z", {}, why, &keep_running)) return false;
  if (keep_running != 0) return true;
  *why = "UnityPlayer.nativeRender returned false";
  return false;
}

bool UnityBootstrap::Pause(std::string* why) {
  if (!pointers_.empty()) Touch(3, 0, 0, 0);
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

void UnityBootstrap::Touch(int kind, int id, float x, float y) {
  if (!touch_available_) return;
  const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  auto found = std::find_if(pointers_.begin(), pointers_.end(), [id](const Pointer& p) { return p.input_id == id; });
  int action = 3;  // ACTION_CANCEL
  if (kind == 0) {
    if (found != pointers_.end() || pointers_.size() == 32) return;
    std::uint32_t used = 0;
    for (const auto& pointer : pointers_) used |= std::uint32_t{1} << pointer.android_id;
    if (pointers_.empty()) down_time_ = now;
    action = pointers_.empty() ? 0 : 5 | (static_cast<int>(pointers_.size()) << 8);
    pointers_.push_back({id, static_cast<unsigned>(std::countr_zero(~used)), x, y});
  } else if (kind == 1 || kind == 2) {
    if (found == pointers_.end()) return;
    found->x = x;
    found->y = y;
    const int index = static_cast<int>(found - pointers_.begin());
    action = kind == 1 ? 2 : pointers_.size() == 1 ? 1 : 6 | (index << 8);
  } else if (pointers_.empty()) {
    return;
  }
  const int object = java_.ObjectOf(touch_event_);
  java_.PutField(object, "action", static_cast<unsigned>(action));
  java_.PutField(object, "time", now);
  java_.PutField(object, "down", down_time_);
  java_.PutField(object, "count", pointers_.size());
  for (std::size_t i = 0; i < pointers_.size(); ++i) {
    const auto index = std::to_string(i);
    java_.PutField(object, "id" + index, pointers_[i].android_id);
    java_.PutField(object, "x" + index, std::bit_cast<std::uint32_t>(pointers_[i].x));
    java_.PutField(object, "y" + index, std::bit_cast<std::uint32_t>(pointers_[i].y));
  }
  if (kind == 2) pointers_.erase(found);
  if (kind == 3) pointers_.clear();
  std::string error;
  if (!Call("nativeInjectEvent", "(Landroid/view/InputEvent;)Z", {touch_event_}, &error)) {
    boot_.Live("unity: touch disabled for this session: " + error);
    touch_available_ = false;
    java_.ExceptionClear();
  }
}

}  // namespace rb::platform::ios
