// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The KitKat DLGE bootstrap against fake JNI natives. This is the ABI the title's Java layer would
// invoke: (JNIEnv*, jclass, ...). The row that matters most is the env pointer: dlxSurfaceChanged
// calls GetStringUTFChars through it, so a null env is a crash inside the first surface callback.
//
// GlSymbol is replaced here; Start still exercises its glViewport hand-off and the glGetError drain.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "rb/java/android_base.h"
#include "rb/java/dlgl.h"
#include "rb/java/table_java.h"
#include "dlgl_bootstrap.h"

namespace {
struct FakeCalls {
  std::vector<std::string> items;
  std::vector<std::uint32_t> envs;
};
FakeCalls* g_calls = nullptr;
}  // namespace

namespace rb::platform::ios {
void* GlSymbol(const std::string& name) {
  using Viewport = void (*)(int, int, int, int);
  if (name == "glViewport") {
    static Viewport viewport = [](int x, int y, int w, int h) {
      ::g_calls->items.push_back("viewport:" + std::to_string(x) + "," + std::to_string(y) + "," +
                                 std::to_string(w) + "," + std::to_string(h));
    };
    return reinterpret_cast<void*>(viewport);
  }
  if (name == "glGetError") {
    static unsigned (*get_error)() = [] { return 0u; };
    return reinterpret_cast<void*>(get_error);
  }
  return nullptr;
}
}  // namespace rb::platform::ios

namespace {

using namespace rb;

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
#define CHECK_CTX(cond, detail)                \
  do {                                         \
    ++checks;                                  \
    if (!(cond)) fail(#cond, __LINE__, detail); \
  } while (0)

constexpr std::uint32_t kEnv = 0x0011'1000u;

void Record(const char* what, void* env) {
  g_calls->items.emplace_back(what);
  g_calls->envs.push_back(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(env)));
}

void AssetManager(void* env, void*, std::uintptr_t manager) {
  Record("asset", env);
  g_calls->items.back() += manager == 0 ? ":null-manager" : ":manager";
}
void Init(void* env, void*) { Record("init", env); }
void Surface(void* env, void*, int w, int h, std::uintptr_t language) {
  Record("surface", env);
  g_calls->items.back() += ":" + std::to_string(w) + "x" + std::to_string(h) +
                           (language == 0 ? ":null-language" : ":language");
}
void Draw(void* env, void*) { Record("draw", env); }
void Touch(void* env, void*, int action, float x, float y) {
  Record("touch", env);
  char b[64];
  std::snprintf(b, sizeof b, ":%d:%.1f:%.1f", action, static_cast<double>(x), static_cast<double>(y));
  g_calls->items.back() += b;
}
void Background(void* env, void*) { Record("background", env); }
void Back(void* env, void*) { Record("back", env); }

struct Rig {
  java::TableJava java;
  FakeCalls calls;
  std::unique_ptr<platform::ios::DlglBootstrap> dlgl;
  std::string err;

  Rig() {
    java::AndroidEnv env;
    env.package_name = "com.doubleleft.androidkitkat";
    env.model = "iPad";
    env.manufacturer = "Apple";
    env.now_ms = [] { return static_cast<std::int64_t>(1'700'000'000'000); };
    env.uptime_ms = [] { return static_cast<std::int64_t>(12'345); };
    java::InstallAndroidBase(java, env);
    java::InstallDlgl(java, env);

    const auto surface = java.FindClass("com/doubleleft/DLGLSurface");
    const auto renderer = java.FindClass("com/doubleleft/DLGLRenderer");
    java.RegisterNatives(surface, {{"dlxAssetManager", "(Landroid/content/res/AssetManager;)V",
                                    reinterpret_cast<void*>(&AssetManager)},
                                   {"dlxTouchEvent", "(IFF)V", reinterpret_cast<void*>(&Touch)},
                                   {"dlxBackground", "()V", reinterpret_cast<void*>(&Background)},
                                   {"dlxBackButton", "()V", reinterpret_cast<void*>(&Back)}});
    java.RegisterNatives(renderer, {{"dlxInit", "()V", reinterpret_cast<void*>(&Init)},
                                    {"dlxSurfaceChanged", "(IILjava/lang/String;)V",
                                     reinterpret_cast<void*>(&Surface)},
                                    {"dlxDraw", "()V", reinterpret_cast<void*>(&Draw)}});
    g_calls = &calls;
    dlgl = std::make_unique<platform::ios::DlglBootstrap>(java, kEnv, "libdlgl.so",
                                                          [this](const std::string& line) { logs.push_back(line); });
  }
  ~Rig() { g_calls = nullptr; }

  std::vector<std::string> logs;
};

void the_native_calls_use_the_real_guest_jnienv() {
  Rig r;
  CHECK_CTX(r.dlgl->Start("game.apk", 640, 480, &r.err), r.err);
  const std::vector<std::string> expect = {"asset:manager", "init", "surface:640x480:language",
                                           "viewport:0,0,640,480"};
  CHECK_CTX(r.calls.items.size() == expect.size(), std::to_string(r.calls.items.size()));
  for (std::size_t i = 0; i < expect.size() && i < r.calls.items.size(); ++i) {
    CHECK_CTX(r.calls.items[i] == expect[i], r.calls.items[i] + " != " + expect[i]);
  }
  for (const std::uint32_t env : r.calls.envs) CHECK_CTX(env == kEnv, std::to_string(env));
  CHECK(!r.java.ExceptionCheck());

  const std::size_t before = r.calls.items.size();
  CHECK(r.dlgl->Render(&r.err));
  CHECK(r.dlgl->Pause(&r.err));
  CHECK(r.dlgl->Back(&r.err));
  r.dlgl->Touch(2, 12.5f, 34.5f);
  CHECK_CTX(r.calls.items.size() == before + 4, std::to_string(r.calls.items.size()));
  if (r.calls.items.size() == before + 4) {
    CHECK_CTX(r.calls.items[before] == "draw", r.calls.items[before]);
    CHECK_CTX(r.calls.items[before + 1] == "background", r.calls.items[before + 1]);
    CHECK_CTX(r.calls.items[before + 2] == "back", r.calls.items[before + 2]);
    CHECK_CTX(r.calls.items[before + 3] == "touch:2:12.5:34.5", r.calls.items[before + 3]);
  }
  for (std::size_t i = before; i < r.calls.envs.size(); ++i) CHECK_CTX(r.calls.envs[i] == kEnv, "post-start env");
}

void a_missing_native_is_refused_by_name() {
  java::TableJava java;
  java::AndroidEnv env;
  java::InstallAndroidBase(java, env);
  java::InstallDlgl(java, env);
  const auto surface = java.FindClass("com/doubleleft/DLGLSurface");
  java.RegisterNatives(surface, {{"dlxAssetManager", "(Landroid/content/res/AssetManager;)V",
                                  reinterpret_cast<void*>(&AssetManager)},
                                 {"dlxTouchEvent", "(IFF)V", reinterpret_cast<void*>(&Touch)},
                                 {"dlxBackground", "()V", reinterpret_cast<void*>(&Background)},
                                 {"dlxBackButton", "()V", reinterpret_cast<void*>(&Back)}});
  FakeCalls unused;
  g_calls = &unused;
  platform::ios::DlglBootstrap dlgl(java, kEnv, "libdlgl.so", [](const std::string&) {});
  std::string err;
  CHECK(!dlgl.Start("game.apk", 640, 480, &err));
  CHECK_CTX(err.find("dlxInit") != std::string::npos, err);
  g_calls = nullptr;
}

}  // namespace

int main() {
  the_native_calls_use_the_real_guest_jnienv();
  a_missing_native_is_refused_by_name();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
