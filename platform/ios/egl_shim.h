// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// EGL on EAGL (S1). iOS has no libEGL: a game that asks for an EGL display/context/surface is answered
// here, with an `EAGLContext` on CAEAGLLayer instead of the device's EGL implementation. The GLES entry
// points themselves are not wrapped — they still go to OpenGLES.framework, on whatever context this makes
// current, which is exactly how the Android thunks compose with the device driver.
//
// One render thread per process (the GLSurfaceView shape every v1 title uses): the shim keeps one display,
// config, context and surface, and is not synchronized for concurrent guests.
#pragma once

#include <cstdint>
#include <string>

namespace rb::platform::ios {

class EglShim {
 public:
  EglShim();
  ~EglShim();
  EglShim(const EglShim&) = delete;
  EglShim& operator=(const EglShim&) = delete;

  static EglShim& Instance();

  /// The shell's drawable: the CAEAGLLayer the render thread presents into. Called on the render thread
  /// before any guest code runs. `client_version` is 1 or 2 (the guest's EGL_CONTEXT_CLIENT_VERSION).
  void SetLayer(void* ca_eagl_layer, int client_version);
  [[nodiscard]] bool has_layer() const { return layer_ != nullptr; }
  [[nodiscard]] int width() const { return width_; }
  [[nodiscard]] int height() const { return height_; }

  /// The host function for an `egl*` name the guest imports, or nullptr when this shim does not implement
  /// it. GL names are answered by `GlSymbol` (gl_symbols.mm), not here.
  static void* Symbol(const std::string& name);

  // What the guest's EGL calls reach. Public because the symbol table above hands them out as function
  // pointers; they are not meant to be called by platform code.
  void* GetDisplay();
  std::uint32_t Initialize(void* display, std::int32_t* major, std::int32_t* minor);
  std::uint32_t Terminate(void* display);
  std::uint32_t ChooseConfig(void* display, const std::int32_t* attribs, void** configs, std::int32_t config_size,
                             std::int32_t* num_config);
  std::uint32_t GetConfigAttrib(void* display, void* config, std::int32_t attrib, std::int32_t* value);
  void* CreateContext(void* display, void* config, void* share, const std::int32_t* attribs);
  void* CreateWindowSurface(void* display, void* config, void* native_window, const std::int32_t* attribs);
  std::uint32_t DestroyContext(void* display, void* context);
  std::uint32_t DestroySurface(void* display, void* surface);
  std::uint32_t MakeCurrent(void* display, void* draw, void* read, void* context);
  std::uint32_t SwapBuffers(void* display, void* surface);
  std::uint32_t QuerySurface(void* display, void* surface, std::int32_t attrib, std::int32_t* value);
  std::int32_t GetError();
  const char* QueryString(void* display, std::int32_t name);
  std::uint32_t SwapInterval(void* display, std::int32_t interval);
  void* GetProcAddress(const char* name);
  void* GetCurrentContext();
  void* GetCurrentSurface(std::int32_t read_draw);
  std::uint32_t BindApi(std::uint32_t api);
  std::uint32_t QueryApi();
  std::uint32_t ReleaseThread();
  std::uint32_t WaitClient();
  void* GetCurrentDisplay();

  /// Makes the context current on the calling (render) thread and builds the drawable. The shell calls this
  /// before any guest code; MakeCurrent() below is the guest-visible EGL function.
  bool Begin(std::string* err);
  /// Presents the drawable (the shell calls it after the guest render entry; the guest never swaps).
  bool Present(std::string* err);

  /// Drops the context and layer (the shim's destructor does this too). Called on the render thread.
  void Detach();

 private:
  bool EnsureDrawable(std::string* err);

  void* layer_ = nullptr;
  void* context_ = nullptr;  ///< EAGLContext*, retained
  int client_version_ = 1;
  int width_ = 0, height_ = 0;
  std::uint32_t framebuffer_ = 0, color_buffer_ = 0;
  bool drawable_ready_ = false;
  std::int32_t swap_interval_ = 1;
  std::int32_t error_ = 0;  ///< 0x3000 (EGL_SUCCESS)
};

}  // namespace rb::platform::ios
