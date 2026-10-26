// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The iOS session: see rb_ios.h. It is the only place that composes the boot, the EGL shim, the Java
// replacement and a title's bootstrap, and it owns the one thread that touches GL.
//
// The boot runs on that thread too, not on the caller's: a Unity/Cocos boot is seconds of guest work, and
// doing it in makeUIView blocked the main thread (and could be killed by the watchdog). rb_ios_create now
// only records what to boot; rb_ios_start's thread boots it once, then initialises and draws.

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

#include <sys/mman.h>
#include <sys/stat.h>

#include "cocos2dx_bootstrap.h"
#include "dlgl_bootstrap.h"
#include "egl_shim.h"
#include "ios_host.h"
#include "rb/host/boot.h"
#include "rb/java/android_base.h"
#include "rb/java/cocos2dx.h"
#include "rb/java/dlgl.h"
#include "rb/loader/apk_assets.h"
#include "rb/java/table_java.h"
#include "rb/loader/guest_elf.h"
#include "rb/runtime/inbound.h"
#include "text_render.h"

struct RbIosSession {
  struct Touch {
    int kind = 0;  // 0 begin, 1 move, 2 end, 3 cancel
    int id = 0;
    float x = 0, y = 0;
  };

  std::string apk, lib, data_dir;
  int client_version = 2;
  int width = 0, height = 0;
  /// The guest window this iOS build can actually reserve (4 GiB is refused; measured), tried largest
  /// first at session creation. 0 means none was found and the boot will fail by name.
  std::size_t window_bytes = 0;

  std::unique_ptr<rb::host::Boot> boot;                              ///< owns the guest process and the Java
  rb::java::TableJava* java = nullptr;                               ///< owned by boot, as its HostJava
  std::unique_ptr<rb::platform::ios::IosHost> host;                  ///< the guest's import answers
  std::unique_ptr<rb::platform::ios::Cocos2dxBootstrap> game;
  std::unique_ptr<rb::platform::ios::DlglBootstrap> dlgl;  ///< KitKat Challenge's DLGE engine, when that is the lib
  bool dlgl_apk = false;  ///< the APK ships assets/rb32/libdlgl.so: boot the whole DLGE Java load order
  bool booted = false;  ///< render thread only

  std::thread render;
  std::atomic<bool> running{false};
  std::atomic<bool> want_run{true};  ///< false while the app is in the background
  std::atomic<int> frames{0};

  std::mutex mu;  ///< leaf: `error`, `log` and `log_file`
  std::string error;
  std::deque<std::string> log;
  std::FILE* log_file = nullptr;  ///< appended and flushed per line, so a crash leaves the lines that led to it

  std::mutex touch_mu;
  std::vector<Touch> touches;

  void Log(const std::string& line) {
    std::lock_guard<std::mutex> lk(mu);
    if (log.size() >= 400) log.pop_front();
    log.push_back(line);
    std::fprintf(stderr, "RB %s\n", line.c_str());
    if (log_file != nullptr) {
      std::fprintf(log_file, "%s\n", line.c_str());
      std::fflush(log_file);
    }
  }
  void Fail(const std::string& e) {
    {
      std::lock_guard<std::mutex> lk(mu);
      error = e;
    }
    Log("error: " + e);
  }

  /// Boots the library and creates its bootstrap. Called on the render thread, once.
  bool Boot(std::string* err);
};

namespace {

void CopyOut(const std::string& text, char* out, int out_len) {
  if (out == nullptr || out_len <= 0) return;
  const std::size_t n = std::min<std::size_t>(text.size(), static_cast<std::size_t>(out_len - 1));
  std::memcpy(out, text.data(), n);
  out[n] = '\0';
}

/// The largest PROT_NONE window this process can reserve, tried largest first. iOS refuses a single
/// 4 GiB mapping (measured: ENOMEM on a 4 GiB probe), so the Android default cannot be used as-is; the
/// guest only addresses what the window covers, and every v1 native title fits in 2 GiB.
std::size_t PickWindowBytes(RbIosSession* s) {
  constexpr std::size_t kMiB = std::size_t{1} << 20;
  for (const std::size_t mib : {std::size_t{4096}, std::size_t{3072}, std::size_t{2048}, std::size_t{1536},
                                std::size_t{1024}, std::size_t{768}, std::size_t{512}}) {
    void* p = ::mmap(nullptr, mib * kMiB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
      ::munmap(p, mib * kMiB);
      s->Log("window: " + std::to_string(mib) + " MiB is reservable; using it");
      return mib * kMiB;
    }
    s->Log("window: " + std::to_string(mib) + " MiB refused (errno " + std::to_string(errno) + ")");
  }
  return 0;
}

/// One-time look at what this iOS build permits on the window it chose: the engine needs subrange
/// mprotect and MAP_FIXED, and the answer decides whether the host can enforce permissions. Logged to
/// retrobug.log so a device run answers it without a debugger.
void ProbeWindowMemory(RbIosSession* s) {
  if (s->window_bytes == 0) return;
  void* p = ::mmap(nullptr, s->window_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    s->Log("probe: the chosen window was refused now (errno " + std::to_string(errno) + ")");
    return;
  }
  char* at = static_cast<char*>(p) + (12u << 20);
  errno = 0; const int mp_none = ::mprotect(at, 16384, PROT_NONE); const int e1 = errno;
  errno = 0; void* fx_none = ::mmap(at, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0); const int e2 = errno;
  errno = 0; void* fx_rw = ::mmap(at, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0); const int e3 = errno;
  errno = 0; const int mp_rw = ::mprotect(at, 16384, PROT_READ | PROT_WRITE); const int e4 = errno;
  errno = 0; const int mp_none2 = ::mprotect(at, 16384, PROT_NONE); const int e5 = errno;
  char buf[384];
  std::snprintf(buf, sizeof buf,
                "probe: reserve=%p miB=%zu mprotect(NONE)=%d/%d MAP_FIXED(NONE)=%p/%d MAP_FIXED(RW)=%p/%d mprotect(RW)=%d/%d mprotect(NONE2)=%d/%d",
                p, s->window_bytes >> 20, mp_none, e1, fx_none, e2, fx_rw, e3, mp_rw, e4, mp_none2, e5);
  s->Log(buf);
  ::munmap(p, s->window_bytes);
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

bool RbIosSession::Boot(std::string* err) {
  RbIosSession* self = this;
  rb::java::AndroidEnv env;
  env.package_name = "retrobug.game";
  env.apk_path = apk;
  env.files_dir = data_dir + "/files";
  env.cache_dir = data_dir + "/cache";
  env.model = "iPad";
  env.manufacturer = "Apple";
  env.brand = "Apple";
  env.device = "iPad";
  env.fingerprint = "retrobug/ios";
  env.sdk_int = 34;
  env.render_text = [](const std::string& text, float font_size, int max_w, int max_h, int& w, int& h,
                       std::vector<std::uint8_t>& pixels) {
    return rb::platform::ios::RenderText(text, font_size, max_w, max_h, w, h, pixels);
  };
  env.publish_text = [self](int width, int height, std::uintptr_t pixels) {
    if (self->boot == nullptr || self->boot->inbound() == nullptr) return;
    const rb::loader::GuestModule* m = self->boot->module(self->lib);
    const auto fn = m != nullptr ? m->FindExport("Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC")
                                 : std::nullopt;
    if (!fn) {
      self->Log("text: the game exports no nativeInitBitmapDC");
      return;
    }
    // A Java->native call from inside a HostJava body: Inbound marshals it to the guest, as ART
    // would (its own frame, the guest thread that is already running this call).
    rb::runtime::CapturedArgs args{};
    args.x[0] = self->boot->jni()->env();
    args.x[1] = 0;
    args.x[2] = static_cast<std::uint64_t>(width);
    args.x[3] = static_cast<std::uint64_t>(height);
    args.x[4] = pixels;
    args.stack = nullptr;
    self->boot->inbound()->Dispatch({fn->tagged(), "VIIL", "nativeInitBitmapDC"}, args);
  };

  rb::host::BootConfig cfg;
  if (window_bytes != 0) cfg.window_bytes = window_bytes;
  cfg.ctor_tier = 1;      // iOS runs guest code on Tier I only
  cfg.enable_jit = false;  // and never builds the JIT object: iOS forbids executable memory
  cfg.monitor = false;
  cfg.log = [self](const std::string& line) { self->Log(line); };
  cfg.make_java = [self, env](std::string*) -> std::unique_ptr<rb::abi::HostJava> {
    auto java = std::make_unique<rb::java::TableJava>();
    rb::java::InstallAndroidBase(*java, env);
    rb::java::InstallCocos2dx(*java, env);
    rb::java::InstallDlgl(*java, env);
    self->java = java.get();
    java->set_trace([self](const std::string& line) { self->Log(line); });
    return java;
  };
  cfg.resolve_host = [self](const std::string& name) -> std::optional<rb::exec::GuestAddr> {
    return self->host ? self->host->Resolve(name) : std::nullopt;
  };
  cfg.on_bridge = [self](rb::host::Boot& boot, rb::runtime::GuestProcess& process) {
    self->host = std::make_unique<rb::platform::ios::IosHost>(
        process, boot, [self](const std::string& line) { self->Log(line); });
    std::string why;
    if (!self->host->OpenApk(self->apk, &why)) self->Log("assets: " + why);
  };

  boot = std::make_unique<rb::host::Boot>(std::move(cfg));
  const rb::host::BootResult result = boot->BootJavaLibraries(apk, lib);
  if (!result.ok) {
    *err = result.report.empty() ? std::string{"the boot failed"} : result.report;
    // A failed boot leaves a half-initialised process; dropping it lets the next play start clean.
    boot.reset();
    host.reset();
    java = nullptr;
    return false;
  }
  if (java == nullptr) {
    *err = "the Java shim was never created";
    boot.reset();
    host.reset();
    return false;
  }
  if (dlgl_apk || lib == "libdlgl.so") {
    dlgl = std::make_unique<rb::platform::ios::DlglBootstrap>(*java, lib,
                                                              [self](const std::string& line) { self->Log(line); });
  } else {
    game = std::make_unique<rb::platform::ios::Cocos2dxBootstrap>(*boot, *java, lib);
  }
  return true;
}

extern "C" RbIosSession* rb_ios_create(const char* apk_path, const char* lib_name, const char* data_dir,
                                       const char* log_path, int client_version, char* err, int err_len) {
  if (apk_path == nullptr || lib_name == nullptr) {
    CopyOut("an APK path and a library name are required", err, err_len);
    return nullptr;
  }
  auto s = std::make_unique<RbIosSession>();
  if (log_path != nullptr && log_path[0] != '\0') s->log_file = std::fopen(log_path, "ab");
  s->apk = apk_path;
  s->lib = lib_name;
  if (s->lib.empty()) {
    // The largest assets/rb32/*.so is the game's engine (the repack layout keeps every sibling there).
    std::string why;
    auto archive = rb::loader::ApkArchive::Open(s->apk, &why);
    if (archive) {
      std::uint32_t best = 0;
      for (const std::string& name : archive->Names()) {
        if (name.rfind("assets/rb32/", 0) != 0 || name.size() < 4 || name.compare(name.size() - 3, 3, ".so") != 0) {
          continue;
        }
        const auto size = archive->EntrySize(name);
        if (size && *size > best) {
          best = *size;
          s->lib = name.substr(name.rfind('/') + 1);
        }
      }
      if (!s->lib.empty()) s->Log("auto: " + s->lib + " (" + std::to_string(best) + " bytes)");
    } else {
      s->Log("auto: cannot read the APK: " + why);
    }
    if (s->lib.empty()) {
      CopyOut("no library name was given and the APK has no assets/rb32/*.so", err, err_len);
      return nullptr;
    }
  }
  s->data_dir = data_dir != nullptr ? data_dir : "";
  s->client_version = client_version == 1 ? 1 : 2;
  s->dlgl_apk = rb::host::Boot::BundlesDlgl(s->apk);
  if (s->dlgl_apk || s->lib == "libdlgl.so") s->client_version = 1;  // KitKat's DLGE draws with GLES1
  if (!s->data_dir.empty()) {
    ::mkdir((s->data_dir + "/files").c_str(), 0755);
    ::mkdir((s->data_dir + "/cache").c_str(), 0755);
  }
  s->window_bytes = PickWindowBytes(s.get());
  s->Log("create: apk=" + s->apk + " lib=" + s->lib + " (boot deferred to the render thread)");
  return s.release();
}

extern "C" int rb_ios_start(RbIosSession* s, void* layer, int width, int height, char* err, int err_len) {
  if (s == nullptr) {
    CopyOut("no session", err, err_len);
    return 0;
  }
  if (s->running.load()) return 1;
  if (layer == nullptr || width <= 0 || height <= 0) {
    CopyOut("no drawable", err, err_len);
    return 0;
  }
  // A previous thread may have exited on an error without anyone joining it; assigning over a joinable
  // std::thread terminates the process.
  if (s->render.joinable()) s->render.join();
  {
    std::lock_guard<std::mutex> lk(s->mu);
    s->error.clear();
  }
  {
    std::lock_guard<std::mutex> lk(s->touch_mu);
    s->touches.clear();
  }
  s->frames.store(0);
  s->want_run.store(true);
  s->width = width;
  s->height = height;
  s->running.store(true);

  s->render = std::thread([s, layer] {
    rb::platform::ios::EglShim& egl = rb::platform::ios::EglShim::Instance();
    std::string why;
    try {
      static std::atomic<bool> probed{false};
      if (!probed.exchange(true)) ProbeWindowMemory(s);
      if (!s->booted) {
        s->Log("game: booting " + s->apk + " lib=" + s->lib);
        if (!s->Boot(&why)) {
          s->Fail("boot: " + why);
          s->running.store(false);
          return;
        }
        s->booted = true;
        s->Log("game: booted");
      }
      egl.SetLayer(layer, s->client_version);
      if (!egl.Begin(&why)) {
        s->Fail("EGL context: " + why);
        egl.ReleaseThread();
        s->running.store(false);
        return;
      }
      const bool started = s->dlgl ? s->dlgl->Start(s->apk, s->width, s->height, &why)
                                   : s->game->Start(s->apk, s->width, s->height, &why);
      if (!started) {
        s->Fail(std::string{s->dlgl ? "dlxInit" : "nativeInit"} + ": " + why);
        egl.ReleaseThread();
        s->running.store(false);
        return;
      }
      s->Log("started: " + std::to_string(s->width) + "x" + std::to_string(s->height));
      bool app_paused = false;
      while (s->running.load()) {
        const bool want = s->want_run.load();
        if (want != !app_paused) {
          std::string e;
          if (want) {
            if (s->dlgl) s->dlgl->Resume(&e); else s->game->Resume(&e);
            s->Log("resume");
          } else {
            if (s->dlgl) s->dlgl->Pause(&e); else s->game->Pause(&e);
            s->Log("pause");
          }
          app_paused = !want;
        }
        if (app_paused) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }
        std::vector<RbIosSession::Touch> queued;
        {
          std::lock_guard<std::mutex> lk(s->touch_mu);
          queued.swap(s->touches);
        }
        for (const RbIosSession::Touch& t : queued) {
          if (s->dlgl) {
            // Android's MotionEvent action: down 0, up 1, move 2, cancel 3.
            const int action = t.kind == 0 ? 0 : t.kind == 1 ? 2 : t.kind == 2 ? 1 : 3;
            s->dlgl->Touch(action, t.x, t.y);
            continue;
          }
          switch (t.kind) {
            case 0: s->game->TouchBegin(t.id, t.x, t.y); break;
            case 1: s->game->TouchMove(t.id, t.x, t.y); break;
            case 2: s->game->TouchEnd(t.id, t.x, t.y); break;
            default: s->game->TouchCancel(t.id, t.x, t.y); break;
          }
        }
        const bool rendered = s->dlgl ? s->dlgl->Render(&why) : s->game->Render(&why);
        if (!rendered) {
          s->Fail(std::string{s->dlgl ? "dlxDraw" : "nativeRender"} + ": " + why);
          break;
        }
        if (!egl.Present(&why)) {
          s->Fail("eglSwapBuffers: " + why);
          break;
        }
        ++s->frames;
        // No sleep: the engine paces itself (Cocos's onDrawFrame interval, DLGE's own frame timing); a
        // second 16 ms here would halve the frame rate. The swap is the present.
        std::this_thread::yield();
      }
    } catch (const std::exception& e) {
      s->Fail(std::string{"host exception: "} + e.what());
    } catch (...) {
      s->Fail("host exception of an unknown type");
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

extern "C" void rb_ios_pause(RbIosSession* s) {
  if (s != nullptr) s->want_run.store(false);
}

extern "C" void rb_ios_resume(RbIosSession* s) {
  if (s != nullptr) s->want_run.store(true);
}

extern "C" void rb_ios_destroy(RbIosSession* s) {
  if (s == nullptr) return;
  rb_ios_stop(s);
  rb::platform::ios::EglShim::Instance().Detach();
  s->game.reset();
  s->host.reset();
  s->boot.reset();
  s->Log("destroy");
  if (s->log_file != nullptr) {
    std::fclose(s->log_file);
    s->log_file = nullptr;
  }
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
