/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The launcher's GL renderer: a real EGL context on the launcher's own Surface, and the guest's GLES1
 * calls forwarded to the device's driver instead of answered as counted no-ops.
 *
 * Two halves, both deliberately small:
 *
 *   - `EglTarget` owns the window, display, context and surface for the thread that drives the guest,
 *     so every forwarded call lands on a context current on that thread.
 *   - `GlForward` turns one guest call into one host call. Arguments come from where softfp AArch32 puts
 *     them (r0..r3, then the stack, floats as raw bits in core registers); a guest pointer becomes the
 *     host address `GuestMemory::Host` gives for it, which the in-place guest window makes contiguous.
 *     The host call goes through one function type with ten integer and eight float parameters: AAPCS64
 *     assigns integer-class and float arguments to x0..x7 and s0..s7 *independently, in order*, and
 *     overflows integers to the stack in order, so a GLES1 entry point with up to eight of each (plus
 *     stack integers, as glTexImage2D's ninth) receives exactly what its own prototype expects. That is an
 *     ABI fact relied on knowingly, not a C++ guarantee, and it is why the signature table below exists.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <android/native_window.h>
#include <EGL/egl.h>

#include <cstdint>
#include <string>

#include "rb/exec/machine_state.h"
#include "rb/mem/guest_memory.h"

namespace rb::launcher {

/// A GLES1 context on an ANativeWindow, made current on the constructing thread.
class EglTarget {
 public:
  /// Takes its own reference on `window`. `ok()` false, with `error()` naming the EGL step, otherwise.
  explicit EglTarget(ANativeWindow* window);
  ~EglTarget();
  EglTarget(const EglTarget&) = delete;
  EglTarget& operator=(const EglTarget&) = delete;

  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] const std::string& error() const { return error_; }
  [[nodiscard]] int width() const { return width_; }
  [[nodiscard]] int height() const { return height_; }
  /// Presents the frame. False (with `error()`) when the surface is gone, e.g. the screen was left.
  bool Swap();

 private:
  ANativeWindow* window_ = nullptr;
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLSurface surface_ = EGL_NO_SURFACE;
  int width_ = 0, height_ = 0;
  bool ok_ = false;
  std::string error_;
};

/// The guest→host GL call path.
class GlForward {
 public:
  /// Whether `name` has a known signature (and so can be forwarded rather than stubbed).
  static bool Known(const std::string& name);

  /// Forwards one guest call: reads its arguments from `s` (and the stack through `mem`), calls the host
  /// function, writes an integer result to r0. False, with `err`, for an unknown name, a host function the
  /// driver does not export, or a pointer argument that is not guest memory.
  bool Call(const std::string& name, exec::MachineState& s, const mem::GuestMemory& mem, std::string* err);

  [[nodiscard]] unsigned forwarded() const { return forwarded_; }

 private:
  unsigned forwarded_ = 0;
};

}  // namespace rb::launcher
