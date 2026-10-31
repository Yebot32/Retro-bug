// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The iOS side's Java (S1): `rb::java::TableJava` plus the base Android profile. No guest, no device, no
// fixtures — the rows are data and the calls are the same HostJava operations the forwarding bridge makes.
//
// Every expectation here is a row's contract, not a property of a device: Build answers the environment it
// was given, System/SystemClock are the supplied clock, File carries a path, Activity answers for its package.
// A class the profile does not define must throw NoClassDefFoundError by name — that is the list a title's
// recorded JNI trace adds to.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rb/abi/host_java.h"
#include "rb/java/android_base.h"
#include "rb/java/blockyroads.h"
#include "rb/java/cocos2dx.h"
#include "rb/java/dlgl.h"
#include "rb/java/table_java.h"

namespace {

using rb::java::AndroidEnv;
using rb::java::TableJava;

int checks = 0;
int failures = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::fprintf(stderr, "FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK(...)                                   \
  do {                                               \
    ++checks;                                        \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)

#define CHECK_CTX(cond, detail)                   \
  do {                                            \
    ++checks;                                     \
    if (!(cond)) fail(#cond, __LINE__, (detail)); \
  } while (0)

std::u16string U(const std::string& s) { return std::u16string(s.begin(), s.end()); }
std::string N(const std::u16string& s) { return std::string(s.begin(), s.end()); }

struct Rig {
  TableJava java;
  std::vector<std::string> logs;
  AndroidEnv env;

  /// `files_dir` overrides the profile's writable path for the FileHelper store tests.
  explicit Rig(const char* files_dir = nullptr) {
    env.package_name = "com.test.game";
    env.files_dir = files_dir != nullptr ? files_dir : "/data/com.test.game/files";
    env.cache_dir = "/data/com.test.game/cache";
    env.model = "iPad";
    env.manufacturer = "Apple";
    env.now_ms = [] { return static_cast<std::int64_t>(1'700'000'000'000); };
    env.uptime_ms = [] { return static_cast<std::int64_t>(12'345); };
    env.log = [this](int prio, const std::string& tag, const std::string& text) {
      logs.push_back(std::to_string(prio) + "/" + tag + ": " + text);
    };
    rb::java::InstallAndroidBase(java, env);
    rb::java::InstallCocos2dx(java, env);
    rb::java::InstallDlgl(java, env);
    rb::java::InstallBlockyroads(java, env);
  }

  TableJava::Ref Cls(const char* name) { return java.FindClass(name); }
  TableJava::Id StaticMethod(const char* cls, const char* name, const char* sig) {
    return java.GetMethodID(Cls(cls), name, sig, true);
  }
  std::uint64_t CallStatic(const char* cls, const char* name, const char* sig,
                           const std::vector<std::uint64_t>& args = {}) {
    const TableJava::Ref c = Cls(cls);
    return java.Call(sig[0], true, c, 0, java.GetMethodID(c, name, sig, true), args);
  }
  std::uint64_t CallVirtual(TableJava::Ref obj, const char* name, const char* sig,
                            const std::vector<std::uint64_t>& args = {}) {
    const TableJava::Ref c = java.GetObjectClass(obj);
    return java.Call(sig[0], false, obj, 0, java.GetMethodID(c, name, sig, false), args);
  }
  std::string StringArg(TableJava::Ref s) { return N(java.StringOf(java.ObjectOf(s))); }
  std::uint64_t StaticStringField(const char* cls, const char* field) {
    const TableJava::Ref c = Cls(cls);
    return java.GetField('L', true, c, java.GetFieldID(c, field, "Ljava/lang/String;", true));
  }
};

void build_answers_the_environment() {
  Rig r;
  CHECK_CTX(r.StringArg(static_cast<TableJava::Ref>(r.StaticStringField("android/os/Build", "MODEL"))) == "iPad", "MODEL");
  CHECK_CTX(r.StringArg(static_cast<TableJava::Ref>(r.StaticStringField("android/os/Build", "MANUFACTURER"))) == "Apple",
            "MANUFACTURER");
  const TableJava::Ref version = r.Cls("android/os/Build$VERSION");
  CHECK(version != 0);
  const auto sdk = r.java.GetField('I', true, version, r.java.GetFieldID(version, "SDK_INT", "I", true));
  CHECK_CTX(sdk == 34u, std::to_string(sdk));
  CHECK(r.Cls("android/os/Build") != 0);
}

void clocks_come_from_the_supplied_functions() {
  Rig r;
  const auto ms = r.CallStatic("java/lang/System", "currentTimeMillis", "()J");
  CHECK_CTX(ms == 1'700'000'000'000ull, std::to_string(ms));
  const auto uptime = r.CallStatic("android/os/SystemClock", "uptimeMillis", "()J");
  CHECK_CTX(uptime == 12'345u, std::to_string(uptime));
  const auto realtime = r.CallStatic("android/os/SystemClock", "elapsedRealtime", "()J");
  CHECK_CTX(realtime == 12'345u, std::to_string(realtime));
  const auto nanos = r.CallStatic("java/lang/System", "nanoTime", "()J");
  CHECK_CTX(nanos == 12'345ull * 1'000'000ull, std::to_string(nanos));
}

void strings_behave() {
  Rig r;
  const TableJava::Ref s = r.java.NewString(u"retrobug");
  CHECK_CTX(static_cast<std::int32_t>(r.CallVirtual(s, "length", "()I")) == 8, "length");
  CHECK_CTX(r.CallVirtual(s, "charAt", "(I)C", {2}) == static_cast<std::uint64_t>('t'), "charAt(2)");
  const auto sub = static_cast<TableJava::Ref>(r.CallVirtual(s, "substring", "(II)Ljava/lang/String;", {3, 8}));
  CHECK_CTX(r.StringArg(sub) == "robug", r.StringArg(sub));
  const TableJava::Ref copy = r.java.NewString(u"retrobug");
  CHECK_CTX(r.CallVirtual(s, "equals", "(Ljava/lang/Object;)Z", {copy}) == 1u, "equals");
  const auto hash = static_cast<std::int32_t>(r.CallVirtual(s, "hashCode", "()I"));
  const auto hash2 = static_cast<std::int32_t>(r.CallVirtual(copy, "hashCode", "()I"));
  CHECK_CTX(hash == hash2, "hashCode differs for equal strings");
  const auto value = static_cast<TableJava::Ref>(r.CallStatic("java/lang/String", "valueOf", "(I)Ljava/lang/String;", {42}));
  CHECK_CTX(r.StringArg(value) == "42", r.StringArg(value));
}

void context_and_activity_answer() {
  Rig r;
  const TableJava::Ref activity = r.java.NewInstance("android/app/Activity");
  CHECK(activity != 0);
  const auto pkg = static_cast<TableJava::Ref>(r.CallVirtual(activity, "getPackageName", "()Ljava/lang/String;"));
  CHECK_CTX(r.StringArg(pkg) == "com.test.game", r.StringArg(pkg));
  const auto files = static_cast<TableJava::Ref>(r.CallVirtual(activity, "getFilesDir", "()Ljava/io/File;"));
  CHECK(files != 0);
  const auto path = static_cast<TableJava::Ref>(r.CallVirtual(files, "getPath", "()Ljava/lang/String;"));
  CHECK_CTX(r.StringArg(path) == "/data/com.test.game/files", r.StringArg(path));
  const auto name = static_cast<TableJava::Ref>(r.CallVirtual(files, "getName", "()Ljava/lang/String;"));
  CHECK_CTX(r.StringArg(name) == "files", r.StringArg(name));
  const auto parent = static_cast<TableJava::Ref>(r.CallVirtual(files, "getParent", "()Ljava/lang/String;"));
  CHECK_CTX(r.StringArg(parent) == "/data/com.test.game", r.StringArg(parent));
  const auto app = static_cast<TableJava::Ref>(r.CallVirtual(activity, "getApplicationContext", "()Landroid/content/Context;"));
  CHECK_CTX(app != 0 && r.java.IsSameObject(app, activity), "getApplicationContext is not the activity");
  CHECK(r.CallVirtual(activity, "getAssets", "()Landroid/content/res/AssetManager;") != 0);
}

void file_construction() {
  Rig r;
  const TableJava::Ref file_cls = r.Cls("java/io/File");
  const auto ctor = r.java.GetMethodID(file_cls, "<init>", "(Ljava/lang/String;)V", false);
  const TableJava::Ref str = r.java.NewString(U("/tmp/rb/save.dat"));
  const TableJava::Ref file = r.java.NewObject(file_cls, ctor, {str});
  CHECK(file != 0);
  const auto path = static_cast<TableJava::Ref>(r.CallVirtual(file, "getPath", "()Ljava/lang/String;"));
  CHECK_CTX(r.StringArg(path) == "/tmp/rb/save.dat", r.StringArg(path));
  const auto name = static_cast<TableJava::Ref>(r.CallVirtual(file, "getName", "()Ljava/lang/String;"));
  CHECK_CTX(r.StringArg(name) == "save.dat", r.StringArg(name));
}

void log_reaches_the_sink() {
  Rig r;
  const TableJava::Ref tag = r.java.NewString(u"Game");
  const TableJava::Ref text = r.java.NewString(u"frame 1");
  CHECK(r.CallStatic("android/util/Log", "i", "(Ljava/lang/String;Ljava/lang/String;)I", {tag, text}) == 0);
  CHECK_CTX(r.logs.size() == 1 && r.logs[0] == "4/Game: frame 1", r.logs.empty() ? "no log line" : r.logs[0]);
}

void an_unimplemented_class_is_named() {
  Rig r;
  const TableJava::Ref cls = r.java.FindClass("android/telephony/TelephonyManager");
  CHECK(cls == 0);
  CHECK_CTX(r.java.ExceptionCheck(), "no pending exception");
  CHECK_CTX(r.java.PendingClass() == "java/lang/NoClassDefFoundError", r.java.PendingClass());
  r.java.ExceptionClear();
  CHECK(!r.java.ExceptionCheck());
}

void cocos2dx_helper_answers() {
  Rig r;
  const auto pkg = static_cast<TableJava::Ref>(
      r.CallStatic("org/cocos2dx/lib/Cocos2dxHelper", "getCocos2dxPackageName", "()Ljava/lang/String;"));
  CHECK_CTX(r.StringArg(pkg) == "com.test.game", r.StringArg(pkg));
  const auto path = static_cast<TableJava::Ref>(
      r.CallStatic("org/cocos2dx/lib/Cocos2dxHelper", "getCocos2dxWritablePath", "()Ljava/lang/String;"));
  CHECK_CTX(r.StringArg(path) == "/data/com.test.game/files", r.StringArg(path));
  CHECK(r.CallStatic("org/cocos2dx/lib/Cocos2dxHelper", "getAssetManager", "()Landroid/content/res/AssetManager;") != 0);
  CHECK_CTX(static_cast<std::int32_t>(r.CallStatic("org/cocos2dx/lib/Cocos2dxHelper", "getDPI", "()I")) == 160, "DPI");
  const auto key = r.java.NewString(u"k");
  const auto fallback = r.java.NewString(u"fallback");
  const auto got = static_cast<TableJava::Ref>(r.CallStatic("org/cocos2dx/lib/Cocos2dxHelper", "getStringForKey",
                                                           "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                                           {key, fallback}));
  CHECK_CTX(r.StringArg(got) == "fallback", r.StringArg(got));
}

void dlgl_profile_answers() {
  Rig r;
  const auto key = r.java.NewString(u"best");
  CHECK(r.CallStatic("com/doubleleft/LocalStorage", "loadFloat", "(Ljava/lang/String;)F", {key}) == 0);
  CHECK_CTX(!r.java.ExceptionCheck(), r.java.PendingClass());
  const auto text = static_cast<TableJava::Ref>(
      r.CallStatic("com/doubleleft/LocalStorage", "loadString", "(Ljava/lang/String;)Ljava/lang/String;", {key}));
  CHECK_CTX(r.StringArg(text).empty(), r.StringArg(text));
  CHECK(r.CallStatic("android/media/AudioTrack", "getMinBufferSize", "(III)I", {44100, 2, 2}) == 4096);
  CHECK(r.Cls("com/doubleleft/DLGLRenderer") != 0);

  // The native tracker wrappers call these static String methods during KitKat's startup. A missing
  // method leaves a pending NoSuchMethodError, which is worse than a no-op: the engine never clears it.
  const auto event = r.java.NewString(u"Android_KitKat_app/kitkat_home/home_carregado");
  for (const char* name : {"init", "trackEvent", "trackPageView"}) {
    CHECK_CTX(r.StaticMethod("com/doubleleft/GATracker", name, "(Ljava/lang/String;)V") != 0, name);
  }
  CHECK(r.CallStatic("com/doubleleft/GATracker", "trackEvent", "(Ljava/lang/String;)V", {event}) == 0);
  CHECK(!r.java.ExceptionCheck());
  for (const char* name : {"init", "reportAchievement", "sendScore", "signIn", "signOut"}) {
    CHECK_CTX(r.StaticMethod("com/doubleleft/GooglePlayServices", name, "(Ljava/lang/String;)V") != 0, name);
  }
  for (const char* name : {"openVideo", "resetScoreAlert", "setWallpaper"}) {
    CHECK_CTX(
        r.StaticMethod("com/doubleleft/androidkitkat/AndroidKitKatActivity", name, "(Ljava/lang/String;)V") != 0,
        name);
  }
  CHECK(!r.java.ExceptionCheck());
}

/// Every row `libgame.so` looks up, as the library names them: the class, method and signature strings were
/// read out of its code (the class-name load followed by the method/signature loads at each JNI call site).
struct JavaRow {
  const char* cls;
  const char* name;
  const char* sig;
  bool is_static;
};

bool RowResolves(Rig& r, const JavaRow& row) {
  const TableJava::Ref cls = r.Cls(row.cls);
  return cls != 0 && r.java.GetMethodID(cls, row.name, row.sig, row.is_static) != 0;
}

void blocky_native_surface_resolves() {
  Rig r;
  static const JavaRow rows[] = {
      // The Cocos2d-x 3.x statics the engine's platform layer calls.
      {"org/cocos2dx/lib/Cocos2dxActivity", "getAppVersion", "()Ljava/lang/String;", true},
      {"org/cocos2dx/lib/Cocos2dxActivity", "getDisplayDensity", "()F", true},
      {"org/cocos2dx/lib/Cocos2dxActivity", "getApkExpansionFilename", "(I)Ljava/lang/String;", true},
      {"org/cocos2dx/lib/Cocos2dxActivity", "deleteApkExpansionFile", "(I)V", true},
      {"org/cocos2dx/lib/Cocos2dxActivity", "showLoadingWindow", "(Ljava/lang/String;)V", true},
      {"org/cocos2dx/lib/Cocos2dxActivity", "hideLoadingWindow", "()V", true},
      {"org/cocos2dx/lib/Cocos2dxActivity", "isHpeExperience", "()Z", true},
      {"org/cocos2dx/lib/Cocos2dxActivity", "isChromeOS", "()Z", true},
      {"org/cocos2dx/lib/Cocos2dxRenderer", "setAnimationInterval", "(D)V", true},
      {"org/cocos2dx/lib/Cocos2dxGLSurfaceView", "openIMEKeyboard", "()V", true},
      {"org/cocos2dx/lib/Cocos2dxGLSurfaceView", "closeIMEKeyboard", "()V", true},
      {"org/cocos2dx/lib/Cocos2dxGLSurfaceView", "pauseGLThread", "()V", true},
      {"org/cocos2dx/lib/Cocos2dxGLSurfaceView", "setPauseGLThreadOnActivityPause", "(Z)V", true},
      {"org/cocos2dx/lib/Cocos2dxBitmap", "getFontSizeAccordingHeight", "(I)I", true},
      {"org/cocos2dx/lib/Cocos2dxBitmap", "getStringWithEllipsis", "(Ljava/lang/String;FF)Ljava/lang/String;", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "showDialog", "(Ljava/lang/String;Ljava/lang/String;)V", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "showDialogAndTerminate", "(Ljava/lang/String;Ljava/lang/String;)V", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "showEditTextDialog", "([B[BIIII)V", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "terminateProcess", "()V", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "getCocos2dxPackageName", "()Ljava/lang/String;", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "getCocos2dxWritablePath", "()Ljava/lang/String;", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "getCurrentLanguage", "()Ljava/lang/String;", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "enableAccelerometer", "()V", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "setAccelerometerInterval", "(F)V", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "disableAccelerometer", "()V", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "getDPI", "()I", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "getBoolForKey", "(Ljava/lang/String;Z)Z", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "getIntegerForKey", "(Ljava/lang/String;I)I", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "getFloatForKey", "(Ljava/lang/String;F)F", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "getDoubleForKey", "(Ljava/lang/String;D)D", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "getStringForKey", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
       true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "setBoolForKey", "(Ljava/lang/String;Z)V", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "setIntegerForKey", "(Ljava/lang/String;I)V", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "setFloatForKey", "(Ljava/lang/String;F)V", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "setDoubleForKey", "(Ljava/lang/String;D)V", true},
      {"org/cocos2dx/lib/Cocos2dxHelper", "setStringForKey", "(Ljava/lang/String;Ljava/lang/String;)V", true},

      // Blocky Roads' own classes: the singleton accessor each native wrapper caches first.
      {"com/utils/GameControllerInput", "getInstance", "()Lcom/utils/GameControllerInput;", true},
      {"com/utils/FileHelper", "getInstance", "()Lcom/utils/FileHelper;", true},
      {"com/utils/CCMotion", "getInstance", "()Lcom/utils/CCMotion;", true},
      {"com/utils/HttpGet", "getInstance", "()Lcom/utils/HttpGet;", true},
      {"com/utils/DeviceAndSystem", "getInstance", "()Lcom/utils/DeviceAndSystem;", true},
      {"com/utils/InappPurchase", "getInstance", "()Lcom/utils/InappPurchase;", true},
      {"com/utils/ShareInterface", "getInstance", "()Lcom/utils/ShareInterface;", true},
      {"com/utils/GPlayService", "getInstance", "()Lcom/utils/GPlayService;", true},
      {"com/utils/DropboxInterface", "getInstance", "()Lcom/utils/DropboxInterface;", true},
      {"com/utils/UnityAds", "getInstance", "()Lcom/utils/UnityAds;", true},
      {"com/utils/Admob", "getInstance", "()Lcom/utils/Admob;", true},

      // The instance surface behind those accessors.
      {"com/utils/GameControllerInput", "init", "()V", false},
      {"com/utils/GameControllerInput", "destroy", "()V", false},
      {"com/utils/GameControllerInput", "updateDevices", "()V", false},
      {"com/utils/GameControllerInput", "setContext", "(Landroid/content/Context;)V", false},
      {"com/utils/GameControllerInput", "dispatchGenericMotionEvent", "(Landroid/view/MotionEvent;)V", false},
      {"com/utils/GameControllerInput", "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z", false},
      {"com/utils/GameControllerInput", "getController", "(I)Lcom/utils/GameController;", false},
      {"com/utils/FileHelper", "readPersistentData", "(Ljava/lang/String;)[B", false},
      {"com/utils/FileHelper", "writePersistentData", "(Ljava/lang/String;[B)Z", false},
      {"com/utils/FileHelper", "erasePersistentData", "(Ljava/lang/String;)V", false},
      {"com/utils/FileHelper", "getAssets", "()Landroid/content/res/AssetManager;", false},
      {"com/utils/FileHelper", "getSaveDir", "()Ljava/lang/String;", false},
      {"com/utils/FileHelper", "getCacheDir", "()Ljava/lang/String;", false},
      {"com/utils/FileHelper", "setContext", "(Landroid/content/Context;)V", false},
      {"com/utils/CCMotion", "isAccelerometerAvailable", "()Z", false},
      {"com/utils/CCMotion", "startAccelerometerUpdate", "(F)V", false},
      {"com/utils/CCMotion", "stopAccelerometerUpdate", "()V", false},
      {"com/utils/CCMotion", "getAccelerationData", "()[F", false},
      {"com/utils/HttpGet", "download", "(ILjava/lang/String;[B)V", false},
      {"com/utils/DeviceAndSystem", "OpenURL", "(Ljava/lang/String;)Z", false},
      {"com/utils/DeviceAndSystem", "isActiveNetworkConnection", "()Z", false},
      {"com/utils/DeviceAndSystem", "SetAutoPowerSave", "(Z)V", false},
      {"com/utils/DeviceAndSystem", "GetDeviceID", "()Ljava/lang/String;", false},
      {"com/utils/DeviceAndSystem", "getOS", "()Ljava/lang/String;", false},
      {"com/utils/DeviceAndSystem", "getClipBoardText", "()Ljava/lang/String;", false},
      {"com/utils/DeviceAndSystem", "getLocalIp4Address", "()Ljava/lang/String;", false},
      {"com/utils/DeviceAndSystem", "getLocalIp6Address", "()Ljava/lang/String;", false},
      {"com/utils/DeviceAndSystem", "SetHighPerformanceWifiMode", "(Z)V", false},
      {"com/utils/DeviceAndSystem", "getCountryCode", "()Ljava/lang/String;", false},
      {"com/utils/DeviceAndSystem", "HasVibrator", "()Z", false},
      {"com/utils/DeviceAndSystem", "Vibrate", "()V", false},
      {"com/utils/DeviceAndSystem", "cancelLocalNotifications", "(Ljava/lang/String;)V", false},
      {"com/utils/DeviceAndSystem", "addLocalNotification",
       "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;)V", false},
      {"com/utils/InappPurchase", "isGoogleIAPEnabled", "()Z", false},
      {"com/utils/InappPurchase", "startPurchase", "(Ljava/lang/String;)V", false},
      {"com/utils/InappPurchase", "restorePurchases", "()V", false},
      {"com/utils/InappPurchase", "addProduct", "(Ljava/lang/String;I)V", false},
      {"com/utils/InappPurchase", "requestProductPrices", "()V", false},
      {"com/utils/InappPurchase", "requestProductPurchases", "(Z)V", false},
      {"com/utils/InappPurchase", "getProductPrice", "(Ljava/lang/String;)Ljava/lang/String;", false},
      {"com/utils/InappPurchase", "getProductPurchaseTime", "(Ljava/lang/String;)J", false},
      {"com/utils/InappPurchase", "setNativeInit", "()V", false},
      {"com/utils/ShareInterface", "Send", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", false},
      {"com/utils/ShareInterface", "SendEmail", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)V", false},
      {"com/utils/ShareInterface", "IsAvailable", "()Z", false},
      {"com/utils/GPlayService", "showAchievements", "()V", false},
      {"com/utils/GPlayService", "unlockAchievement", "(Ljava/lang/String;)V", false},
      {"com/utils/GPlayService", "setAchievementCounter", "(Ljava/lang/String;I)V", false},
      {"com/utils/GPlayService", "showSignIn", "()V", false},
      {"com/utils/GPlayService", "showLeaderboards", "()V", false},
      {"com/utils/GPlayService", "setHighScore", "(Ljava/lang/String;I)V", false},
      {"com/utils/GPlayService", "getLeaderboardScores", "(Ljava/lang/String;II)Z", false},
      {"com/utils/GPlayService", "getLoggedInPlayerName", "()Ljava/lang/String;", false},
      {"com/utils/GPlayService", "getLoggedInPlayerID", "()Ljava/lang/String;", false},
      {"com/utils/GPlayService", "setNativeInit", "()V", false},
      {"com/utils/GPlayService", "cloudSaveData", "(Ljava/lang/String;Ljava/lang/String;I[B)V", false},
      {"com/utils/GPlayService", "cloudLoadData", "(Ljava/lang/String;I)V", false},
      {"com/utils/DropboxInterface", "listFiles", "(Ljava/lang/String;II)V", false},
      {"com/utils/DropboxInterface", "uploadFile", "(Ljava/lang/String;[B)V", false},
      {"com/utils/DropboxInterface", "shareFile", "(Ljava/lang/String;)Ljava/lang/String;", false},
      {"com/utils/DropboxInterface", "getUserDisplayName", "()Ljava/lang/String;", false},
      {"com/utils/DropboxInterface", "getUserGivenName", "()Ljava/lang/String;", false},
      {"com/utils/DropboxInterface", "linkAccount", "()I", false},
      {"com/utils/DropboxInterface", "isFileExists", "(Ljava/lang/String;)V", false},
      {"com/utils/DropboxInterface", "getDPURL", "(Ljava/lang/String;)V", false},
      {"com/utils/DropboxInterface", "logout", "()V", false},
      {"com/utils/DropboxInterface", "download", "(Ljava/lang/String;)V", false},
      {"com/utils/DropboxInterface", "initParams", "(Ljava/lang/String;Ljava/lang/String;)V", false},
      {"com/utils/DropboxInterface", "hasLinkedAcc", "()Z", false},
      {"com/utils/DropboxFileItem", "getFileName", "()Ljava/lang/String;", false},
      {"com/utils/DropboxFileItem", "getData", "()[B", false},
      {"com/utils/DropboxFileItem", "getPDate", "()J", false},
      {"com/utils/UnityAds", "showInterstitial", "()Z", false},
      {"com/utils/UnityAds", "showRewardedAd", "()V", false},
      {"com/utils/UnityAds", "hasCachedRewardedAd", "()Z", false},
      {"com/utils/Admob", "SetBannerPosition", "(I)V", false},
      {"com/utils/Admob", "SetBannerVisible", "(Z)V", false},
      {"com/utils/Admob", "ShowInterstitial", "()Z", false},
      {"com/utils/Admob", "CacheInterstitial", "()V", false},
      {"com/utils/Admob", "ShowRewarded", "()V", false},
      {"com/utils/Admob", "HasCachedRewarded", "()Z", false},
      {"com/utils/Admob", "CacheRewarded", "()V", false},
  };
  for (const JavaRow& row : rows) {
    CHECK_CTX(RowResolves(r, row), std::string(row.cls) + "." + row.name + row.sig);
    r.java.ExceptionClear();  // one miss must not mask the rest of the table
  }

  // The leaderboard reader walks the entry's fields by name.
  const TableJava::Ref entry = r.Cls("com/utils/GPLeaderboardEntry");
  CHECK(entry != 0);
  CHECK(r.java.GetFieldID(entry, "mPlayerID", "Ljava/lang/String;", false) != 0);
  CHECK(r.java.GetFieldID(entry, "mPlayerName", "Ljava/lang/String;", false) != 0);
  CHECK(r.java.GetFieldID(entry, "mRank", "I", false) != 0);
  CHECK(r.java.GetFieldID(entry, "mScore", "I", false) != 0);
  CHECK(r.java.GetFieldID(entry, "mFormattedScore", "Ljava/lang/String;", false) != 0);

  // CCControllerDispatcher's actual sequence: class, getInstance, then init on the returned object.
  const auto pad = static_cast<TableJava::Ref>(
      r.CallStatic("com/utils/GameControllerInput", "getInstance", "()Lcom/utils/GameControllerInput;"));
  CHECK_CTX(pad != 0, "GameControllerInput.getInstance");
  CHECK(r.CallVirtual(pad, "init", "()V") == 0);
  CHECK(r.CallVirtual(pad, "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z", {0}) == 0);

  const auto motion =
      static_cast<TableJava::Ref>(r.CallStatic("com/utils/CCMotion", "getInstance", "()Lcom/utils/CCMotion;"));
  CHECK(r.CallVirtual(motion, "isAccelerometerAvailable", "()Z") == 0);
  const auto accel = static_cast<TableJava::Ref>(r.CallVirtual(motion, "getAccelerationData", "()[F"));
  CHECK_CTX(accel != 0 && r.java.GetArrayLength(accel) == 3, "getAccelerationData is three floats");

  const auto system = static_cast<TableJava::Ref>(
      r.CallStatic("com/utils/DeviceAndSystem", "getInstance", "()Lcom/utils/DeviceAndSystem;"));
  CHECK_CTX(r.CallVirtual(system, "isActiveNetworkConnection", "()Z") == 0, "no guest network stack");
  CHECK(r.CallVirtual(system, "OpenURL", "(Ljava/lang/String;)Z", {r.java.NewString(u"https://example.com")}) == 0);
  CHECK_CTX(!r.StringArg(static_cast<TableJava::Ref>(r.CallVirtual(system, "GetDeviceID", "()Ljava/lang/String;")))
                 .empty(),
            "GetDeviceID");

  const auto iap =
      static_cast<TableJava::Ref>(r.CallStatic("com/utils/InappPurchase", "getInstance", "()Lcom/utils/InappPurchase;"));
  CHECK_CTX(r.CallVirtual(iap, "isGoogleIAPEnabled", "()Z") == 0, "billing disabled");

  // The Activity answers: no expansion file, real density, and the loading window call is accepted.
  CHECK(r.CallStatic("org/cocos2dx/lib/Cocos2dxActivity", "getApkExpansionFilename", "(I)Ljava/lang/String;",
                     {0}) == 0);
  CHECK(r.CallStatic("org/cocos2dx/lib/Cocos2dxActivity", "hideLoadingWindow", "()V") == 0);
  const auto density = static_cast<std::uint32_t>(
      r.CallStatic("org/cocos2dx/lib/Cocos2dxActivity", "getDisplayDensity", "()F"));
  float density_value = 0.0f;
  std::memcpy(&density_value, &density, sizeof density_value);
  CHECK_CTX(density_value == 2.0f, std::to_string(density_value));
  CHECK(!r.java.ExceptionCheck());
}

void file_helper_store_round_trips() {
  Rig r("/tmp");
  const std::string name = "rb_table_java_filehelper_test.bin";
  const std::string path = "/tmp/" + name;
  std::remove(path.c_str());
  const auto helper =
      static_cast<TableJava::Ref>(r.CallStatic("com/utils/FileHelper", "getInstance", "()Lcom/utils/FileHelper;"));
  CHECK_CTX(helper != 0, "FileHelper.getInstance");
  const TableJava::Ref jname = r.java.NewString(U(name));
  const std::vector<std::uint8_t> payload{0x52, 0x42, 0x01, 0x00, 0xff};
  const TableJava::Ref array = r.java.NewArray('B', static_cast<std::int32_t>(payload.size()), 0, 0);
  CHECK(r.java.SetArrayRegion('B', array, 0, static_cast<std::int32_t>(payload.size()), payload.data()));
  CHECK_CTX(r.CallVirtual(helper, "writePersistentData", "(Ljava/lang/String;[B)Z", {jname, array}) == 1, "write");
  const auto read = static_cast<TableJava::Ref>(
      r.CallVirtual(helper, "readPersistentData", "(Ljava/lang/String;)[B", {jname}));
  CHECK_CTX(read != 0, "read returns the array");
  CHECK_CTX(read != 0 && r.java.GetArrayLength(read) == 5, std::to_string(r.java.GetArrayLength(read)));
  std::uint8_t got[8] = {};
  CHECK(read != 0 && r.java.GetArrayRegion('B', read, 0, 5, got));
  CHECK_CTX(std::memcmp(got, payload.data(), payload.size()) == 0, "payload bytes");
  CHECK(r.CallVirtual(helper, "erasePersistentData", "(Ljava/lang/String;)V", {jname}) == 0);
  CHECK_CTX(static_cast<TableJava::Ref>(
                 r.CallVirtual(helper, "readPersistentData", "(Ljava/lang/String;)[B", {jname})) == 0,
            "erased save answers null");
  const TableJava::Ref escape = r.java.NewString(u"../escape");
  CHECK_CTX(r.CallVirtual(helper, "writePersistentData", "(Ljava/lang/String;[B)Z", {escape, array}) == 0,
            "a traversal name is refused");
  const auto save = static_cast<TableJava::Ref>(r.CallVirtual(helper, "getSaveDir", "()Ljava/lang/String;"));
  CHECK_CTX(r.StringArg(save) == "/tmp", r.StringArg(save));
  CHECK(!r.java.ExceptionCheck());
  std::remove(path.c_str());
}

void a_missing_row_is_named() {
  Rig r;
  std::vector<std::string> trace;
  r.java.set_trace([&trace](const std::string& line) { trace.push_back(line); });
  const TableJava::Ref helper = r.Cls("org/cocos2dx/lib/Cocos2dxHelper");
  // A row that does not exist must fail; proving the lookup can fail is what makes the table above mean
  // something. The failure names the class and the signature, which is the line the device log prints.
  CHECK(r.java.GetMethodID(helper, "showEditTextDialog", "(Ljava/lang/String;II)V", true) == 0);
  CHECK_CTX(r.java.PendingClass() == "java/lang/NoSuchMethodError", r.java.PendingClass());
  CHECK_CTX(!trace.empty() && trace.back().find("org/cocos2dx/lib/Cocos2dxHelper.showEditTextDialog") !=
                                   std::string::npos,
            trace.empty() ? std::string{"no trace"} : trace.back());
  r.java.ExceptionClear();
  // Both Cocos2d-x generations' signatures are rows: 2.x strings and 3.x byte arrays.
  CHECK(r.StaticMethod("org/cocos2dx/lib/Cocos2dxHelper", "showEditTextDialog", "([B[BIIII)V") != 0);
  CHECK(r.StaticMethod("org/cocos2dx/lib/Cocos2dxHelper", "showEditTextDialog",
                       "(Ljava/lang/String;Ljava/lang/String;IIII)V") != 0);
}

void integers_behave() {
  Rig r;
  const auto parsed = r.CallStatic("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", {r.java.NewString(u"-123")});
  CHECK_CTX(static_cast<std::int32_t>(parsed) == -123, std::to_string(static_cast<std::int32_t>(parsed)));
  const auto bad = r.CallStatic("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", {r.java.NewString(u"x")});
  CHECK(bad == 0);
  CHECK_CTX(r.java.PendingClass() == "java/lang/NumberFormatException", r.java.PendingClass());
  r.java.ExceptionClear();
  const auto boxed = static_cast<TableJava::Ref>(r.CallStatic("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;", {7}));
  CHECK_CTX(static_cast<std::int32_t>(r.CallVirtual(boxed, "intValue", "()I")) == 7, "boxed intValue");
}

}  // namespace

int main() {
  build_answers_the_environment();
  clocks_come_from_the_supplied_functions();
  strings_behave();
  context_and_activity_answer();
  file_construction();
  log_reaches_the_sink();
  an_unimplemented_class_is_named();
  integers_behave();
  cocos2dx_helper_answers();
  dlgl_profile_answers();
  blocky_native_surface_resolves();
  file_helper_store_round_trips();
  a_missing_row_is_named();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
