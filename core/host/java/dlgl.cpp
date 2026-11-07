// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See rb/java/dlgl.h.

#include "rb/java/dlgl.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
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

/// KitKat's save store (`com/doubleleft/LocalStorage`). It was in-memory until this build, so every
/// score and every unlocked minigame vanished when the app closed. The file is the game's own profile
/// directory and is rewritten as a whole on each save — a handful of keys, written once per level.
class LocalStore {
 public:
  explicit LocalStore(std::string path) : path_(std::move(path)) { Load(); }

  bool GetFloat(const std::string& key, float* out) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = floats_.find(key);
    if (it == floats_.end()) return false;
    *out = it->second;
    return true;
  }
  std::string GetString(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = strings_.find(key);
    return it == strings_.end() ? std::string{} : it->second;
  }
  void PutFloat(const std::string& key, float value) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      floats_[key] = value;
    }
    Save();
  }
  void PutString(const std::string& key, const std::string& value) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      strings_[key] = value;
    }
    Save();
  }

 private:
  static std::string Escape(const std::string& s) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : s) {
      if (c <= 0x20 || c == '%' || c == 0x7f) {
        out += '%';
        out += kHex[c >> 4];
        out += kHex[c & 0xf];
      } else {
        out += static_cast<char>(c);
      }
    }
    return out;
  }
  static std::string Unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
      if (s[i] != '%' || i + 2 >= s.size()) {
        out += s[i];
        continue;
      }
      const auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
      };
      const int hi = nib(s[i + 1]);
      const int lo = nib(s[i + 2]);
      if (hi < 0 || lo < 0) {
        out += s[i];
        continue;
      }
      out += static_cast<char>((hi << 4) | lo);
      i += 2;
    }
    return out;
  }

  void Load() {
    if (path_.empty()) return;
    std::FILE* file = std::fopen(path_.c_str(), "rb");
    if (file == nullptr) return;
    std::string line;
    char chunk[512];
    std::size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof chunk, file)) > 0) line.append(chunk, n);
    std::fclose(file);
    std::size_t pos = 0;
    while (pos < line.size()) {
      const std::size_t end = line.find('\n', pos);
      const std::string row = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      pos = end == std::string::npos ? line.size() : end + 1;
      if (row.size() < 4) continue;
      const std::size_t first = row.find(' ', 2);
      if (first == std::string::npos) continue;
      const std::string key = Unescape(row.substr(2, first - 2));
      const std::string value = row.substr(first + 1);
      if (row[0] == 'F') {
        std::uint32_t bits = 0;
        if (std::sscanf(value.c_str(), "%x", &bits) == 1) {
          float f = 0;
          std::memcpy(&f, &bits, 4);
          floats_[key] = f;
        }
      } else if (row[0] == 'S') {
        strings_[key] = Unescape(value);
      }
    }
  }

  void Save() const {
    if (path_.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    const std::string tmp = path_ + ".tmp";
    std::FILE* file = std::fopen(tmp.c_str(), "wb");
    if (file == nullptr) return;
    for (const auto& [key, f] : floats_) {
      std::uint32_t bits = 0;
      std::memcpy(&bits, &f, 4);
      std::fprintf(file, "F %s %08x\n", Escape(key).c_str(), bits);
    }
    for (const auto& [key, s] : strings_) {
      std::fprintf(file, "S %s %s\n", Escape(key).c_str(), Escape(s).c_str());
    }
    std::fclose(file);
    std::rename(tmp.c_str(), path_.c_str());
  }

  std::string path_;
  mutable std::mutex mu_;
  std::map<std::string, float> floats_;
  std::map<std::string, std::string> strings_;
};

std::string NarrowString(TableJava& f, const std::vector<std::uint64_t>& a, std::size_t i) {
  const std::u16string s = f.StringOf(f.ObjectOf(static_cast<TableJava::Ref>(a[i])));
  return std::string(s.begin(), s.end());
}

}  // namespace

void InstallDlgl(TableJava& j, const AndroidEnv& env) {
  // The classes its JNI_OnLoad names before RegisterNatives: a missing class turns registration into a
  // NoClassDefFoundError and the bootstrap reports it by name.
  for (const char* cls : {"com/doubleleft/DLGLSurface", "com/doubleleft/DLGLRenderer",
                          "com/doubleleft/AdmobPlugin", "com/doubleleft/GATracker",
                          "com/doubleleft/GooglePlayServices", "com/doubleleft/androidkitkat/AndroidKitKatActivity",
                          "com/doubleleft/LocalStorage", "android/media/AudioTrack",
                          "android/content/SharedPreferences"}) {
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
  // The engine's own quit path. Build 14's log shows it reached `callJavaMethod` for this method after a
  // failed frame; the lookup missed, and the call that followed carried method ID 0 into the bridge.
  NoOp(j, activity, "exitApp", "(Ljava/lang/String;)V");

  // LocalStorage: the engine's saves, on disk (see LocalStore). A float crosses the bridge as its bit
  // pattern, the same encoding every float row here uses.
  const std::string storage = "com/doubleleft/LocalStorage";
  const auto store = std::make_shared<LocalStore>(
      env.files_dir.empty() ? std::string{} : env.files_dir + "/localstorage.dat");
  j.DefineMethod(storage, "loadFloat", "(Ljava/lang/String;)F", true,
                 [store](TableJava& f, int, const std::vector<std::uint64_t>& a) {
                   const std::string key = NarrowString(f, a, 0);
                   float value = 0;
                   if (!store->GetFloat(key, &value)) value = 0;
                   std::uint32_t bits = 0;
                   std::memcpy(&bits, &value, 4);
                   return static_cast<std::uint64_t>(bits);
                 });
  j.DefineMethod(storage, "loadString", "(Ljava/lang/String;)Ljava/lang/String;", true,
                 [store](TableJava& f, int, const std::vector<std::uint64_t>& a) {
                   const std::string value = store->GetString(NarrowString(f, a, 0));
                   return static_cast<std::uint64_t>(
                       f.NewJavaString(std::u16string(value.begin(), value.end())));
                 });
  j.DefineMethod(storage, "saveFloat", "(Ljava/lang/String;F)V", true,
                 [store](TableJava& f, int, const std::vector<std::uint64_t>& a) {
                   const auto bits = static_cast<std::uint32_t>(a[1]);
                   float value = 0;
                   std::memcpy(&value, &bits, 4);
                   store->PutFloat(NarrowString(f, a, 0), value);
                   return static_cast<std::uint64_t>(0);
                 });
  j.DefineMethod(storage, "saveString", "(Ljava/lang/String;Ljava/lang/String;)V", true,
                 [store](TableJava& f, int, const std::vector<std::uint64_t>& a) {
                   store->PutString(NarrowString(f, a, 0), NarrowString(f, a, 1));
                   return static_cast<std::uint64_t>(0);
                 });
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
