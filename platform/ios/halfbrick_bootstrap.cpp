// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See halfbrick_bootstrap.h.

#include "halfbrick_bootstrap.h"

#include "rb/abi/jni_forward.h"
#include "rb/host/boot.h"
#include "rb/java/table_java.h"
#include "rb/runtime/guest_process.h"

namespace rb::platform::ios {

namespace {

constexpr const char* kNativeClass = "com/halfbrick/mortar/NativeGameLib";
constexpr const char* kPrefix = "Java_com_halfbrick_mortar_NativeGameLib_native_1";

std::u16string U(const std::string& s) { return std::u16string(s.begin(), s.end()); }

}  // namespace

HalfbrickBootstrap::HalfbrickBootstrap(rb::host::Boot& boot, rb::java::TableJava& java, std::string lib,
                                       std::string files_dir, std::string cache_dir)
    : boot_(boot),
      java_(java),
      lib_(std::move(lib)),
      files_dir_(std::move(files_dir)),
      cache_dir_(std::move(cache_dir)) {}

std::uint32_t HalfbrickBootstrap::Export(const char* name, std::string* err) const {
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

rb::abi::Ret32 HalfbrickBootstrap::Call(std::uint32_t fn, const char* shorty,
                                        const std::vector<rb::abi::Value32>& args,
                                        rb::exec::RunResult* outcome) {
  return boot_.process()->abi().CallGuestToCompletion(fn, rb::abi::Args32{args}, rb::abi::Shorty::For(shorty),
                                                     rb::runtime::GuestProcess::kThreadTurnBudget, outcome);
}

bool HalfbrickBootstrap::CallVoid(std::uint32_t fn, const char* shorty,
                                  const std::vector<rb::abi::Value32>& args, const char* what,
                                  std::string* err) {
  rb::exec::RunResult rr{};
  Call(fn, shorty, args, &rr);
  if (rr.reason != rb::exec::StopReason::Returned) {
    if (err != nullptr) *err = std::string{what} + " stopped: " + rr.detail;
    return false;
  }
  return true;
}

bool HalfbrickBootstrap::Start(const std::string& apk_path, int width, int height, std::string* err) {
  rb::abi::ForwardingJni* jni = boot_.jni();
  if (jni == nullptr || boot_.process() == nullptr) {
    if (err != nullptr) *err = "the JNI bridge is not up";
    return false;
  }
  if ((init_files_ = Export((std::string{kPrefix} + "InitFileManager").c_str(), err)) == 0) return false;
  if ((system_init_ = Export((std::string{kPrefix} + "SystemInit").c_str(), err)) == 0) return false;
  if ((game_init_ = Export((std::string{kPrefix} + "GameInit").c_str(), err)) == 0) return false;
  if ((step_ = Export((std::string{kPrefix} + "step").c_str(), err)) == 0) return false;
  init_opensl_ = Export((std::string{kPrefix} + "InitOpenSLSoundManager").c_str(), nullptr);
  init_java_sound_ = Export((std::string{kPrefix} + "InitJavaSoundManager").c_str(), nullptr);
  touch_ = Export((std::string{kPrefix} + "touchEvent").c_str(), nullptr);
  on_pause_ = Export((std::string{kPrefix} + "onPause").c_str(), nullptr);
  on_resume_ = Export((std::string{kPrefix} + "onResume").c_str(), nullptr);
  on_focus_lost_ = Export((std::string{kPrefix} + "onFocusLost").c_str(), nullptr);
  on_focus_retrieved_ = Export((std::string{kPrefix} + "onFocusRetrieved").c_str(), nullptr);

  jni->PushFrame();
  std::string why;
  const std::uint32_t cls = jni->ToGuestLocal(java_.FindClass(kNativeClass), &why);
  const std::uint32_t apk = jni->ToGuestLocal(java_.NewString(U(apk_path)), &why);
  const std::uint32_t files = jni->ToGuestLocal(java_.NewString(U(files_dir_ + "/")), &why);
  const std::uint32_t cache = jni->ToGuestLocal(java_.NewString(U(cache_dir_ + "/")), &why);
  const std::uint32_t lang = jni->ToGuestLocal(java_.NewString(u"en"), &why);
  bool ok = cls != 0 && apk != 0 && files != 0 && cache != 0 && lang != 0;

  // GameManager.initFileSystem: sourceDir, save dir, cache dir, and a literal false (the dex's const/4 v6).
  if (ok) {
    ok = CallVoid(init_files_, "VIILLLZ",
                  {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(cls), rb::abi::Value32::Ptr(apk),
                   rb::abi::Value32::Ptr(files), rb::abi::Value32::Ptr(cache), rb::abi::Value32::S32(0)},
                  "InitFileManager", err);
  }
  // The sound manager: the silent OpenSL backend answers the object model. A false return falls back to
  // the Java sound manager, whose natives the same library carries.
  if (ok && init_opensl_ != 0) {
    rb::exec::RunResult rr{};
    const std::uint32_t assets = jni->ToGuestLocal(java_.NewInstance("android/content/res/AssetManager"), &why);
    const auto r = Call(init_opensl_, "VIIL",
                        {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(cls),
                         rb::abi::Value32::Ptr(assets)},
                        &rr);
    ok = rr.reason == rb::exec::StopReason::Returned;
    if (ok && r.i() == 0 && init_java_sound_ != 0) {
      ok = CallVoid(init_java_sound_, "VII",
                    {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(cls)}, "InitJavaSoundManager", err);
    }
  }
  if (ok) {
    ok = CallVoid(system_init_, "VIIIL",
                  {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(cls), rb::abi::Value32::S32(width),
                   rb::abi::Value32::S32(height), rb::abi::Value32::Ptr(lang)},
                  "SystemInit", err);
  }
  if (ok) {
    ok = CallVoid(game_init_, "VII", {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(cls)},
                  "GameInit", err);
  }
  jni->PopFrame(0);
  started_ = ok;
  if (ok) started_at_ = std::chrono::steady_clock::now();
  return ok;
}

bool HalfbrickBootstrap::Render(std::string* err) {
  if (!started_) {
    if (err != nullptr) *err = "halfbrick has not started";
    return false;
  }
  rb::abi::ForwardingJni* jni = boot_.jni();
  jni->PushFrame();
  std::string why;
  const std::uint32_t cls = jni->ToGuestLocal(java_.FindClass(kNativeClass), &why);
  rb::exec::RunResult rr{};
  const auto r = Call(step_, "VII", {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(cls)}, &rr);
  jni->PopFrame(0);
  if (rr.reason != rb::exec::StopReason::Returned) {
    if (err != nullptr) *err = "step stopped: " + rr.detail;
    return false;
  }
  if (r.i() == 0) {
    if (err != nullptr) *err = "step() returned false — the game requested shutdown or restart";
    return false;
  }
  return true;
}

bool HalfbrickBootstrap::Pause(std::string* err) {
  (void)on_focus_lost_;
  if (!started_ || on_pause_ == 0) return true;
  rb::abi::ForwardingJni* jni = boot_.jni();
  jni->PushFrame();
  std::string why;
  const std::uint32_t cls = jni->ToGuestLocal(java_.FindClass(kNativeClass), &why);
  const bool ok =
      CallVoid(on_pause_, "VII", {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(cls)}, "onPause", err);
  jni->PopFrame(0);
  return ok;
}

bool HalfbrickBootstrap::Resume(std::string* err) {
  (void)on_focus_retrieved_;
  if (!started_ || on_resume_ == 0) return true;
  rb::abi::ForwardingJni* jni = boot_.jni();
  jni->PushFrame();
  std::string why;
  const std::uint32_t cls = jni->ToGuestLocal(java_.FindClass(kNativeClass), &why);
  const bool ok =
      CallVoid(on_resume_, "VII", {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(cls)}, "onResume", err);
  jni->PopFrame(0);
  return ok;
}

void HalfbrickBootstrap::Touch(int id, int action, float x, float y) {
  if (!started_ || touch_ == 0) return;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started_at_)
                      .count();
  rb::abi::ForwardingJni* jni = boot_.jni();
  jni->PushFrame();
  std::string why;
  const std::uint32_t cls = jni->ToGuestLocal(java_.FindClass(kNativeClass), &why);
  if (cls != 0) {
    // (action, eventTime, pointerId, x, y, pressure, size), exactly what MotionEvent's getters feed.
    rb::exec::RunResult rr{};
    Call(touch_, "VIIJIFFFF",
         {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(cls), rb::abi::Value32::S32(action),
          rb::abi::Value32::I64(ms), rb::abi::Value32::S32(id), rb::abi::Value32::F32(x),
          rb::abi::Value32::F32(y), rb::abi::Value32::F32(1.0f), rb::abi::Value32::F32(0.0f)},
         &rr);
  }
  jni->PopFrame(0);
}

}  // namespace rb::platform::ios
