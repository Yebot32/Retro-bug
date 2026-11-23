// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// KitKat Challenge's DLGE engine (2D, GLES1). Its Java layer is a GLSurfaceView.Renderer whose natives
// are registered in JNI_OnLoad; seven calls are the whole contract between the game and Android, so the
// shell makes them directly: the AssetManager, init, the surface size, a draw per frame, touch, pause and
// back. No Java text, no engine-specific dialogue — the simplest title in the v1 set.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "rb/java/table_java.h"

namespace rb::platform::ios {

class DlglBootstrap {
 public:
  /// `jni_env` is the guest JNIEnv* value (`Boot::jni()->env()`), not a host pointer: the natives
  /// pass it straight to their JNI calls, starting with GetStringUTFChars in dlxSurfaceChanged.
  DlglBootstrap(rb::java::TableJava& java, std::uint32_t jni_env, std::string lib,
                std::function<void(const std::string&)> log);

  /// Resolves the registered natives and makes the Java layer's start-up calls. On the render thread,
  /// with the GLES1 context current.
  bool Start(const std::string& apk, int width, int height, std::string* err);
  bool Render(std::string* err);
  bool Pause(std::string* err);
  bool Resume(std::string*) { return true; }
  bool Back(std::string* err);
  /// `action` is Android's MotionEvent action (0 down, 1 up, 2 move, 3 cancel), coordinates in pixels.
  void Touch(int action, float x, float y);
  [[nodiscard]] bool started() const { return started_; }

 private:
  void* Native(const char* cls, const char* name, std::string* err) const;
  void* Env() const;
  /// Logs (and consumes) any GL error our calls left, naming the step. Diagnostics only.
  void Drain(const char* after);

  rb::java::TableJava& java_;
  std::uint32_t jni_env_ = 0;
  std::string lib_;
  std::function<void(const std::string&)> log_;
  void* asset_ = nullptr;
  void* init_ = nullptr;
  void* surface_ = nullptr;
  void* draw_ = nullptr;
  void* touch_ = nullptr;
  void* pause_ = nullptr;
  void* back_ = nullptr;
  bool started_ = false;
};

}  // namespace rb::platform::ios
