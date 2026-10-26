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
#include "rb/java/cocos2dx.h"
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

  Rig() {
    env.package_name = "com.test.game";
    env.files_dir = "/data/com.test.game/files";
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
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
