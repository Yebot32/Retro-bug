// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The iOS session: see rb_ios.h. It is the only place that composes the boot, the EGL shim, the Java
// replacement and a title's bootstrap, and it owns the one thread that touches GL.

#include "rb_ios.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cocos2dx_bootstrap.h"
#include "egl_shim.h"
#include "ios_host.h"
#include "rb/host/boot.h"
#include "rb/java/android_base.h"
#include "rb/java/cocos2dx.h"
#include "rb/java/table_java.h"

struct RbIosSession {
  struct Touch {
    int kind = 0;  // 0 begin, 1 move, 2 end, 3 cancel
    int id = 0;
    float x = 0, y = 0;
  };

  std::string apk, lib, data_dir;
  int client_version = 2;
  int width = 0, height = 0;

  std::unique_ptr<rb::host::Boot> boot;                              ///< owns the guest process and the Java
  rb::java::TableJava* java = nullptr;                               ///< owned by boot, as its HostJava
  std::unique_ptr<rb::platform::ios::IosHost> host;                  ///< the guest's import answers
  std::unique_ptr<rb::platform::ios::Cocos2dxBootstrap> game;

  std::thread render;
  std::atomic<bool> running{false};
  std::atomic<int> frames{0};

  std::mutex mu;  ///< leaf: `error` and `log`
  std::string error;
  std::deque<std::string> log;

  std::mutex touch_mu;
  std::vector<Touch> touches;

  void Log(const std::string& line) {
    std::lock_guard<std::mutex> lk(mu);
    if (log.size() >= 400) log.pop_front();
    log.push_back(line);
    std::fprintf(stderr, "RB %s\n", line.c_str());
  }
  void Fail(const std::string& e) {
    {
      std::lock_guard<std::mutex> lk(mu);
      error = e;
    }
    Log("error: " + e);
  }
};

namespace {

void CopyOut(const std::string& text, char* out, int out_len) {
  if (out == nullptr || out_len <= 0) return;
  const std::size_t n = std::min<std::size_t>(text.size(), static_cast<std::size_t>(out_len - 1));
  std::memcpy(out, text.data(), n);
  out[n] = '\0';
}

void Take(RbIosSession* s, int kind, int id, float x, float y) {
  RbIosSession::Touch t;
  t.kind = kind;
  t.id = id;
  t.x = x;
  t.y = y;
  std::lock_guard<std::mutex> lk(s->touch_mu);
  s->touches.push_back(t);
}

}  // namespace

extern "C" RbIosSession* rb_ios_create(const char* apk_path, const char* lib_name, const char* data_dir,
                                       int client_version, char* err, int err_len) {
  if (apk_path == nullptr || lib_name == nullptr) {
    CopyOut("an APK path and a library name are required", err, err_len);
    return nullptr;
  }
  auto s = std::make_unique<RbIosSession>();
  s->apk = apk_path;
  s->lib = lib_name;
  s->data_dir = data_dir != nullptr ? data_dir : "";
  s->client_version = client_version == 1 ? 1 : 2;

  rb::java::AndroidEnv env;
  env.package_name = "retrobug.game";
  env.files_dir = s->data_dir + "/files";
  env.cache_dir = s->data_dir + "/cache";
  env.model = "iPad";
  env.manufacturer = "Apple";
  env.brand = "Apple";
  env.device = "iPad";
  env.fingerprint = "retrobug/ios";
  env.sdk_int = 34;

  rb::host::BootConfig cfg;
  cfg.ctor_tier = 1;    // iOS runs guest code on Tier I only
  cfg.enable_jit = false;  // and never builds the JIT object: iOS forbids executable memory
  cfg.monitor = false;
  cfg.log = [sp = s.get()](const std::string& line) { sp->Log(line); };
  cfg.make_java = [sp = s.get(), env](std::string*) -> std::unique_ptr<rb::abi::HostJava> {
    auto java = std::make_unique<rb::java::TableJava>();
    rb::java::InstallAndroidBase(*java, env);
    rb::java::InstallCocos2dx(*java, env);
    sp->java = java.get();
    return java;
  };
  cfg.resolve_host = [sp = s.get()](const std::string& name) -> std::optional<rb::exec::GuestAddr> {
    return sp->host ? sp->host->Resolve(name) : std::nullopt;
  };
  cfg.on_bridge = [sp = s.get()](rb::host::Boot&, rb::runtime::GuestProcess& process) {
    sp->host = std::make_unique<rb::platform::ios::IosHost>(
        process, [sp](const std::string& line) { sp->Log(line); });
    std::string why;
    if (!sp->host->OpenApk(sp->apk, &why)) sp->Log("assets: " + why);
  };

  s->boot = std::make_unique<rb::host::Boot>(std::move(cfg));
  const rb::host::BootResult result = s->boot->BootLibrary(s->apk, s->lib);
  if (!result.ok) {
    CopyOut(result.report.empty() ? std::string{"the boot failed"} : result.report, err, err_len);
    return nullptr;
  }
  s->game = std::make_unique<rb::platform::ios::Cocos2dxBootstrap>(*s->boot, *s->java, s->lib);
  return s.release();
}

extern "C" int rb_ios_start(RbIosSession* s, void* layer, int width, int height, char* err, int err_len) {
  if (s == nullptr || s->game == nullptr) {
    CopyOut("no game session", err, err_len);
    return 0;
  }
  if (s->running.load()) return 1;
  if (layer == nullptr || width <= 0 || height <= 0) {
    CopyOut("no drawable", err, err_len);
    return 0;
  }
  s->width = width;
  s->height = height;
  s->running.store(true);

  s->render = std::thread([s, layer] {
    rb::platform::ios::EglShim& egl = rb::platform::ios::EglShim::Instance();
    egl.SetLayer(layer, s->client_version);
    std::string why;
    if (!egl.Begin(&why)) {
      s->Fail("EGL context: " + why);
      egl.ReleaseThread();
      s->running.store(false);
      return;
    }
    if (!s->game->Start(s->apk, s->width, s->height, &why)) {
      s->Fail("nativeInit: " + why);
      egl.ReleaseThread();
      s->running.store(false);
      return;
    }
    s->Log("started: " + std::to_string(s->width) + "x" + std::to_string(s->height));
    while (s->running.load()) {
      std::vector<RbIosSession::Touch> queued;
      {
        std::lock_guard<std::mutex> lk(s->touch_mu);
        queued.swap(s->touches);
      }
      for (const RbIosSession::Touch& t : queued) {
        switch (t.kind) {
          case 0: s->game->TouchBegin(t.id, t.x, t.y); break;
          case 1: s->game->TouchMove(t.id, t.x, t.y); break;
          case 2: s->game->TouchEnd(t.id, t.x, t.y); break;
          default: s->game->TouchCancel(t.id, t.x, t.y); break;
        }
      }
      if (!s->game->Render(&why)) {
        s->Fail("nativeRender: " + why);
        break;
      }
      if (!egl.Present(&why)) {
        s->Fail("eglSwapBuffers: " + why);
        break;
      }
      ++s->frames;
      // No sleep: Cocos2d-x paces itself (setAnimationInterval/onDrawFrame's interval); adding a second
      // 16 ms here would halve the frame rate. The swap is the present.
      std::this_thread::yield();
    }
    egl.ReleaseThread();
    s->running.store(false);
  });
  return 1;
}

extern "C" void rb_ios_stop(RbIosSession* s) {
  if (s == nullptr) return;
  s->running.store(false);
  if (s->render.joinable()) s->render.join();
}

extern "C" void rb_ios_destroy(RbIosSession* s) {
  if (s == nullptr) return;
  rb_ios_stop(s);
  rb::platform::ios::EglShim::Instance().Detach();
  s->game.reset();
  s->host.reset();
  s->boot.reset();
  delete s;
}

extern "C" void rb_ios_touch_begin(RbIosSession* s, int id, float x, float y) {
  if (s != nullptr) Take(s, 0, id, x, y);
}
extern "C" void rb_ios_touch_move(RbIosSession* s, int id, float x, float y) {
  if (s != nullptr) Take(s, 1, id, x, y);
}
extern "C" void rb_ios_touch_end(RbIosSession* s, int id, float x, float y) {
  if (s != nullptr) Take(s, 2, id, x, y);
}
extern "C" void rb_ios_touch_cancel(RbIosSession* s, int id, float x, float y) {
  if (s != nullptr) Take(s, 3, id, x, y);
}

extern "C" int rb_ios_frame_count(RbIosSession* s) { return s != nullptr ? s->frames.load() : 0; }

extern "C" void rb_ios_last_error(RbIosSession* s, char* out, int out_len) {
  if (s == nullptr) {
    CopyOut("no session", out, out_len);
    return;
  }
  std::lock_guard<std::mutex> lk(s->mu);
  CopyOut(s->error, out, out_len);
}

extern "C" int rb_ios_log_tail(RbIosSession* s, char* out, int out_len) {
  if (s == nullptr || out == nullptr || out_len <= 0) return 0;
  std::lock_guard<std::mutex> lk(s->mu);
  std::string text;
  for (const std::string& line : s->log) {
    text += line;
    text += '\n';
  }
  CopyOut(text, out, out_len);
  return static_cast<int>(std::strlen(out));
}
