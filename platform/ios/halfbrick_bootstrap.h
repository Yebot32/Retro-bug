// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Halfbrick's Mortar engine (Fruit Ninja vs. Skittles and the other Mortar titles). Its Java layer is an
// Activity plus a GLSurfaceView.Renderer that wraps exported natives in `NativeGameLib`; iOS has no VM, so
// the shell makes the calls that layer would make, in the order its dex shows:
//
//   GameManager.initFileSystem   NativeGameLib.InitFileManager(apk, files + "/", cache + "/", false)
//                                then InitOpenSLSoundManager(assets), or InitJavaSoundManager on false
//   GameManager.onSurfaceChanged NativeGameLib.SystemInit(width, height, language)
//   Renderer.onDrawFrame         NativeGameLib.GameInit() once, then NativeGameLib.step() per frame
//   MortarGameView.onTouchEvent  NativeGameLib.touchEvent(action, eventTime, pointerId, x, y, pressure, size)
//   Activity lifecycle           onPause / onResume / onFocusLost / onFocusRetrieved
//
// The natives receive no Activity, only paths and the AssetManager, so no Java class beyond the jclass
// argument has to exist.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "rb/abi/abi.h"

namespace rb::exec {
struct RunResult;
}
namespace rb::host {
class Boot;
}
namespace rb::java {
class TableJava;
}

namespace rb::platform::ios {

class HalfbrickBootstrap {
 public:
  HalfbrickBootstrap(rb::host::Boot& boot, rb::java::TableJava& java, std::string lib,
                     std::string files_dir, std::string cache_dir);

  bool Start(const std::string& apk_path, int width, int height, std::string* err);
  bool Render(std::string* err);
  bool Pause(std::string* err);
  bool Resume(std::string* err);
  /// `id` is the pointer id, `action` Android's MotionEvent action (down 0, up 1, move 2, cancel 3).
  void Touch(int id, int action, float x, float y);
  bool Back(std::string*) { return true; }

  [[nodiscard]] bool started() const { return started_; }

 private:
  std::uint32_t Export(const char* name, std::string* err) const;
  rb::abi::Ret32 Call(std::uint32_t fn, const char* shorty, const std::vector<rb::abi::Value32>& args,
                      rb::exec::RunResult* outcome);
  bool CallVoid(std::uint32_t fn, const char* shorty, const std::vector<rb::abi::Value32>& args,
                const char* what, std::string* err);

  rb::host::Boot& boot_;
  rb::java::TableJava& java_;
  std::string lib_;
  std::string files_dir_;
  std::string cache_dir_;
  std::uint32_t init_files_ = 0;
  std::uint32_t init_opensl_ = 0;
  std::uint32_t init_java_sound_ = 0;
  std::uint32_t system_init_ = 0;
  std::uint32_t game_init_ = 0;
  std::uint32_t step_ = 0;
  std::uint32_t touch_ = 0;
  std::uint32_t on_pause_ = 0;
  std::uint32_t on_resume_ = 0;
  std::uint32_t on_focus_lost_ = 0;
  std::uint32_t on_focus_retrieved_ = 0;
  std::chrono::steady_clock::time_point started_at_{};
  bool started_ = false;
};

}  // namespace rb::platform::ios
