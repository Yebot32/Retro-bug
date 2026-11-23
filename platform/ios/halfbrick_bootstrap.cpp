// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See halfbrick_bootstrap.h.

#include "halfbrick_bootstrap.h"

#include "gl_symbols.h"

#include "rb/abi/jni_forward.h"
#include "rb/host/boot.h"
#include "rb/java/table_java.h"
#include "rb/runtime/guest_process.h"

namespace rb::platform::ios {

namespace {

constexpr const char* kNativeClass = "com/halfbrick/mortar/NativeGameLib";
constexpr const char* kPrefix = "Java_com_halfbrick_mortar_NativeGameLib_native_1";

std::u16string U(const std::string& s) { return std::u16string(s.begin(), s.end()); }

/// The GL state the game's Java layer sets before the engine draws. GameManager.SystemInit disables the
/// four client states when the engine is not GLES1, turns dithering and face culling off and sets the
/// front face; GameManager.Render re-enables culling and the depth test before every step. The constants
/// are the game's own (from its dex): GL_CCW 2305, GL_CULL_FACE 2884, GL_BACK 1029, GL_DEPTH_TEST 2929,
/// GL_DITHER 3042, and the client states 0x8078, 0x8076, 0x8074, 0x8075.
void CallGl(const char* name, int a) {
  using Fn = void (*)(int);
  const auto fn = reinterpret_cast<Fn>(GlSymbol(name));
  if (fn != nullptr) fn(a);
}

void CallGlViewport(int x, int y, int width, int height) {
  using Fn = void (*)(int, int, int, int);
  const auto fn = reinterpret_cast<Fn>(GlSymbol("glViewport"));
  if (fn != nullptr) fn(x, y, width, height);
}

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
  gles_version_ = Export((std::string{kPrefix} + "GLESVersion").c_str(), nullptr);

  // The engine's own GLES version decides whether the client-state disables happen (the Java skips them
  // for a GLES1 engine). It is asked before the first frame, exactly where the Java asks it.
  int gles_version = 2;
  if (gles_version_ != 0) {
    rb::abi::ForwardingJni* version_jni = boot_.jni();
    version_jni->PushFrame();
    std::string version_why;
    const std::uint32_t cls = version_jni->ToGuestLocal(java_.FindClass(kNativeClass), &version_why);
    if (cls != 0) {
      rb::exec::RunResult rr{};
      const auto r = Call(gles_version_, "VII", {rb::abi::Value32::Ptr(version_jni->env()),
                                                 rb::abi::Value32::Ptr(cls)}, &rr);
      if (rr.reason == rb::exec::StopReason::Returned) gles_version = r.i();
    }
    version_jni->PopFrame(0);
  }
  if (gles_version != 1) {
    for (const int state : {0x8078, 0x8076, 0x8074, 0x8075}) CallGl("glDisableClientState", state);
  }
  CallGl("glFrontFace", 2305);  // GL_CCW
  CallGl("glDisable", 3042);    // GL_DITHER
  CallGl("glDisable", 2884);    // GL_CULL_FACE
  width_ = width;
  height_ = height;
  CallGlViewport(0, 0, width, height);

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
    ok = CallVoid(system_init_, "VIIIIL",
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
  // The state the Java sets before every frame: front face, face culling and the depth test.
  CallGl("glFrontFace", 2305);  // GL_CCW
  CallGl("glEnable", 2884);     // GL_CULL_FACE
  CallGl("glCullFace", 1029);   // GL_BACK
  CallGl("glEnable", 2929);     // GL_DEPTH_TEST
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
  (void)id;
  if (!started_ || touch_ == 0) return;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started_at_)
                      .count();
  // The Java normalises the point by the display size and passes a literal zero for the third int:
  // touchEvent(action, eventTime, 0, x / width, y / height, pressure, size).
  const float nx = width_ > 0 ? x / static_cast<float>(width_) : x;
  const float ny = height_ > 0 ? y / static_cast<float>(height_) : y;
  rb::abi::ForwardingJni* jni = boot_.jni();
  jni->PushFrame();
  std::string why;
  const std::uint32_t cls = jni->ToGuestLocal(java_.FindClass(kNativeClass), &why);
  if (cls != 0) {
    rb::exec::RunResult rr{};
    Call(touch_, "VIIJIFFFF",
         {rb::abi::Value32::Ptr(jni->env()), rb::abi::Value32::Ptr(cls), rb::abi::Value32::S32(action),
          rb::abi::Value32::I64(ms), rb::abi::Value32::S32(0), rb::abi::Value32::F32(nx),
          rb::abi::Value32::F32(ny), rb::abi::Value32::F32(1.0f), rb::abi::Value32::F32(0.0f)},
         &rr);
  }
  jni->PopFrame(0);
}

}  // namespace rb::platform::ios
