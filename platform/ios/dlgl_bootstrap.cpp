// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See dlgl_bootstrap.h.

#include "dlgl_bootstrap.h"

#include "gl_symbols.h"

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

DlglBootstrap::DlglBootstrap(rb::java::TableJava& java, std::string lib,
                             std::function<void(const std::string&)> log)
    : java_(java), lib_(std::move(lib)), log_(std::move(log)) {}

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
  static_cast<VL>(asset_)(nullptr, nullptr, manager);
  static_cast<V0>(init_)(nullptr, nullptr);
  const auto language = java_.NewString(u"en");
  static_cast<VIIL>(surface_)(nullptr, nullptr, width, height, language);
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
  static_cast<V0>(draw_)(nullptr, nullptr);
  return true;
}

bool DlglBootstrap::Pause(std::string*) {
  if (started_ && pause_ != nullptr) static_cast<V0>(pause_)(nullptr, nullptr);
  return true;
}

bool DlglBootstrap::Back(std::string*) {
  if (started_ && back_ != nullptr) static_cast<V0>(back_)(nullptr, nullptr);
  return true;
}

void DlglBootstrap::Touch(int action, float x, float y) {
  if (started_ && touch_ != nullptr) static_cast<VIFF>(touch_)(nullptr, nullptr, action, x, y);
}

}  // namespace rb::platform::ios
