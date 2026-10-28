// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// EGL on EAGL: see egl_shim.h. One render thread; the shell calls SetLayer() with its CAEAGLLayer before
// any guest code runs, and the guest's eglInitialize/ChooseConfig/CreateContext/MakeCurrent then build an
// EAGLContext and a framebuffer over that layer.

#include "egl_shim.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>

#import <Foundation/Foundation.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES1/gl.h>
#import <OpenGLES/ES1/glext.h>
#import <QuartzCore/QuartzCore.h>

#include <cstdio>
#include <cstring>
#include <map>

#include "gl_symbols.h"

// ES2's framebuffer entry points have the same names without the OES suffix and different prototypes in no
// way that matters here; ES1/glext.h already declared the OES set, and ES2/gl.h cannot be included in the
// same unit (both headers share a guard), so the core names are declared where they are used.
extern "C" {
void glGenFramebuffers(GLsizei n, GLuint* framebuffers);
void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
void glGenRenderbuffers(GLsizei n, GLuint* renderbuffers);
void glBindRenderbuffer(GLenum target, GLuint renderbuffer);
void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint* params);
GLenum glCheckFramebufferStatus(GLenum target);
}

namespace rb::platform::ios {

namespace {

// EGL 1.4 names this shim uses (the numbers are the Khronos values; iOS has no egl.h to take them from).
constexpr std::int32_t kSuccess = 0x3000;
constexpr std::int32_t kNotInitialized = 0x3001;
constexpr std::int32_t kBadAccess = 0x3002;
constexpr std::int32_t kBadAlloc = 0x3003;
constexpr std::int32_t kBadAttribute = 0x3004;
constexpr std::int32_t kBadConfig = 0x3005;
constexpr std::int32_t kBadContext = 0x3006;
constexpr std::int32_t kBadDisplay = 0x3008;
constexpr std::int32_t kBadMatch = 0x3009;
constexpr std::int32_t kBadNativeWindow = 0x300B;
constexpr std::int32_t kBadParameter = 0x300C;
constexpr std::int32_t kBadSurface = 0x300D;

constexpr std::int32_t kAlphaSize = 0x3021;
constexpr std::int32_t kBlueSize = 0x3022;
constexpr std::int32_t kGreenSize = 0x3023;
constexpr std::int32_t kRedSize = 0x3024;
constexpr std::int32_t kDepthSize = 0x3025;
constexpr std::int32_t kStencilSize = 0x3026;
constexpr std::int32_t kConfigCaveat = 0x3027;
constexpr std::int32_t kConfigId = 0x3028;
constexpr std::int32_t kNativeVisualId = 0x302E;
constexpr std::int32_t kNativeVisualType = 0x302F;
constexpr std::int32_t kSamples = 0x3031;
constexpr std::int32_t kSampleBuffers = 0x3032;
constexpr std::int32_t kSurfaceType = 0x3033;
constexpr std::int32_t kNone = 0x3038;
constexpr std::int32_t kRenderableType = 0x3040;
constexpr std::int32_t kConformant = 0x3042;
constexpr std::int32_t kSlowConfig = 0x3050;
constexpr std::int32_t kNonConformantConfig = 0x3051;
constexpr std::int32_t kVendor = 0x3053;
constexpr std::int32_t kVersion = 0x3054;
constexpr std::int32_t kExtensions = 0x3055;
constexpr std::int32_t kHeight = 0x3056;
constexpr std::int32_t kWidth = 0x3057;
constexpr std::int32_t kClientApis = 0x308D;
constexpr std::int32_t kContextClientVersion = 0x3098;
constexpr std::int32_t kWindowBit = 0x0004;
constexpr std::int32_t kEs1Bit = 0x0001;
constexpr std::int32_t kEs2Bit = 0x0004;
constexpr std::uint32_t kOpenGlEsApi = 0x30A0;

// Distinct non-null object identities. The guest only passes them back; nothing dereferences them.
int g_display_marker = 0;
int g_config_marker = 0;
int g_surface_marker = 0;
int g_pbuffer_marker = 0;

int ConfigVersion(const std::int32_t* attribs) {
  int version = 1;
  if (attribs == nullptr) return version;
  for (int i = 0; attribs[i] != kNone; i += 2) {
    if (attribs[i] == kRenderableType && (attribs[i + 1] & kEs2Bit) != 0) version = 2;
  }
  return version;
}

int ContextVersion(const std::int32_t* attribs, int fallback) {
  if (attribs == nullptr) return fallback;
  for (int i = 0; attribs[i] != kNone; i += 2) {
    if (attribs[i] == kContextClientVersion) return attribs[i + 1] == 2 ? 2 : 1;
  }
  return fallback;
}

// The table rb::platform::ios::EglShim::Symbol hands the host-slot layer. Each entry has the EGL prototype
// the guest's import expects.
void* EglGetDisplay(void*) { return EglShim::Instance().GetDisplay(); }
std::uint32_t EglInitialize(void* d, std::int32_t* major, std::int32_t* minor) {
  return EglShim::Instance().Initialize(d, major, minor);
}
std::uint32_t EglTerminate(void* d) { return EglShim::Instance().Terminate(d); }
std::uint32_t EglChooseConfig(void* d, const std::int32_t* a, void** c, std::int32_t n, std::int32_t* num) {
  return EglShim::Instance().ChooseConfig(d, a, c, n, num);
}
std::uint32_t EglGetConfigAttrib(void* d, void* c, std::int32_t a, std::int32_t* v) {
  return EglShim::Instance().GetConfigAttrib(d, c, a, v);
}
void* EglCreateContext(void* d, void* c, void* s, const std::int32_t* a) {
  return EglShim::Instance().CreateContext(d, c, s, a);
}
void* EglCreateWindowSurface(void* d, void* c, void* w, const std::int32_t* a) {
  return EglShim::Instance().CreateWindowSurface(d, c, w, a);
}
void* EglCreatePbufferSurface(void* d, void* c, const std::int32_t* a) {
  (void)d;
  (void)c;
  (void)a;
  return &g_pbuffer_marker;
}
std::uint32_t EglDestroyContext(void* d, void* c) { return EglShim::Instance().DestroyContext(d, c); }
std::uint32_t EglDestroySurface(void* d, void* s) { return EglShim::Instance().DestroySurface(d, s); }
std::uint32_t EglMakeCurrent(void* d, void* draw, void* read, void* c) {
  return EglShim::Instance().MakeCurrent(d, draw, read, c);
}
std::uint32_t EglSwapBuffers(void* d, void* s) { return EglShim::Instance().SwapBuffers(d, s); }
std::uint32_t EglQuerySurface(void* d, void* s, std::int32_t a, std::int32_t* v) {
  return EglShim::Instance().QuerySurface(d, s, a, v);
}
std::int32_t EglGetError() { return EglShim::Instance().GetError(); }
const char* EglQueryString(void* d, std::int32_t n) { return EglShim::Instance().QueryString(d, n); }
std::uint32_t EglSwapInterval(void* d, std::int32_t i) { return EglShim::Instance().SwapInterval(d, i); }
void* EglGetProcAddress(const char* n) { return EglShim::Instance().GetProcAddress(n); }
void* EglGetCurrentContext() { return EglShim::Instance().GetCurrentContext(); }
void* EglGetCurrentSurface(std::int32_t r) { return EglShim::Instance().GetCurrentSurface(r); }
std::uint32_t EglBindApi(std::uint32_t a) { return EglShim::Instance().BindApi(a); }
std::uint32_t EglQueryApi() { return EglShim::Instance().QueryApi(); }
std::uint32_t EglReleaseThread() { return EglShim::Instance().ReleaseThread(); }
std::uint32_t EglWaitClient() { return EglShim::Instance().WaitClient(); }
void* EglGetCurrentDisplay() { return EglShim::Instance().GetCurrentDisplay(); }
std::uint32_t EglGetConfigs(void* d, void** c, std::int32_t n, std::int32_t* num) {
  (void)d;
  if (num != nullptr) *num = 1;
  if (c == nullptr) return 1;
  if (n < 1) return 0;
  c[0] = &g_config_marker;
  return 1;
}

const std::map<std::string, void*>& Symbols() {
  static const std::map<std::string, void*> k = {
      {"eglGetDisplay", reinterpret_cast<void*>(&EglGetDisplay)},
      {"eglInitialize", reinterpret_cast<void*>(&EglInitialize)},
      {"eglTerminate", reinterpret_cast<void*>(&EglTerminate)},
      {"eglChooseConfig", reinterpret_cast<void*>(&EglChooseConfig)},
      {"eglGetConfigs", reinterpret_cast<void*>(&EglGetConfigs)},
      {"eglGetConfigAttrib", reinterpret_cast<void*>(&EglGetConfigAttrib)},
      {"eglCreateContext", reinterpret_cast<void*>(&EglCreateContext)},
      {"eglCreateWindowSurface", reinterpret_cast<void*>(&EglCreateWindowSurface)},
      {"eglCreatePbufferSurface", reinterpret_cast<void*>(&EglCreatePbufferSurface)},
      {"eglDestroyContext", reinterpret_cast<void*>(&EglDestroyContext)},
      {"eglDestroySurface", reinterpret_cast<void*>(&EglDestroySurface)},
      {"eglMakeCurrent", reinterpret_cast<void*>(&EglMakeCurrent)},
      {"eglSwapBuffers", reinterpret_cast<void*>(&EglSwapBuffers)},
      {"eglQuerySurface", reinterpret_cast<void*>(&EglQuerySurface)},
      {"eglGetError", reinterpret_cast<void*>(&EglGetError)},
      {"eglQueryString", reinterpret_cast<void*>(&EglQueryString)},
      {"eglSwapInterval", reinterpret_cast<void*>(&EglSwapInterval)},
      {"eglGetProcAddress", reinterpret_cast<void*>(&EglGetProcAddress)},
      {"eglGetCurrentContext", reinterpret_cast<void*>(&EglGetCurrentContext)},
      {"eglGetCurrentSurface", reinterpret_cast<void*>(&EglGetCurrentSurface)},
      {"eglBindAPI", reinterpret_cast<void*>(&EglBindApi)},
      {"eglQueryAPI", reinterpret_cast<void*>(&EglQueryApi)},
      {"eglReleaseThread", reinterpret_cast<void*>(&EglReleaseThread)},
      {"eglWaitClient", reinterpret_cast<void*>(&EglWaitClient)},
      {"eglWaitGL", reinterpret_cast<void*>(&EglWaitClient)},
      {"eglGetCurrentDisplay", reinterpret_cast<void*>(&EglGetCurrentDisplay)},
  };
  return k;
}

}  // namespace

EglShim& EglShim::Instance() {
  static EglShim* shim = new EglShim;
  return *shim;
}

EglShim::EglShim() = default;

EglShim::~EglShim() { Detach(); }

bool EglShim::Begin(std::string* err) {
  if (context_ == nullptr) {
    if (CreateContext(&g_display_marker, &g_config_marker, nullptr, nullptr) == nullptr) {
      if (err != nullptr) *err = "EAGLContext creation failed";
      return false;
    }
  }
  return EnsureDrawable(err);
}

bool EglShim::Present(std::string* err) {
  if (SwapBuffers(&g_display_marker, &g_surface_marker) != 0) return true;
  if (err != nullptr) *err = "eglSwapBuffers failed";
  return false;
}

void EglShim::SamplePixels(std::string* out) {
  out->clear();
  if (!drawable_ready_ || width_ <= 0 || height_ <= 0) return;
  if (client_version_ == 1) {
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, framebuffer_);
  } else {
    glBindFramebuffer(0x8D40, framebuffer_);
  }
  char line[384];
  std::size_t used = 0;
  for (int gy = 1; gy <= 3; ++gy) {
    for (int gx = 1; gx <= 3; ++gx) {
      unsigned char rgba[4] = {};
      const int x = (width_ * gx) / 4;
      const int y = (height_ * gy) / 4;
      glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
      const int n = std::snprintf(line + used, sizeof line - used, "%s%d,%d=%02x%02x%02x%02x",
                                  used == 0 ? "" : " ", x, y, rgba[0], rgba[1], rgba[2], rgba[3]);
      if (n <= 0 || static_cast<std::size_t>(n) >= sizeof line - used) {
        out->assign(line, used);
        return;
      }
      used += static_cast<std::size_t>(n);
    }
  }
  out->assign(line, used);
}

void EglShim::Detach() {
  if (drawable_ready_) {
    if (context_ != nullptr) [EAGLContext setCurrentContext:nil];
    drawable_ready_ = false;
  }
  if (context_ != nullptr) {
    CFRelease(static_cast<CFTypeRef>(context_));
    context_ = nullptr;
  }
  if (layer_ != nullptr) {
    CFRelease(static_cast<CFTypeRef>(layer_));
    layer_ = nullptr;
  }
  framebuffer_ = color_buffer_ = 0;
  width_ = height_ = 0;
}

void EglShim::SetLayer(void* ca_eagl_layer, int client_version) {
  if (layer_ != nullptr && layer_ != ca_eagl_layer) {
    CFRelease(static_cast<CFTypeRef>(layer_));
    layer_ = nullptr;
    drawable_ready_ = false;
  }
  if (ca_eagl_layer != nullptr && layer_ != ca_eagl_layer) {
    layer_ = const_cast<void*>(CFBridgingRetain((__bridge id)ca_eagl_layer));
  }
  // A restart after a resize or a stop keeps the same layer but needs the drawable rebuilt.
  if (ca_eagl_layer != nullptr) drawable_ready_ = false;
  if (client_version == 1 || client_version == 2) client_version_ = client_version;
}

void* EglShim::Symbol(const std::string& name) {
  const auto it = Symbols().find(name);
  return it == Symbols().end() ? nullptr : it->second;
}

void* EglShim::GetDisplay() { return &g_display_marker; }

std::uint32_t EglShim::Initialize(void* display, std::int32_t* major, std::int32_t* minor) {
  if (display == nullptr) {
    error_ = kBadDisplay;
    return 0;
  }
  if (major != nullptr) *major = 1;
  if (minor != nullptr) *minor = 4;
  return 1;
}

std::uint32_t EglShim::Terminate(void* display) {
  (void)display;
  return 1;
}

std::uint32_t EglShim::ChooseConfig(void* display, const std::int32_t* attribs, void** configs,
                                    std::int32_t config_size, std::int32_t* num_config) {
  if (display == nullptr) {
    error_ = kBadDisplay;
    return 0;
  }
  for (int i = 0; attribs != nullptr && attribs[i] != kNone; i += 2) {
    // The two attributes this shim cannot satisfy: a non-window surface and an unknown renderable type.
    if (attribs[i] == kSurfaceType && (attribs[i + 1] & kWindowBit) == 0) {
      if (num_config != nullptr) *num_config = 0;
      error_ = kBadMatch;
      return 1;  // "no config matched" is a successful query with zero results, as in EGL
    }
    if (attribs[i] == kRenderableType && (attribs[i + 1] & (kEs1Bit | kEs2Bit)) == 0) {
      if (num_config != nullptr) *num_config = 0;
      return 1;
    }
  }
  client_version_ = ConfigVersion(attribs);
  if (num_config != nullptr) *num_config = 1;
  if (configs != nullptr && config_size > 0) configs[0] = &g_config_marker;
  return 1;
}

std::uint32_t EglShim::GetConfigAttrib(void* display, void* config, std::int32_t attrib, std::int32_t* value) {
  if (display == nullptr || config == nullptr || value == nullptr) {
    error_ = kBadParameter;
    return 0;
  }
  switch (attrib) {
    case kRedSize:
    case kGreenSize:
    case kBlueSize: *value = 8; break;
    case kAlphaSize: *value = 8; break;
    case kDepthSize: *value = 16; break;
    case kStencilSize:
    case kSamples:
    case kSampleBuffers: *value = 0; break;
    case kSurfaceType: *value = kWindowBit; break;
    case kRenderableType: *value = kEs1Bit | kEs2Bit; break;
    case kConformant: *value = kEs1Bit | kEs2Bit; break;
    case kConfigCaveat: *value = kNone; break;
    case kConfigId: *value = 1; break;
    case kNativeVisualId:
    case kNativeVisualType: *value = 0; break;
    case kSlowConfig:
    case kNonConformantConfig: *value = 0; break;
    default:
      error_ = kBadAttribute;
      return 0;
  }
  return 1;
}

void* EglShim::CreateContext(void* display, void* config, void* share, const std::int32_t* attribs) {
  (void)share;
  if (display == nullptr) {
    error_ = kBadDisplay;
    return nullptr;
  }
  if (config == nullptr) {
    error_ = kBadConfig;
    return nullptr;
  }
  const int version = ContextVersion(attribs, client_version_);
  if (context_ != nullptr) {
    if (version == client_version_) return context_;
    [EAGLContext setCurrentContext:nil];
    CFRelease(static_cast<CFTypeRef>(context_));
    context_ = nullptr;
  }
  EAGLContext* ctx = [[EAGLContext alloc]
      initWithAPI:(version == 1 ? kEAGLRenderingAPIOpenGLES1 : kEAGLRenderingAPIOpenGLES2)];
  if (ctx == nil) {
    error_ = kBadAlloc;
    return nullptr;
  }
  client_version_ = version;
  context_ = const_cast<void*>(CFBridgingRetain(ctx));
  return context_;
}

void* EglShim::CreateWindowSurface(void* display, void* config, void* native_window, const std::int32_t* attribs) {
  (void)native_window;  // the shell's layer is the drawable; see SetLayer
  (void)attribs;
  if (display == nullptr) {
    error_ = kBadDisplay;
    return nullptr;
  }
  if (config == nullptr) {
    error_ = kBadConfig;
    return nullptr;
  }
  return &g_surface_marker;
}

std::uint32_t EglShim::DestroyContext(void* display, void* context) {
  (void)display;
  if (context != nullptr && context == context_) {
    [EAGLContext setCurrentContext:nil];
    CFRelease(static_cast<CFTypeRef>(context_));
    context_ = nullptr;
    drawable_ready_ = false;
  }
  return 1;
}

std::uint32_t EglShim::DestroySurface(void* display, void* surface) {
  (void)display;
  (void)surface;
  return 1;
}

bool EglShim::EnsureDrawable(std::string* err) {
  if (layer_ == nullptr) {
    if (err != nullptr) *err = "no CAEAGLLayer was set (EglShim::SetLayer)";
    error_ = kBadNativeWindow;
    return false;
  }
  if (context_ == nullptr) {
    if (CreateContext(&g_display_marker, &g_config_marker, nullptr, nullptr) == nullptr) {
      if (err != nullptr) *err = "EAGLContext creation failed";
      return false;
    }
  }
  CAEAGLLayer* layer = (__bridge CAEAGLLayer*)layer_;
  EAGLContext* ctx = (__bridge EAGLContext*)context_;
  if (![EAGLContext setCurrentContext:ctx]) {
    if (err != nullptr) *err = "EAGLContext could not be made current";
    error_ = kBadAccess;
    return false;
  }
  if (!drawable_ready_) {
    if (client_version_ == 1) {
      glGenFramebuffersOES(1, &framebuffer_);
      glBindFramebufferOES(GL_FRAMEBUFFER_OES, framebuffer_);
      glGenRenderbuffersOES(1, &color_buffer_);
      glBindRenderbufferOES(GL_RENDERBUFFER_OES, color_buffer_);
      if (![ctx renderbufferStorage:GL_RENDERBUFFER_OES fromDrawable:layer]) {
        if (err != nullptr) *err = "renderbufferStorage from the layer failed";
        error_ = kBadAlloc;
        return false;
      }
      glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_RENDERBUFFER_OES, color_buffer_);
      glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES, GL_RENDERBUFFER_WIDTH_OES, &width_);
      glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES, GL_RENDERBUFFER_HEIGHT_OES, &height_);
      // The config promises a 16-bit depth buffer (eglGetConfigAttrib); without one the game's depth test
      // is undefined. Then bind the colour buffer back: presentRenderbuffer presents the bound one.
      glGenRenderbuffersOES(1, &depth_buffer_);
      glBindRenderbufferOES(GL_RENDERBUFFER_OES, depth_buffer_);
      glRenderbufferStorageOES(GL_RENDERBUFFER_OES, GL_DEPTH_COMPONENT16_OES, width_, height_);
      glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_DEPTH_ATTACHMENT_OES, GL_RENDERBUFFER_OES, depth_buffer_);
      // Bind the colour buffer back: presentRenderbuffer presents the bound one.
      glBindRenderbufferOES(GL_RENDERBUFFER_OES, color_buffer_);
      if (glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES) != GL_FRAMEBUFFER_COMPLETE_OES) {
        if (err != nullptr) *err = "the EAGL framebuffer is incomplete";
        error_ = kBadSurface;
        return false;
      }
    } else {
      glGenFramebuffers(1, &framebuffer_);
      glBindFramebuffer(0x8D40, framebuffer_);  // GL_FRAMEBUFFER
      glGenRenderbuffers(1, &color_buffer_);
      glBindRenderbuffer(0x8D41, color_buffer_);  // GL_RENDERBUFFER
      if (![ctx renderbufferStorage:0x8D41 fromDrawable:layer]) {
        if (err != nullptr) *err = "renderbufferStorage from the layer failed";
        error_ = kBadAlloc;
        return false;
      }
      glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, color_buffer_);  // FRAMEBUFFER, COLOR_ATTACHMENT0
      glGetRenderbufferParameteriv(0x8D41, 0x8D42, &width_);             // RENDERBUFFER_WIDTH
      glGetRenderbufferParameteriv(0x8D41, 0x8D43, &height_);            // RENDERBUFFER_HEIGHT
      glGenRenderbuffers(1, &depth_buffer_);
      glBindRenderbuffer(0x8D41, depth_buffer_);
      glRenderbufferStorage(0x8D41, 0x81A5, width_, height_);              // DEPTH_COMPONENT16
      glFramebufferRenderbuffer(0x8D40, 0x8D00, 0x8D41, depth_buffer_);    // DEPTH_ATTACHMENT
      glBindRenderbuffer(0x8D41, color_buffer_);
      if (glCheckFramebufferStatus(0x8D40) != 0x8CD5) {                  // GL_FRAMEBUFFER_COMPLETE
        if (err != nullptr) *err = "the EAGL framebuffer is incomplete";
        error_ = kBadSurface;
        return false;
      }
    }
    drawable_ready_ = true;
  }
  return true;
}

bool EglShim::BindDrawable(std::string* err) {
  std::string why;
  if (!EnsureDrawable(&why)) {
    if (err != nullptr) *err = why;
    return false;
  }
  if (client_version_ == 1) {
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, framebuffer_);
  } else {
    glBindFramebuffer(0x8D40, framebuffer_);
  }
  return true;
}

std::uint32_t EglShim::MakeCurrent(void* display, void* draw, void* read, void* context) {
  (void)read;
  if (display == nullptr) {
    error_ = kBadDisplay;
    return 0;
  }
  if (context == nullptr && draw == nullptr) {  // the guest is releasing the current context
    [EAGLContext setCurrentContext:nil];
    return 1;
  }
  if (context == nullptr || draw == nullptr) {
    [EAGLContext setCurrentContext:nil];
    error_ = kBadContext;
    return 0;
  }
  if (context != context_) {
    error_ = kBadContext;
    return 0;
  }
  std::string err;
  if (!EnsureDrawable(&err)) return 0;
  return 1;
}

std::uint32_t EglShim::SwapBuffers(void* display, void* surface) {
  if (display == nullptr || surface == nullptr) {
    error_ = kBadSurface;
    return 0;
  }
  std::string err;
  if (!EnsureDrawable(&err)) return 0;
  EAGLContext* ctx = (__bridge EAGLContext*)context_;
  if (client_version_ == 1) {
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, color_buffer_);
  } else {
    glBindRenderbuffer(0x8D41, color_buffer_);
  }
  if (![ctx presentRenderbuffer:0x8D41]) {  // GL_RENDERBUFFER
    error_ = kBadSurface;
    return 0;
  }
  return 1;
}

std::uint32_t EglShim::QuerySurface(void* display, void* surface, std::int32_t attrib, std::int32_t* value) {
  if (display == nullptr || surface == nullptr || value == nullptr) {
    error_ = kBadParameter;
    return 0;
  }
  switch (attrib) {
    case kWidth: *value = width_; return 1;
    case kHeight: *value = height_; return 1;
    default: error_ = kBadAttribute; return 0;
  }
}

std::int32_t EglShim::GetError() {
  const std::int32_t e = error_;
  error_ = kSuccess;
  return e;
}

const char* EglShim::QueryString(void* display, std::int32_t name) {
  (void)display;
  switch (name) {
    case kVendor: return "RetroBug";
    case kVersion: return "1.4 RetroBug-EAGL";
    case kExtensions: return "";
    case kClientApis: return "OpenGL_ES";
    default:
      error_ = kBadParameter;
      return nullptr;
  }
}

std::uint32_t EglShim::SwapInterval(void* display, std::int32_t interval) {
  (void)display;
  swap_interval_ = interval;
  return 1;
}

void* EglShim::GetProcAddress(const char* name) {
  if (name == nullptr) {
    error_ = kBadParameter;
    return nullptr;
  }
  if (void* egl = Symbol(name)) return egl;
  return GlSymbol(name);
}

void* EglShim::GetCurrentContext() {
  EAGLContext* current = [EAGLContext currentContext];
  if (current == nil || context_ == nullptr) return nullptr;
  return (__bridge void*)current == context_ ? context_ : nullptr;
}

void* EglShim::GetCurrentSurface(std::int32_t read_draw) {
  (void)read_draw;
  return GetCurrentContext() != nullptr ? &g_surface_marker : nullptr;
}

std::uint32_t EglShim::BindApi(std::uint32_t api) {
  if (api != kOpenGlEsApi) {
    error_ = kBadParameter;
    return 0;
  }
  return 1;
}

std::uint32_t EglShim::QueryApi() { return kOpenGlEsApi; }

std::uint32_t EglShim::ReleaseThread() {
  [EAGLContext setCurrentContext:nil];
  return 1;
}

std::uint32_t EglShim::WaitClient() {
  glFinish();
  return 1;
}

void* EglShim::GetCurrentDisplay() { return GetCurrentContext() != nullptr ? &g_display_marker : nullptr; }

}  // namespace rb::platform::ios
