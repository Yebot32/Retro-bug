// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See dlgl_bootstrap.h.

#include "dlgl_bootstrap.h"

#include "gl_symbols.h"

#include <cstdio>

namespace rb::platform::ios {

namespace {

constexpr const char* kRenderer = "com/doubleleft/DLGLRenderer";
constexpr const char* kSurface = "com/doubleleft/DLGLSurface";

// The registered natives are AAPCS64 entry points into the inbound pool; each type spells the JNI
// arguments ART would pass (env, jclass, then the method's own).
using V0 = void (*)(void*, void*);
using VL = void (*)(void*, void*, std::uintptr_t);
using VIIL = void (*)(void*, void*, int, int, std::uintptr_t);
using VIFF = void (*)(void*, void*, int, float, float);

}  // namespace

DlglBootstrap::DlglBootstrap(rb::java::TableJava& java, std::uint32_t jni_env, std::string lib,
                             std::function<void(const std::string&)> log)
    : java_(java), jni_env_(jni_env), lib_(std::move(lib)), log_(std::move(log)) {}

void* DlglBootstrap::Env() const { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(jni_env_)); }

void* DlglBootstrap::Native(const char* cls, const char* name, std::string* err) const {
  for (const auto& binding : java_.natives(cls)) {
    if (binding.name == name) return binding.host_fn;
  }
  if (err != nullptr) *err = std::string{"the game registered no "} + cls + "." + name;
  return nullptr;
}

bool DlglBootstrap::Start(const std::string& apk, int width, int height, std::string* err) {
  (void)apk;
  if ((asset_ = Native(kSurface, "dlxAssetManager", err)) == nullptr) return false;
  if ((init_ = Native(kRenderer, "dlxInit", err)) == nullptr) return false;
  if ((surface_ = Native(kRenderer, "dlxSurfaceChanged", err)) == nullptr) return false;
  if ((draw_ = Native(kRenderer, "dlxDraw", err)) == nullptr) return false;
  if ((touch_ = Native(kSurface, "dlxTouchEvent", err)) == nullptr) return false;
  if ((pause_ = Native(kSurface, "dlxBackground", err)) == nullptr) return false;
  if ((back_ = Native(kSurface, "dlxBackButton", err)) == nullptr) return false;

  // DLGLSurface's constructor hands the AssetManager over before the renderer exists; then the renderer's
  // onSurfaceCreated/onSurfaceChanged call dlxInit and dlxSurfaceChanged(w, h, language) + glViewport.
  const auto manager = java_.NewInstance("android/content/res/AssetManager");
  reinterpret_cast<VL>(asset_)(Env(), nullptr, manager);
  Drain("dlxAssetManager");
  reinterpret_cast<V0>(init_)(Env(), nullptr);
  Drain("dlxInit");
  const auto language = java_.NewString(u"en");
  reinterpret_cast<VIIL>(surface_)(Env(), nullptr, width, height, language);
  Drain("dlxSurfaceChanged");
  if (java_.ExceptionCheck() && log_) log_("dlgl: pending " + java_.PendingClass() + " after surfaceChanged");
  using Viewport = void (*)(int, int, int, int);
  const auto viewport = reinterpret_cast<Viewport>(GlSymbol("glViewport"));
  if (viewport != nullptr) viewport(0, 0, width, height);
  started_ = true;
  if (log_) log_("dlgl: assets, init, surface " + std::to_string(width) + "x" + std::to_string(height) + ", " +
                 std::to_string(java_.natives(kRenderer).size()) + " renderer native(s)");
  return true;
}

bool DlglBootstrap::Render(std::string* err) {
  if (!started_) {
    if (err != nullptr) *err = "dlgl has not started";
    return false;
  }
  reinterpret_cast<V0>(draw_)(Env(), nullptr);
  return true;
}

bool DlglBootstrap::Pause(std::string*) {
  if (started_ && pause_ != nullptr) reinterpret_cast<V0>(pause_)(Env(), nullptr);
  return true;
}

bool DlglBootstrap::Back(std::string*) {
  if (started_ && back_ != nullptr) reinterpret_cast<V0>(back_)(Env(), nullptr);
  return true;
}

void DlglBootstrap::Touch(int action, float x, float y) {
  if (started_ && touch_ != nullptr) reinterpret_cast<VIFF>(touch_)(Env(), nullptr, action, x, y);
}

void DlglBootstrap::Drain(const char* after) {
  using GetError = unsigned (*)();
  static const auto get_error = reinterpret_cast<GetError>(GlSymbol("glGetError"));
  if (get_error == nullptr) return;
  for (int i = 0; i < 8; ++i) {
    const unsigned e = get_error();
    if (e == 0) return;
    if (log_) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "0x%x", e);
      log_(std::string{"dlgl: glGetError after "} + after + " = " + buf);
    }
  }
}

}  // namespace rb::platform::ios
