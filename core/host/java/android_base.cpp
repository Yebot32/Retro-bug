// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The base Android Java profile: see android_base.h.

#include "rb/java/android_base.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rb::java {

namespace {

using Ref = TableJava::Ref;

std::u16string U(const std::string& s) { return std::u16string(s.begin(), s.end()); }

std::string Narrow(const std::u16string& s) { return std::string(s.begin(), s.end()); }

std::string StrArg(TableJava& j, const std::vector<std::uint64_t>& a, std::size_t i) {
  if (i >= a.size() || a[i] == 0) return {};
  return Narrow(j.StringOf(j.ObjectOf(static_cast<Ref>(a[i]))));
}

/// A new java/io/File holding `path`.
Ref NewFile(TableJava& j, const std::string& path) {
  const Ref file = j.NewInstance("java/io/File");
  if (file != 0) j.PutField(j.ObjectOf(file), "path", j.NewJavaString(U(path)));
  return file;
}

std::string FilePath(TableJava& j, int self) {
  const auto ref = static_cast<Ref>(j.FieldOf(self, "path"));
  return ref == 0 ? std::string{} : Narrow(j.StringOf(j.ObjectOf(ref)));
}

std::string ParentOf(const std::string& path) {
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return {};
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

std::string NameOf(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void SetStaticString(TableJava& j, const std::string& cls, const std::string& field, const std::string& value) {
  const Ref c = j.FindClass(cls);
  if (c == 0) return;
  j.SetField('L', true, c, j.GetFieldID(c, field, "Ljava/lang/String;", true), j.NewJavaString(U(value)));
}

void SetStaticInt(TableJava& j, const std::string& cls, const std::string& field, std::int32_t value) {
  const Ref c = j.FindClass(cls);
  if (c == 0) return;
  j.SetField('I', true, c, j.GetFieldID(c, field, "I", true), static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)));
}

}  // namespace

void InstallAndroidBase(TableJava& j, AndroidEnv env) {
  const auto now_ms = env.now_ms ? env.now_ms : [] {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
  };
  const auto uptime_ms = env.uptime_ms ? env.uptime_ms : [] {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
  };
  const auto log = env.log ? env.log : [](int, const std::string&, const std::string&) {};

  // ---- java/lang/Object: the root every other class extends.
  j.DefineClass("java/lang/Object");

  // ---- java/lang/String. TableJava creates instances of this class for every string the guest makes or is
  // handed, so the methods here are what a guest's string calls reach.
  j.DefineClass("java/lang/String", "java/lang/Object");
  j.DefineMethod("java/lang/String", "length", "()I", false, [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(f.StringOf(self).size()));
  });
  j.DefineMethod("java/lang/String", "isEmpty", "()Z", false, [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
    return f.StringOf(self).empty() ? 1u : 0u;
  });
  j.DefineMethod("java/lang/String", "charAt", "(I)C", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>& a) {
                   const auto s = f.StringOf(self);
                   const auto i = static_cast<std::int32_t>(a[0]);
                   if (i < 0 || static_cast<std::size_t>(i) >= s.size()) {
                     f.SetPending("java/lang/StringIndexOutOfBoundsException", std::to_string(i));
                     return std::uint64_t{0};
                   }
                   return static_cast<std::uint64_t>(s[static_cast<std::size_t>(i)]);
                 });
  j.DefineMethod("java/lang/String", "hashCode", "()I", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   std::uint32_t h = 0;
                   for (char16_t c : f.StringOf(self)) h = h * 31u + c;
                   return static_cast<std::uint64_t>(h);
                 });
  j.DefineMethod("java/lang/String", "equals", "(Ljava/lang/Object;)Z", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>& a) {
                   const auto other = static_cast<Ref>(a[0]);
                   return other != 0 && f.StringOf(self) == f.StringOf(f.ObjectOf(other)) ? 1u : 0u;
                 });
  j.DefineMethod("java/lang/String", "toString", "()Ljava/lang/String;", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(f.MakeRef(self)); });
  const auto substring = [](TableJava& f, int self, const std::vector<std::uint64_t>& a, bool two) {
    const auto s = f.StringOf(self);
    const auto begin = static_cast<std::int32_t>(a[0]);
    const auto end = two ? static_cast<std::int32_t>(a[1]) : static_cast<std::int32_t>(s.size());
    if (begin < 0 || end < begin || static_cast<std::size_t>(end) > s.size()) {
      f.SetPending("java/lang/StringIndexOutOfBoundsException", std::to_string(begin) + "," + std::to_string(end));
      return static_cast<std::uint64_t>(0);
    }
    return static_cast<std::uint64_t>(f.NewJavaString(s.substr(static_cast<std::size_t>(begin),
                                                               static_cast<std::size_t>(end - begin))));
  };
  j.DefineMethod("java/lang/String", "substring", "(I)Ljava/lang/String;", false,
                 [substring](TableJava& f, int self, const std::vector<std::uint64_t>& a) { return substring(f, self, a, false); });
  j.DefineMethod("java/lang/String", "substring", "(II)Ljava/lang/String;", false,
                 [substring](TableJava& f, int self, const std::vector<std::uint64_t>& a) { return substring(f, self, a, true); });
  j.DefineMethod("java/lang/String", "indexOf", "(Ljava/lang/String;)I", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>& a) {
                   const auto hay = f.StringOf(self);
                   const auto needle = f.StringOf(f.ObjectOf(static_cast<Ref>(a[0])));
                   const auto at = hay.find(needle);
                   return static_cast<std::uint64_t>(at == std::u16string::npos ? -1 : static_cast<std::int32_t>(at));
                 });
  j.DefineMethod("java/lang/String", "contains", "(Ljava/lang/CharSequence;)Z", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>& a) {
                   const auto hay = f.StringOf(self);
                   const auto needle = f.StringOf(f.ObjectOf(static_cast<Ref>(a[0])));
                   return hay.find(needle) != std::u16string::npos ? 1u : 0u;
                 });
  j.DefineMethod("java/lang/String", "getBytes", "()[B", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   const std::string utf8 = Narrow(f.StringOf(self));
                   const Ref bytes = f.NewArray('B', static_cast<std::int32_t>(utf8.size()), 0, 0);
                   if (bytes != 0 && !utf8.empty()) f.SetArrayRegion('B', bytes, 0, static_cast<std::int32_t>(utf8.size()), utf8.data());
                   return static_cast<std::uint64_t>(bytes);
                 });
  j.DefineMethod("java/lang/String", "getBytes", "(Ljava/lang/String;)[B", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   const std::string utf8 = Narrow(f.StringOf(self));
                   const Ref bytes = f.NewArray('B', static_cast<std::int32_t>(utf8.size()), 0, 0);
                   if (bytes != 0 && !utf8.empty()) f.SetArrayRegion('B', bytes, 0, static_cast<std::int32_t>(utf8.size()), utf8.data());
                   return static_cast<std::uint64_t>(bytes);
                 });
  const auto value_of = [](TableJava& f, int, const std::vector<std::uint64_t>& a) -> std::uint64_t {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(static_cast<std::int64_t>(a[0])));
    return static_cast<std::uint64_t>(f.NewJavaString(U(buf)));
  };
  j.DefineMethod("java/lang/String", "valueOf", "(I)Ljava/lang/String;", true, value_of);
  j.DefineMethod("java/lang/String", "valueOf", "(J)Ljava/lang/String;", true, value_of);
  j.DefineMethod("java/lang/String", "valueOf", "(Z)Ljava/lang/String;", true,
                 [](TableJava& f, int, const std::vector<std::uint64_t>& a) {
                   return static_cast<std::uint64_t>(f.NewJavaString(a[0] != 0 ? u"true" : u"false"));
                 });
  j.DefineMethod("java/lang/String", "valueOf", "(F)Ljava/lang/String;", true,
                 [](TableJava& f, int, const std::vector<std::uint64_t>& a) {
                   float v = 0;
                   const auto bits = static_cast<std::uint32_t>(a[0]);
                   std::memcpy(&v, &bits, 4);
                   char buf[40];
                   std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
                   return static_cast<std::uint64_t>(f.NewJavaString(U(buf)));
                 });

  // ---- java/lang/Throwable and the throwables the profiles and guests name. A native that checks
  // FindClass("java/lang/Error") after a failed Java call needs these to exist; a missing class made the
  // throw itself fail. The hierarchy mirrors Android's, so IsAssignableFrom answers a catch-shaped query.
  const auto define_throwable = [&j](const std::string& cls, const std::string& super) {
    j.DefineClass(cls, super);
    j.DefineField(cls, "message", "Ljava/lang/String;", false);
    j.DefineMethod(cls, "<init>", "()V", false,
                   [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
    j.DefineMethod(cls, "<init>", "(Ljava/lang/String;)V", false,
                   [](TableJava& f, int self, const std::vector<std::uint64_t>& a) {
                     f.PutField(self, "message", a.empty() ? 0u : a[0]);
                     return std::uint64_t{0};
                   });
    j.DefineMethod(cls, "getMessage", "()Ljava/lang/String;", false,
                   [](TableJava& f, int self, const std::vector<std::uint64_t>&) -> std::uint64_t {
                     const auto message = static_cast<Ref>(f.FieldOf(self, "message"));
                     if (message != 0) return message;
                     const auto text = f.StringOf(self);  // SetPending keeps its message on the object
                     return text.empty() ? 0u : static_cast<std::uint64_t>(f.NewJavaString(text));
                   });
    j.DefineMethod(cls, "getLocalizedMessage", "()Ljava/lang/String;", false,
                   [](TableJava& f, int self, const std::vector<std::uint64_t>&) -> std::uint64_t {
                     const auto message = static_cast<Ref>(f.FieldOf(self, "message"));
                     if (message != 0) return message;
                     const auto text = f.StringOf(self);
                     return text.empty() ? 0u : static_cast<std::uint64_t>(f.NewJavaString(text));
                   });
    j.DefineMethod(cls, "toString", "()Ljava/lang/String;", false,
                   [cls](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                     std::u16string out(cls.begin(), cls.end());
                     const auto message = static_cast<Ref>(f.FieldOf(self, "message"));
                     const std::u16string text = message != 0 ? f.StringOf(f.ObjectOf(message)) : f.StringOf(self);
                     if (!text.empty()) {
                       out += u": ";
                       out += text;
                     }
                     return static_cast<std::uint64_t>(f.NewJavaString(out));
                   });
    j.DefineMethod(cls, "printStackTrace", "()V", false,
                   [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
    j.DefineMethod(cls, "getCause", "()Ljava/lang/Throwable;", false,
                   [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
    j.DefineMethod(cls, "fillInStackTrace", "()Ljava/lang/Throwable;", false,
                   [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                     return static_cast<std::uint64_t>(f.MakeRef(self));
                   });
  };
  define_throwable("java/lang/Throwable", "java/lang/Object");
  define_throwable("java/lang/Exception", "java/lang/Throwable");
  define_throwable("java/lang/Error", "java/lang/Throwable");
  define_throwable("java/lang/RuntimeException", "java/lang/Exception");
  define_throwable("java/lang/LinkageError", "java/lang/Error");
  define_throwable("java/lang/NoClassDefFoundError", "java/lang/LinkageError");
  define_throwable("java/lang/ClassNotFoundException", "java/lang/Exception");
  define_throwable("java/lang/OutOfMemoryError", "java/lang/Error");
  define_throwable("java/lang/StackOverflowError", "java/lang/Error");
  define_throwable("java/lang/IllegalStateException", "java/lang/RuntimeException");
  define_throwable("java/lang/IllegalArgumentException", "java/lang/RuntimeException");
  define_throwable("java/lang/NullPointerException", "java/lang/RuntimeException");
  define_throwable("java/lang/ArithmeticException", "java/lang/RuntimeException");
  define_throwable("java/lang/UnsupportedOperationException", "java/lang/RuntimeException");
  define_throwable("java/lang/ClassCastException", "java/lang/RuntimeException");
  define_throwable("java/lang/IndexOutOfBoundsException", "java/lang/RuntimeException");
  define_throwable("java/lang/StringIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException");
  define_throwable("java/lang/ArrayIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException");
  define_throwable("java/lang/NumberFormatException", "java/lang/IllegalArgumentException");
  define_throwable("java/lang/InterruptedException", "java/lang/Exception");

  // ---- java/lang/Integer: parse/format, and the boxed form a game stores in collections.
  j.DefineClass("java/lang/Integer", "java/lang/Object");
  j.DefineField("java/lang/Integer", "value", "I", false);
  j.DefineField("java/lang/Integer", "MAX_VALUE", "I", true);
  j.DefineField("java/lang/Integer", "MIN_VALUE", "I", true);
  SetStaticInt(j, "java/lang/Integer", "MAX_VALUE", 0x7fff'ffff);
  SetStaticInt(j, "java/lang/Integer", "MIN_VALUE", -0x7fff'ffff - 1);
  j.DefineMethod("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", true,
                 [](TableJava& f, int, const std::vector<std::uint64_t>& a) -> std::uint64_t {
                   const std::string text = StrArg(f, a, 0);
                   char* end = nullptr;
                   const long v = std::strtol(text.c_str(), &end, 10);
                   if (text.empty() || end == nullptr || *end != '\0') {
                     f.SetPending("java/lang/NumberFormatException", text);
                     return std::uint64_t{0};
                   }
                   return static_cast<std::uint64_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                 });
  j.DefineMethod("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;", true,
                 [](TableJava& f, int, const std::vector<std::uint64_t>& a) -> std::uint64_t {
                   const Ref boxed = f.NewInstance("java/lang/Integer");
                   if (boxed != 0) f.PutField(f.ObjectOf(boxed), "value", a[0]);
                   return static_cast<std::uint64_t>(boxed);
                 });
  j.DefineMethod("java/lang/Integer", "intValue", "()I", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(f.FieldOf(self, "value"))));
                 });
  j.DefineMethod("java/lang/Integer", "toString", "()Ljava/lang/String;", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   char buf[32];
                   std::snprintf(buf, sizeof buf, "%d", static_cast<std::int32_t>(f.FieldOf(self, "value")));
                   return static_cast<std::uint64_t>(f.NewJavaString(U(buf)));
                 });

  // ---- java/lang/System: the clock and properties a native title uses to time its frames.
  j.DefineClass("java/lang/System", "java/lang/Object");
  j.DefineMethod("java/lang/System", "currentTimeMillis", "()J", true,
                 [now_ms](TableJava&, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(now_ms());
                 });
  j.DefineMethod("java/lang/System", "nanoTime", "()J", true,
                 [uptime_ms](TableJava&, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(uptime_ms() * 1'000'000);
                 });
  j.DefineMethod("java/lang/System", "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", true,
                 [env](TableJava& f, int, const std::vector<std::uint64_t>& a) -> std::uint64_t {
                   const std::string key = StrArg(f, a, 0);
                   std::string value;
                   if (key == "os.name") value = "Linux";
                   else if (key == "os.arch") value = "armv7l";
                   else if (key == "java.vendor") value = "RetroBug";
                   else if (key == "java.version") value = "0";
                   else if (key == "user.dir") value = env.files_dir;
                   else if (key == "line.separator") value = "\n";
                   if (value.empty() && key == "file.separator") value = "/";
                   return value.empty() ? std::uint64_t{0} : static_cast<std::uint64_t>(f.NewJavaString(U(value)));
                 });
  j.DefineMethod("java/lang/System", "getenv", "(Ljava/lang/String;)Ljava/lang/String;", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });

  // ---- android/os/SystemClock.
  j.DefineClass("android/os/SystemClock", "java/lang/Object");
  j.DefineMethod("android/os/SystemClock", "uptimeMillis", "()J", true,
                 [uptime_ms](TableJava&, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(uptime_ms());
                 });
  j.DefineMethod("android/os/SystemClock", "elapsedRealtime", "()J", true,
                 [uptime_ms](TableJava&, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(uptime_ms());
                 });
  j.DefineMethod("android/os/SystemClock", "currentThreadTimeMillis", "()J", true,
                 [uptime_ms](TableJava&, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(uptime_ms());
                 });
  j.DefineMethod("android/os/SystemClock", "sleep", "(J)V", true, [](TableJava&, int, const std::vector<std::uint64_t>&) {
    return static_cast<std::uint64_t>(0);
  });
  j.DefineMethod("android/os/SystemClock", "setCurrentTimeMillis", "(J)Z", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(1); });

  // ---- android/os/Build and Build$VERSION: what a game reads to pick a quality tier.
  j.DefineClass("android/os/Build", "java/lang/Object");
  for (const char* field : {"MODEL", "MANUFACTURER", "BRAND", "DEVICE", "PRODUCT", "HARDWARE", "FINGERPRINT", "ID", "DISPLAY"}) {
    j.DefineField("android/os/Build", field, "Ljava/lang/String;", true);
  }
  SetStaticString(j, "android/os/Build", "MODEL", env.model);
  SetStaticString(j, "android/os/Build", "MANUFACTURER", env.manufacturer);
  SetStaticString(j, "android/os/Build", "BRAND", env.brand);
  SetStaticString(j, "android/os/Build", "DEVICE", env.device);
  SetStaticString(j, "android/os/Build", "PRODUCT", env.device);
  SetStaticString(j, "android/os/Build", "HARDWARE", env.device);
  SetStaticString(j, "android/os/Build", "FINGERPRINT", env.fingerprint);
  SetStaticString(j, "android/os/Build", "ID", "retrobug");
  SetStaticString(j, "android/os/Build", "DISPLAY", "retrobug");
  j.DefineClass("android/os/Build$VERSION", "java/lang/Object");
  j.DefineField("android/os/Build$VERSION", "SDK_INT", "I", true);
  j.DefineField("android/os/Build$VERSION", "RELEASE", "Ljava/lang/String;", true);
  SetStaticInt(j, "android/os/Build$VERSION", "SDK_INT", env.sdk_int);
  SetStaticString(j, "android/os/Build$VERSION", "RELEASE", env.release);

  // ---- android/util/Log: the guest's log calls become the app's log, with the same priority names.
  j.DefineClass("android/util/Log", "java/lang/Object");
  const auto lev = [log](int prio) {
    return [log, prio](TableJava& f, int, const std::vector<std::uint64_t>& a) -> std::uint64_t {
      log(prio, StrArg(f, a, 0), StrArg(f, a, 1));
      return 0;
    };
  };
  j.DefineMethod("android/util/Log", "v", "(Ljava/lang/String;Ljava/lang/String;)I", true, lev(2));
  j.DefineMethod("android/util/Log", "d", "(Ljava/lang/String;Ljava/lang/String;)I", true, lev(3));
  j.DefineMethod("android/util/Log", "i", "(Ljava/lang/String;Ljava/lang/String;)I", true, lev(4));
  j.DefineMethod("android/util/Log", "w", "(Ljava/lang/String;Ljava/lang/String;)I", true, lev(5));
  j.DefineMethod("android/util/Log", "e", "(Ljava/lang/String;Ljava/lang/String;)I", true, lev(6));
  j.DefineMethod("android/util/Log", "v", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I", true, lev(2));
  j.DefineMethod("android/util/Log", "d", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I", true, lev(3));
  j.DefineMethod("android/util/Log", "i", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I", true, lev(4));
  j.DefineMethod("android/util/Log", "w", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I", true, lev(5));
  j.DefineMethod("android/util/Log", "e", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I", true, lev(6));
  j.DefineMethod("android/util/Log", "isLoggable", "(Ljava/lang/String;I)Z", true,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(1); });
  j.DefineMethod("android/util/Log", "getStackTraceString", "(Ljava/lang/Throwable;)Ljava/lang/String;", true,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewJavaString(u""));
                 });

  // ---- java/util/Locale.
  j.DefineClass("java/util/Locale", "java/lang/Object");
  j.DefineMethod("java/util/Locale", "getDefault", "()Ljava/util/Locale;", true,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewInstance("java/util/Locale"));
                 });
  j.DefineMethod("java/util/Locale", "getLanguage", "()Ljava/lang/String;", false,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewJavaString(u"en"));
                 });
  j.DefineMethod("java/util/Locale", "getCountry", "()Ljava/lang/String;", false,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewJavaString(u"US"));
                 });
  j.DefineMethod("java/util/Locale", "toString", "()Ljava/lang/String;", false,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewJavaString(u"en_US"));
                 });

  // ---- java/io/File: the paths a game asks for. Existence and writes are the shell's filesystem later.
  j.DefineClass("java/io/File", "java/lang/Object");
  j.DefineField("java/io/File", "path", "Ljava/lang/String;", false);
  j.DefineMethod("java/io/File", "<init>", "(Ljava/lang/String;)V", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>& a) {
                   f.PutField(self, "path", a[0]);
                   return static_cast<std::uint64_t>(0);
                 });
  j.DefineMethod("java/io/File", "getPath", "()Ljava/lang/String;", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.MakeRef(f.ObjectOf(static_cast<Ref>(f.FieldOf(self, "path")))));
                 });
  j.DefineMethod("java/io/File", "getAbsolutePath", "()Ljava/lang/String;", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.MakeRef(f.ObjectOf(static_cast<Ref>(f.FieldOf(self, "path")))));
                 });
  j.DefineMethod("java/io/File", "toString", "()Ljava/lang/String;", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.MakeRef(f.ObjectOf(static_cast<Ref>(f.FieldOf(self, "path")))));
                 });
  j.DefineMethod("java/io/File", "getName", "()Ljava/lang/String;", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewJavaString(U(NameOf(FilePath(f, self)))));
                 });
  j.DefineMethod("java/io/File", "getParent", "()Ljava/lang/String;", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) -> std::uint64_t {
                   const std::string parent = ParentOf(FilePath(f, self));
                   return parent.empty() ? std::uint64_t{0} : static_cast<std::uint64_t>(f.NewJavaString(U(parent)));
                 });
  j.DefineMethod("java/io/File", "exists", "()Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod("java/io/File", "isDirectory", "()Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod("java/io/File", "isFile", "()Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod("java/io/File", "mkdirs", "()Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(1); });
  j.DefineMethod("java/io/File", "mkdir", "()Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(1); });
  j.DefineMethod("java/io/File", "delete", "()Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(1); });
  j.DefineMethod("java/io/File", "length", "()J", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod("java/io/File", "canRead", "()Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(1); });
  j.DefineMethod("java/io/File", "canWrite", "()Z", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(1); });
  j.DefineMethod("java/io/File", "lastModified", "()J", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });

  // ---- Context / Activity: the object the shell makes for the game, and what it asks about itself.
  j.DefineClass("android/content/Context", "java/lang/Object");
  j.DefineClass("android/content/ContextWrapper", "android/content/Context");
  j.DefineClass("android/app/Activity", "android/content/ContextWrapper");
  j.DefineClass("android/content/res/AssetManager", "java/lang/Object");
  j.DefineMethod("android/content/Context", "getPackageName", "()Ljava/lang/String;", false,
                 [env](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewJavaString(U(env.package_name)));
                 });
  j.DefineMethod("android/content/Context", "getFilesDir", "()Ljava/io/File;", false,
                 [env](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(NewFile(f, env.files_dir));
                 });
  j.DefineMethod("android/content/Context", "getCacheDir", "()Ljava/io/File;", false,
                 [env](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(NewFile(f, env.cache_dir));
                 });
  j.DefineMethod("android/content/Context", "getAssets", "()Landroid/content/res/AssetManager;", false,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewInstance("android/content/res/AssetManager"));
                 });
  j.DefineMethod("android/content/Context", "getApplicationContext", "()Landroid/content/Context;", false,
                 [](TableJava& f, int self, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.MakeRef(self));
                 });
  j.DefineMethod("android/content/Context", "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod("android/content/Context", "getString", "(I)Ljava/lang/String;", false,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewJavaString(u""));
                 });
  j.DefineClass("android/content/pm/ApplicationInfo", "java/lang/Object");
  j.DefineField("android/content/pm/ApplicationInfo", "packageName", "Ljava/lang/String;", false);
  j.DefineField("android/content/pm/ApplicationInfo", "sourceDir", "Ljava/lang/String;", false);
  j.DefineMethod("android/content/Context", "getString", "(I[Ljava/lang/Object;)Ljava/lang/String;", false,
                 [](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   return static_cast<std::uint64_t>(f.NewJavaString(u""));
                 });
  j.DefineMethod("android/content/Context", "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;", false,
                 [env](TableJava& f, int, const std::vector<std::uint64_t>&) {
                   const Ref info = f.NewInstance("android/content/pm/ApplicationInfo");
                   if (info != 0) {
                     f.PutField(f.ObjectOf(info), "packageName", f.NewJavaString(U(env.package_name)));
                     f.PutField(f.ObjectOf(info), "sourceDir", f.NewJavaString(U(env.apk_path)));
                   }
                   return static_cast<std::uint64_t>(info);
                 });
  for (const char* name : {"onCreate", "onResume", "onPause", "onDestroy", "onStart", "onStop"}) {
    // Activity lifecycle hooks, called by the shell through the same tables a game would use.
    j.DefineMethod("android/app/Activity", name, "()V", false,
                   [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  }
  j.DefineMethod("android/app/Activity", "showDialog", "(Ljava/lang/String;Ljava/lang/String;)V", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod("android/app/Activity", "terminateProcess", "()V", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod("android/app/Activity", "runOnUiThread", "(Ljava/lang/Runnable;)V", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod("android/app/Activity", "getWindowManager", "()Landroid/view/WindowManager;", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod("android/app/Activity", "setRequestedOrientation", "(I)V", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
  j.DefineMethod("android/app/Activity", "setVolumeControlStream", "(I)V", false,
                 [](TableJava&, int, const std::vector<std::uint64_t>&) { return static_cast<std::uint64_t>(0); });
}

}  // namespace rb::java
