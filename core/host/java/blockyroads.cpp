// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See rb/java/blockyroads.h.

#include "rb/java/blockyroads.h"

#include <cstdio>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rb::java {

namespace {

using Ref = TableJava::Ref;

std::u16string U(const std::string& s) { return std::u16string(s.begin(), s.end()); }

std::uint64_t NewString(TableJava& f, const std::string& s) {
  return static_cast<std::uint64_t>(f.NewJavaString(U(s)));
}

/// A save name is one printable ASCII path component under the profile's files directory. Separators, dot
/// names and non-ASCII are refused: the store is the app's own, and a guest-chosen `..` must not walk out.
bool SaveNameOk(const std::u16string& name) {
  if (name.empty() || name == u"." || name == u"..") return false;
  for (char16_t c : name) {
    if (c < 0x20 || c == 0x7f || c == u'/' || c == u'\\' || c > 0x7e) return false;
  }
  return true;
}

std::string Narrow(const std::u16string& s) { return std::string(s.begin(), s.end()); }

std::string SavePath(const AndroidEnv& env, const std::string& name) { return env.files_dir + "/" + name; }

/// The singleton shape the game's Java uses on every `com/utils` helper: `getInstance()` answers an object
/// whose methods are the instance surface below. Identity is not part of the native code's use, so a fresh
/// object per call is enough.
void GetInstance(TableJava& j, const std::string& cls) {
  j.DefineMethod(cls, "getInstance", "()L" + cls + ";", true,
                 [cls](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewInstance(cls));
                 });
}

void NoOpVirtual(TableJava& j, const std::string& cls, const std::string& name, const std::string& sig) {
  j.DefineMethod(cls, name, sig, false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
}

void FalseVirtual(TableJava& j, const std::string& cls, const std::string& name) {
  j.DefineMethod(cls, name, "()Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
}

/// An instance string answer, for the "nothing to report" rows (`getUserDisplayName`, `getProductPrice`, ...).
/// Empty rather than null: a title that prints the answer must not crash on a null charset lookup.
void EmptyVirtualString(TableJava& j, const std::string& cls, const std::string& name, const std::string& sig) {
  j.DefineMethod(cls, name, sig, false,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) { return NewString(f, ""); });
}

void InstanceString(TableJava& j, const std::string& cls, const std::string& name, const std::string& sig,
                    std::function<std::string()> value) {
  j.DefineMethod(cls, name, sig, false,
                 [value = std::move(value)](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return NewString(f, value());
                 });
}

void InstallFileHelper(TableJava& j, const AndroidEnv& env) {
  const std::string cls = "com/utils/FileHelper";
  GetInstance(j, cls);
  NoOpVirtual(j, cls, "setContext", "(Landroid/content/Context;)V");
  j.DefineMethod(cls, "getAssets", "()Landroid/content/res/AssetManager;", false,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewInstance("android/content/res/AssetManager"));
                 });
  InstanceString(j, cls, "getSaveDir", "()Ljava/lang/String;", [env] { return env.files_dir; });
  InstanceString(j, cls, "getCacheDir", "()Ljava/lang/String;",
                 [env] { return env.cache_dir.empty() ? env.files_dir : env.cache_dir; });

  // The save store the engine actually reads and writes. The Java class's catch-all returns
  // `toByteArray()` even when the file is missing, so a failed read here answers a zero-length array and
  // never null.
  j.DefineMethod(cls, "readPersistentData", "(Ljava/lang/String;)[B", false,
                 [env](TableJava& f, int, const std::vector<std::uint64_t>& a) -> std::uint64_t {
                   const std::u16string name = f.StringOf(f.ObjectOf(static_cast<Ref>(a[0])));
                   std::vector<std::uint8_t> bytes;
                   if (SaveNameOk(name)) {
                     if (std::FILE* file = std::fopen(SavePath(env, Narrow(name)).c_str(), "rb")) {
                       std::fseek(file, 0, SEEK_END);
                       const long size = std::ftell(file);
                       std::rewind(file);
                       if (size > 0) {
                         bytes.resize(static_cast<std::size_t>(size));
                         if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
                       }
                       std::fclose(file);
                     }
                   }
                   const Ref array = f.NewArray('B', static_cast<std::int32_t>(bytes.size()), 0, 0);
                   if (array != 0 && !bytes.empty()) {
                     f.SetArrayRegion('B', array, 0, static_cast<std::int32_t>(bytes.size()), bytes.data());
                   }
                   return static_cast<std::uint64_t>(array);
                 });
  j.DefineMethod(cls, "writePersistentData", "(Ljava/lang/String;[B)Z", false,
                 [env](TableJava& f, int, const std::vector<std::uint64_t>& a) -> std::uint64_t {
                   const std::u16string name = f.StringOf(f.ObjectOf(static_cast<Ref>(a[0])));
                   const Ref array = static_cast<Ref>(a[1]);
                   if (!SaveNameOk(name) || array == 0) return 0;
                   const std::int32_t size = f.GetArrayLength(array);
                   if (size < 0) return 0;
                   std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
                   if (size > 0 && !f.GetArrayRegion('B', array, 0, size, bytes.data())) return 0;
                   std::FILE* file = std::fopen(SavePath(env, Narrow(name)).c_str(), "wb");
                   if (file == nullptr) return 0;
                   const bool wrote = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
                   return (std::fclose(file) == 0 && wrote) ? 1u : 0u;
                 });
  j.DefineMethod(cls, "erasePersistentData", "(Ljava/lang/String;)V", false,
                 [env](TableJava& f, int, const std::vector<std::uint64_t>& a) -> std::uint64_t {
                   const std::u16string name = f.StringOf(f.ObjectOf(static_cast<Ref>(a[0])));
                   if (SaveNameOk(name)) std::remove(SavePath(env, Narrow(name)).c_str());
                   return 0;
                 });
}

}  // namespace

void InstallBlockyroads(TableJava& j, const AndroidEnv& env) {
  for (const char* cls : {"com/utils/GameControllerInput", "com/utils/FileHelper", "com/utils/CCMotion",
                          "com/utils/HttpGet", "com/utils/DeviceAndSystem", "com/utils/InappPurchase",
                          "com/utils/ShareInterface", "com/utils/GPlayService", "com/utils/GPLeaderboardEntry",
                          "com/utils/DropboxInterface", "com/utils/DropboxFileItem", "com/utils/UnityAds",
                          "com/utils/Admob", "com/utils/GameController"}) {
    j.DefineClass(cls, "java/lang/Object");
  }

  InstallFileHelper(j, env);

  // The Cocos2d-x controller bridge. No controller is attached, so `init` and the dispatch rows are inert;
  // the Java-side dispatchers are never executed here, the GLSurfaceView's events reach the engine directly.
  const std::string pad = "com/utils/GameControllerInput";
  GetInstance(j, pad);
  for (const char* name : {"init", "destroy", "updateDevices"}) NoOpVirtual(j, pad, name, "()V");
  NoOpVirtual(j, pad, "setContext", "(Landroid/content/Context;)V");
  NoOpVirtual(j, pad, "dispatchGenericMotionEvent", "(Landroid/view/MotionEvent;)V");
  j.DefineMethod(pad, "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod(pad, "getController", "(I)Lcom/utils/GameController;", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });

  // Motion: an iPad has no accelerometer the engine samples, so it answers unavailable and zero data.
  const std::string motion = "com/utils/CCMotion";
  GetInstance(j, motion);
  NoOpVirtual(j, motion, "startAccelerometerUpdate", "(F)V");
  NoOpVirtual(j, motion, "stopAccelerometerUpdate", "()V");
  FalseVirtual(j, motion, "isAccelerometerAvailable");
  j.DefineMethod(motion, "getAccelerationData", "()[F", false,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewArray('F', 3, 0, 0));
                 });

  // The downloader: there is no HTTP client behind it yet, so a request is accepted and never completes.
  // Nothing in the boot path waits on it; it backs optional remote content.
  const std::string http = "com/utils/HttpGet";
  GetInstance(j, http);
  NoOpVirtual(j, http, "download", "(ILjava/lang/String;[B)V");

  // OS actions. Opening URLs, vibrating and notifications are outside the sandbox; the answers are the
  // "no backend" ones so game logic takes its fallback path rather than waiting for a callback.
  const std::string system = "com/utils/DeviceAndSystem";
  GetInstance(j, system);
  j.DefineMethod(system, "OpenURL", "(Ljava/lang/String;)Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  FalseVirtual(j, system, "HasVibrator");
  // No guest network stack exists, so "not connected" is the answer that keeps the title on its offline
  // path instead of starting a download whose callback can never arrive.
  FalseVirtual(j, system, "isActiveNetworkConnection");
  for (const char* name : {"SetAutoPowerSave", "SetHighPerformanceWifiMode"}) NoOpVirtual(j, system, name, "(Z)V");
  NoOpVirtual(j, system, "Vibrate", "()V");
  NoOpVirtual(j, system, "cancelLocalNotifications", "(Ljava/lang/String;)V");
  NoOpVirtual(j, system, "addLocalNotification", "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;)V");
  InstanceString(j, system, "GetDeviceID", "()Ljava/lang/String;", [] { return std::string{"retrobug-ios"}; });
  InstanceString(j, system, "getOS", "()Ljava/lang/String;", [env] { return "Android " + env.release; });
  InstanceString(j, system, "getClipBoardText", "()Ljava/lang/String;", [] { return std::string{}; });
  InstanceString(j, system, "getLocalIp4Address", "()Ljava/lang/String;", [] { return std::string{"127.0.0.1"}; });
  InstanceString(j, system, "getLocalIp6Address", "()Ljava/lang/String;", [] { return std::string{"::1"}; });
  InstanceString(j, system, "getCountryCode", "()Ljava/lang/String;", [] { return std::string{"US"}; });

  // Billing and Play Games: signed out and billing disabled are the truthful answers, and they keep the
  // store UI on its fallback path instead of waiting for a purchase callback that cannot arrive.
  const std::string iap = "com/utils/InappPurchase";
  GetInstance(j, iap);
  FalseVirtual(j, iap, "isGoogleIAPEnabled");
  j.DefineMethod(iap, "getProductPurchaseTime", "(Ljava/lang/String;)J", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  EmptyVirtualString(j, iap, "getProductPrice", "(Ljava/lang/String;)Ljava/lang/String;");
  NoOpVirtual(j, iap, "startPurchase", "(Ljava/lang/String;)V");
  NoOpVirtual(j, iap, "addProduct", "(Ljava/lang/String;I)V");
  NoOpVirtual(j, iap, "requestProductPurchases", "(Z)V");
  for (const char* name : {"restorePurchases", "requestProductPrices", "setNativeInit"}) {
    NoOpVirtual(j, iap, name, "()V");
  }

  const std::string share = "com/utils/ShareInterface";
  GetInstance(j, share);
  FalseVirtual(j, share, "IsAvailable");
  NoOpVirtual(j, share, "Send", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
  NoOpVirtual(j, share, "SendEmail", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)V");

  const std::string games = "com/utils/GPlayService";
  GetInstance(j, games);
  j.DefineMethod(games, "getLeaderboardScores", "(Ljava/lang/String;II)Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  for (const char* name : {"showAchievements", "showSignIn", "showLeaderboards", "setNativeInit"}) {
    NoOpVirtual(j, games, name, "()V");
  }
  NoOpVirtual(j, games, "unlockAchievement", "(Ljava/lang/String;)V");
  NoOpVirtual(j, games, "setAchievementCounter", "(Ljava/lang/String;I)V");
  NoOpVirtual(j, games, "setHighScore", "(Ljava/lang/String;I)V");
  NoOpVirtual(j, games, "cloudSaveData", "(Ljava/lang/String;Ljava/lang/String;I[B)V");
  NoOpVirtual(j, games, "cloudLoadData", "(Ljava/lang/String;I)V");
  EmptyVirtualString(j, games, "getLoggedInPlayerName", "()Ljava/lang/String;");
  EmptyVirtualString(j, games, "getLoggedInPlayerID", "()Ljava/lang/String;");

  // The leaderboard reader walks these fields by name; it never asks the native side to build an entry.
  const std::string entry = "com/utils/GPLeaderboardEntry";
  j.DefineField(entry, "mPlayerID", "Ljava/lang/String;", false);
  j.DefineField(entry, "mPlayerName", "Ljava/lang/String;", false);
  j.DefineField(entry, "mRank", "I", false);
  j.DefineField(entry, "mScore", "I", false);
  j.DefineField(entry, "mFormattedScore", "Ljava/lang/String;", false);

  // Dropbox: no linked account, so every query answers empty and every upload is dropped.
  const std::string dropbox = "com/utils/DropboxInterface";
  GetInstance(j, dropbox);
  FalseVirtual(j, dropbox, "hasLinkedAcc");
  j.DefineMethod(dropbox, "linkAccount", "()I", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  EmptyVirtualString(j, dropbox, "shareFile", "(Ljava/lang/String;)Ljava/lang/String;");
  EmptyVirtualString(j, dropbox, "getUserDisplayName", "()Ljava/lang/String;");
  EmptyVirtualString(j, dropbox, "getUserGivenName", "()Ljava/lang/String;");
  NoOpVirtual(j, dropbox, "listFiles", "(Ljava/lang/String;II)V");
  NoOpVirtual(j, dropbox, "uploadFile", "(Ljava/lang/String;[B)V");
  NoOpVirtual(j, dropbox, "isFileExists", "(Ljava/lang/String;)V");
  NoOpVirtual(j, dropbox, "getDPURL", "(Ljava/lang/String;)V");
  NoOpVirtual(j, dropbox, "download", "(Ljava/lang/String;)V");
  NoOpVirtual(j, dropbox, "initParams", "(Ljava/lang/String;Ljava/lang/String;)V");
  NoOpVirtual(j, dropbox, "logout", "()V");

  const std::string item = "com/utils/DropboxFileItem";
  EmptyVirtualString(j, item, "getFileName", "()Ljava/lang/String;");
  j.DefineMethod(item, "getData", "()[B", false,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewArray('B', 0, 0, 0));
                 });
  j.DefineMethod(item, "getPDate", "()J", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });

  // Ads: no cached creative and no fill, which is the answer each provider's "show" already handles.
  const std::string unity = "com/utils/UnityAds";
  GetInstance(j, unity);
  FalseVirtual(j, unity, "showInterstitial");
  FalseVirtual(j, unity, "hasCachedRewardedAd");
  NoOpVirtual(j, unity, "showRewardedAd", "()V");

  const std::string admob = "com/utils/Admob";
  GetInstance(j, admob);
  FalseVirtual(j, admob, "ShowInterstitial");
  FalseVirtual(j, admob, "HasCachedRewarded");
  NoOpVirtual(j, admob, "SetBannerPosition", "(I)V");
  NoOpVirtual(j, admob, "SetBannerVisible", "(Z)V");
  for (const char* name : {"CacheInterstitial", "ShowRewarded", "CacheRewarded"}) {
    NoOpVirtual(j, admob, name, "()V");
  }
}

}  // namespace rb::java
