// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The Cocos2d-x bootstrap: see cocos2dx.h.

#include "cocos2dx.h"

#include <cstdio>

#include "rb/abi/abi.h"
#include "rb/abi/jni_forward.h"
#include "rb/host/boot.h"
#include "rb/java/table_java.h"
#include "rb/runtime/guest_process.h"

namespace rb::platform::ios {

namespace {

constexpr const char* kRenderer = "Java_org_cocos2dx_lib_Cocos2dxRenderer_";
constexpr const char* kSetApkPath = "Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath";

std::u16string U(const std::string& s) { return std::u16string(s.begin(), s.end()); }

}  // namespace

Cocos2dxBootstrap::Cocos2dxBootstrap(rb::host::Boot& boot, rb::java::TableJava& java, std::string lib)
    : boot_(boot), java_(java), lib_(std::move(lib)) {}

std::uint32_t Cocos2dxBootstrap::Export(const char* name, std::string* err) const {
  const rb::loader::GuestModule* m = boot_.module(lib_);
  if (m == nullptr) {
    if (err != nullptr) *err = lib_ + " is not a mounted library";
    return 0;
  }
  const auto e = m->FindExport(name);
  if (!e) {
    if (err != nullptr) *err = lib_ + " exports no " + name;
    return 0;
  }
  return e->tagged();
}

bool Cocos2dxBootstrap::Call(std::uint32_t fn, const char* shorty, const std::vector<rb::abi::Value32>& args,
                             const char* what, std::string* err) {
  rb::runtime::GuestProcess* p = boot_.process();
  if (p == nullptr) {
    if (err != nullptr) *err = "no guest process";
    return false;
  }
  rb::exec::RunResult rr{};
  p->abi().CallGuestToCompletion(fn, rb::abi::Args32{args}, rb::abi::Shorty::For(shorty),
                                 rb::runtime::GuestProcess::kThreadTurnBudget, &rr);
  if (rr.reason != rb::exec::StopReason::Returned) {
    if (err != nullptr) *err = std::string{what} + " stopped: " + rr.detail;
    return false;
  }
  return true;
}

bool Cocos2dxBootstrap::Start(const std::string& apk_path, int width, int height, std::string* err) {
  rb::abi::ForwardingJni* jni = boot_.jni();
  if (jni == nullptr || boot_.process() == nullptr) {
    if (err != nullptr) *err = "the JNI bridge is not up";
    return false;
  }
  const std::uint32_t set_path = Export(kSetApkPath, err);
  if (set_path == 0) return false;
  const std::uint32_t native_init = Export((std::string{kRenderer} + "nativeInit").c_str(), err);
  if (native_init == 0) return false;

  // The Java helper passes its context and the APK path; the native side only reads the string.
  jni->PushFrame();
  bool ok = false;
  {
    std::string why;
    const std::uint32_t guest_path = jni->ToGuestLocal(java_.NewString(U(apk_path)), &why);
    if (guest_path == 0) {
      if (err != nullptr) *err = "could not hand the APK path to the guest: " + why;
    } else {
      ok = Call(set_path, "VIIL",
                {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(0), rb::abi::Value32::Ptr(guest_path)},
                "nativeSetApkPath", err);
    }
  }
  if (ok) {
    ok = Call(native_init, "VIII",
              {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(0), rb::abi::Value32::S32(width),
               rb::abi::Value32::S32(height)},
              "nativeInit", err);
  }
  jni->PopFrame(0);
  started_ = ok;
  return ok;
}

bool Cocos2dxBootstrap::Render(std::string* err) {
  const std::uint32_t fn = Export((std::string{kRenderer} + "nativeRender").c_str(), err);
  if (fn == 0) return false;
  return Call(fn, "VII", {rb::abi::Value32::Ptr(boot_.jni()->env()), rb::abi::Value32::Ptr(0)}, "nativeRender", err);
}

bool Cocos2dxBootstrap::Pause(std::string* err) {
  const std::uint32_t fn = Export((std::string{kRenderer} + "nativeOnPause").c_str(), err);
  if (fn == 0) return false;
  return Call(fn, "VII", {rb::abi::Value32::Ptr(boot_.jni()->env()), rb::abi::Value32::Ptr(0)}, "nativeOnPause", err);
}

bool Cocos2dxBootstrap::Resume(std::string* err) {
  const std::uint32_t fn = Export((std::string{kRenderer} + "nativeOnResume").c_str(), err);
  if (fn == 0) return false;
  return Call(fn, "VII", {rb::abi::Value32::Ptr(boot_.jni()->env()), rb::abi::Value32::Ptr(0)}, "nativeOnResume", err);
}

namespace {

/// `nativeTouchesBegin(IFFI)V` / `nativeTouchesEnd(IFFI)V`.
void TouchSingle(rb::host::Boot& boot, const char* name, const std::string& lib, int id, float x, float y) {
  if (boot.process() == nullptr || boot.jni() == nullptr) return;
  const rb::loader::GuestModule* m = boot.module(lib);
  std::optional<rb::loader::CodeAddress> e;
  if (m != nullptr) e = m->FindExport(name);
  if (!e) return;
  rb::exec::RunResult rr{};
  boot.process()->abi().CallGuestToCompletion(
      e->tagged(),
      rb::abi::Args32{{rb::abi::Value32::Ptr(boot.jni()->env()), rb::abi::Value32::Ptr(0), rb::abi::Value32::S32(id),
                       rb::abi::Value32::F32(x), rb::abi::Value32::F32(y), rb::abi::Value32::S32(0)}},
      rb::abi::Shorty::For("VIIFFI"), rb::runtime::GuestProcess::kThreadTurnBudget, &rr);
}

}  // namespace

void Cocos2dxBootstrap::TouchBegin(int id, float x, float y) {
  TouchSingle(boot_, (std::string{kRenderer} + "nativeTouchesBegin").c_str(), lib_, id, x, y);
}

void Cocos2dxBootstrap::TouchEnd(int id, float x, float y) {
  TouchSingle(boot_, (std::string{kRenderer} + "nativeTouchesEnd").c_str(), lib_, id, x, y);
}

bool Cocos2dxBootstrap::TouchArrays(const char* native, int id, float x, float y) {
  rb::abi::ForwardingJni* jni = boot_.jni();
  const std::uint32_t fn = Export((std::string{kRenderer} + native).c_str(), nullptr);
  if (fn == 0 || jni == nullptr) return false;
  const auto ids = java_.NewArray('I', 1, 0, 0);
  const auto xs = java_.NewArray('F', 1, 0, 0);
  const auto ys = java_.NewArray('F', 1, 0, 0);
  if (ids == 0 || xs == 0 || ys == 0) return false;
  java_.SetArrayRegion('I', ids, 0, 1, &id);
  java_.SetArrayRegion('F', xs, 0, 1, &x);
  java_.SetArrayRegion('F', ys, 0, 1, &y);
  jni->PushFrame();
  std::string why;
  const std::uint32_t gids = jni->ToGuestLocal(ids, &why);
  const std::uint32_t gxs = jni->ToGuestLocal(xs, &why);
  const std::uint32_t gys = jni->ToGuestLocal(ys, &why);
  bool ok = false;
  if (gids != 0 && gxs != 0 && gys != 0) {
    ok = Call(fn, "VIILLLI",
              {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(0), rb::abi::Value32::Ptr(gids),
               rb::abi::Value32::Ptr(gxs), rb::abi::Value32::Ptr(gys), rb::abi::Value32::S32(0)},
              native, nullptr);
  }
  jni->PopFrame(0);
  return ok;
}

void Cocos2dxBootstrap::TouchMove(int id, float x, float y) { TouchArrays("nativeTouchesMove", id, x, y); }

void Cocos2dxBootstrap::TouchCancel(int id, float x, float y) { TouchArrays("nativeTouchesCancel", id, x, y); }

}  // namespace rb::platform::ios
