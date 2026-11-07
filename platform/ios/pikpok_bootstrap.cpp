// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See pikpok_bootstrap.h.

#include "pikpok_bootstrap.h"

#include "rb/abi/abi.h"
#include "rb/abi/jni_forward.h"
#include "rb/host/boot.h"
#include "rb/java/table_java.h"
#include "rb/runtime/guest_process.h"

namespace rb::platform::ios {

namespace {

constexpr const char* kActivityClass = "com/pikpok/MabActivity";
constexpr const char* kRendererClass = "com/pikpok/MabRenderer";
constexpr const char* kInputClass = "com/pikpok/MabInput";

}  // namespace

PikPokBootstrap::PikPokBootstrap(rb::host::Boot& boot, rb::java::TableJava& java, std::string lib)
    : boot_(boot), java_(java), lib_(std::move(lib)) {}

std::uint32_t PikPokBootstrap::Export(const char* name, std::string* err) const {
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

bool PikPokBootstrap::Call(std::uint32_t fn, const char* shorty, const std::vector<rb::abi::Value32>& args,
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

bool PikPokBootstrap::Start(const std::string& apk_path, int width, int height, std::string* err) {
  (void)apk_path;
  rb::abi::ForwardingJni* jni = boot_.jni();
  if (jni == nullptr || boot_.process() == nullptr) {
    if (err != nullptr) *err = "the JNI bridge is not up";
    return false;
  }
  if ((init_ = Export("Java_com_pikpok_MabActivity_initApplication", err)) == 0) return false;
  if ((acquired_ = Export("Java_com_pikpok_MabRenderer_nativeOnGraphicsContextAquired", err)) == 0) return false;
  if ((init_graphics_ = Export("Java_com_pikpok_MabRenderer_nativeInitGraphics", err)) == 0) return false;
  if ((render_ = Export("Java_com_pikpok_MabRenderer_nativeRender", err)) == 0) return false;
  resize_ = Export("Java_com_pikpok_MabRenderer_nativeResize", nullptr);
  resumed_ = Export("Java_com_pikpok_MabRenderer_nativeApplicationResumed", nullptr);
  paused_ = Export("Java_com_pikpok_MabRenderer_nativeApplicationPaused", nullptr);
  back_ = Export("Java_com_pikpok_MabRenderer_nativeOnBackButtonPressed", nullptr);
  touch_down_ = Export("Java_com_pikpok_MabInput_nativeTouchDown", nullptr);
  touch_move_ = Export("Java_com_pikpok_MabInput_nativeTouchMove", nullptr);
  touch_up_ = Export("Java_com_pikpok_MabInput_nativeTouchUp", nullptr);
  touch_cancel_ = Export("Java_com_pikpok_MabInput_nativeTouchCancel", nullptr);

  // The activity the Java layer would have built. It is a process-lifetime global reference because the
  // engine may keep it past the call (a local would die with this frame, and on a device Java would keep
  // the object alive for as long as its Activity does).
  jni->PushFrame();
  std::string why;
  const auto activity_ref = java_.NewGlobalRef(java_.NewInstance(kActivityClass));
  const std::uint32_t activity = activity_ref != 0 ? jni->ToGuestLocal(activity_ref, &why) : 0;
  const std::uint32_t empty_argv = jni->ToGuestLocal(
      java_.NewArray('L', 0, java_.FindClass("java/lang/String"), 0), &why);
  const std::uint32_t renderer = jni->ToGuestLocal(java_.FindClass(kRendererClass), &why);
  bool ok = activity != 0 && empty_argv != 0 && renderer != 0 &&
            Call(init_, "VIIL", {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(activity),
                                 rb::abi::Value32::Ptr(empty_argv)},
                 "initApplication", err);
  if (ok && resumed_ != 0) {
    ok = Call(resumed_, "VII", {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(renderer)},
              "nativeApplicationResumed", err);
  }
  if (ok) {
    ok = Call(acquired_, "VII", {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(renderer)},
              "nativeOnGraphicsContextAquired", err);
  }
  if (ok && resize_ != 0) {
    ok = Call(resize_, "VIII", {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(renderer),
                                rb::abi::Value32::S32(width), rb::abi::Value32::S32(height)},
              "nativeResize", err);
  }
  if (ok) {
    ok = Call(init_graphics_, "VIII", {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(renderer),
                                       rb::abi::Value32::S32(width), rb::abi::Value32::S32(height)},
              "nativeInitGraphics", err);
  }
  jni->PopFrame(0);
  started_ = ok;
  if (ok) started_at_ = std::chrono::steady_clock::now();
  return ok;
}

bool PikPokBootstrap::Render(std::string* err) {
  if (!started_) {
    if (err != nullptr) *err = "pikpok has not started";
    return false;
  }
  rb::abi::ForwardingJni* jni = boot_.jni();
  jni->PushFrame();
  std::string why;
  const std::uint32_t renderer = jni->ToGuestLocal(java_.FindClass(kRendererClass), &why);
  const bool ok = renderer != 0 &&
                  Call(render_, "VII", {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(renderer)},
                       "nativeRender", err);
  jni->PopFrame(0);
  return ok;
}

bool PikPokBootstrap::Pause(std::string* err) {
  if (!started_ || paused_ == 0) return true;
  rb::abi::ForwardingJni* jni = boot_.jni();
  jni->PushFrame();
  std::string why;
  const std::uint32_t renderer = jni->ToGuestLocal(java_.FindClass(kRendererClass), &why);
  const bool ok = Call(paused_, "VII", {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(renderer)},
                       "nativeApplicationPaused", err);
  jni->PopFrame(0);
  return ok;
}

bool PikPokBootstrap::Resume(std::string* err) {
  if (!started_ || resumed_ == 0) return true;
  rb::abi::ForwardingJni* jni = boot_.jni();
  jni->PushFrame();
  std::string why;
  const std::uint32_t renderer = jni->ToGuestLocal(java_.FindClass(kRendererClass), &why);
  const bool ok = Call(resumed_, "VII", {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(renderer)},
                       "nativeApplicationResumed", err);
  jni->PopFrame(0);
  return ok;
}

bool PikPokBootstrap::Back(std::string* err) {
  if (!started_ || back_ == 0) return true;
  rb::abi::ForwardingJni* jni = boot_.jni();
  jni->PushFrame();
  std::string why;
  const std::uint32_t renderer = jni->ToGuestLocal(java_.FindClass(kRendererClass), &why);
  const bool ok = Call(back_, "VII", {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(renderer)},
                       "nativeOnBackButtonPressed", err);
  jni->PopFrame(0);
  return ok;
}

void PikPokBootstrap::Touch(int id, int action, float x, float y) {
  // Android's MotionEvent action: down 0, up 1, move 2, cancel 3. The session hands begin/move/end/cancel.
  const std::uint32_t fn = action == 0 ? touch_down_ : action == 2 ? touch_move_ : action == 1 ? touch_up_ : touch_cancel_;
  if (!started_ || fn == 0) return;
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at_).count();
  rb::abi::ForwardingJni* jni = boot_.jni();
  jni->PushFrame();
  std::string why;
  const std::uint32_t input = jni->ToGuestLocal(java_.FindClass(kInputClass), &why);
  if (input != 0) {
    Call(fn, "VIIFFD",
         {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(input), rb::abi::Value32::S32(id),
          rb::abi::Value32::F32(x), rb::abi::Value32::F32(y), rb::abi::Value32::F64(elapsed)},
         "nativeTouch", nullptr);
  }
  jni->PopFrame(0);
}

}  // namespace rb::platform::ios
