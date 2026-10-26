// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The Cocos2d-x bootstrap (S1): the calls the game's own Java layer makes, made directly on the guest's
// exported natives. Android's Cocos2dxHelper/Cocos2dxRenderer are Java; iOS has no VM, so the shell plays
// their part: set the APK path, initialise with the drawable's size, call nativeRender once per frame, and
// forward touches to the static natives.
//
// Every method runs on the render thread, with the EAGL context current and the layer set (EglShim::SetLayer).
// The entry points are the ones Blocky Roads' libgame.so exports (`llvm-nm`), and the same names every
// Cocos2d-x 3.x title exports.
#pragma once

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

class Cocos2dxBootstrap {
 public:
  Cocos2dxBootstrap(rb::host::Boot& boot, rb::java::TableJava& java, std::string lib);

  /// `nativeSetApkPath` then `nativeInit(width, height)`, inside one JNI local frame.
  bool Start(const std::string& apk_path, int width, int height, std::string* err);
  /// `nativeRender()` for one frame.
  bool Render(std::string* err);
  bool Pause(std::string* err);
  bool Resume(std::string* err);

  void TouchBegin(int id, float x, float y);
  void TouchEnd(int id, float x, float y);
  void TouchMove(int id, float x, float y);
  void TouchCancel(int id, float x, float y);

  [[nodiscard]] bool started() const { return started_; }

 private:
  std::uint32_t Export(const char* name, std::string* err) const;
  bool Call(std::uint32_t fn, const char* shorty, const std::vector<rb::abi::Value32>& args, const char* what,
            std::string* err);
  /// nativeTouchesMove/Cancel: `([I[F[FI)V`, one pointer.
  bool TouchArrays(const char* native, int id, float x, float y);

  rb::host::Boot& boot_;
  rb::java::TableJava& java_;
  std::string lib_;
  bool started_ = false;
};

}  // namespace rb::platform::ios
