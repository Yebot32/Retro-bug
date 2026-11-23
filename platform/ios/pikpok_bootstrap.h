// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// PikPok's Mab engine (OREO: Twist, Lick, Dunk; Turbo). Android's Java layer is an Activity and a
// GLSurfaceView.Renderer; iOS has no VM, so the shell makes the calls that layer would make, on the
// exported Java_* natives the game library carries (it registers none in JNI_OnLoad):
//
//   MabActivity.initApplication(String[])       onCreate, before the surface exists
//   MabRenderer.nativeApplicationResumed()      onResume
//   MabRenderer.nativeOnGraphicsContextAquired() onSurfaceCreated
//   MabRenderer.nativeResize(w, h)              onSurfaceChanged
//   MabRenderer.nativeInitGraphics(w, h)        the first onDrawFrame
//   MabRenderer.nativeRender()                  every onDrawFrame
//   MabInput.nativeTouch{Down,Move,Up,Cancel}(id, x, y, time)
//
// The native side still calls the Activity's context methods through JNI, which is why the shell defines
// com/pikpok/MabActivity with an Activity superclass before any library loads (`InstallPikPok`).
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace rb::abi {
struct Value32;
}
namespace rb::host {
class Boot;
}
namespace rb::java {
class TableJava;
}

namespace rb::platform::ios {

class PikPokBootstrap {
 public:
  PikPokBootstrap(rb::host::Boot& boot, rb::java::TableJava& java, std::string lib);

  bool Start(const std::string& apk_path, int width, int height, std::string* err);
  bool Render(std::string* err);
  bool Pause(std::string* err);
  bool Resume(std::string* err);
  /// `id` is the pointer the Java `MabInput.Touch` would carry, `action` Android's MotionEvent action.
  void Touch(int id, int action, float x, float y);
  bool Back(std::string* err);

  [[nodiscard]] bool started() const { return started_; }

 private:
  std::uint32_t Export(const char* name, std::string* err) const;
  bool Call(std::uint32_t fn, const char* shorty, const std::vector<rb::abi::Value32>& args, const char* what,
            std::string* err);

  rb::host::Boot& boot_;
  rb::java::TableJava& java_;
  std::string lib_;
  std::uint32_t init_ = 0;
  std::uint32_t acquired_ = 0;
  std::uint32_t init_graphics_ = 0;
  std::uint32_t resize_ = 0;
  std::uint32_t render_ = 0;
  std::uint32_t paused_ = 0;
  std::uint32_t resumed_ = 0;
  std::uint32_t back_ = 0;
  std::uint32_t touch_down_ = 0;
  std::uint32_t touch_move_ = 0;
  std::uint32_t touch_up_ = 0;
  std::uint32_t touch_cancel_ = 0;
  /// The Java layer stamps each touch with `MotionEvent.getEventTime() / 1000.0`; the engine derives its
  /// input timing from consecutive values, so they must advance.
  std::chrono::steady_clock::time_point started_at_{};
  bool started_ = false;
};

}  // namespace rb::platform::ios
