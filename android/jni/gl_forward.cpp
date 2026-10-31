/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The launcher's GL renderer — see gl_forward.h for the two halves and the calling-convention fact the
 * forwarding rests on.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gl_forward.h"

#include <dlfcn.h>
#include <EGL/egl.h>

#include <cstring>
#include <map>
#include <mutex>

namespace rb::launcher {

namespace {

std::string EglErr(const char* step) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "%s failed (EGL error 0x%x)", step, eglGetError());
  return buf;
}

/**
 * GLES 1.1's entry points, each as "<ret>:<args>": ret `v` void or `i` an integer result; args `i` a
 * 32-bit integer word (GLenum, GLint, GLsizei, GLboolean, GLbitfield — all one core word under AAPCS32),
 * `f` a GLfloat (softfp: its raw bits in that word), `p` a guest pointer to translate. Every function the
 * two target games import is here (Gumball's 31, Angry Birds' 53), plus the rest of the common core so a
 * third game's first frame does not stop on a name nobody wrote down. The OES framebuffer entry points are
 * extensions, so they come from eglGetProcAddress rather than the library's exports.
 */
const std::map<std::string, const char*>& Signatures() {
  static const std::map<std::string, const char*> k = {
      {"glActiveTexture", "v:i"},        {"glAlphaFunc", "v:if"},
      {"glBindBuffer", "v:ii"},          {"glBindTexture", "v:ii"},
      {"glBlendFunc", "v:ii"},           {"glBufferData", "v:iipi"},
      {"glBufferSubData", "v:iiip"},     {"glClear", "v:i"},
      {"glClearColor", "v:ffff"},        {"glClearDepthf", "v:f"},
      {"glClearStencil", "v:i"},         {"glClientActiveTexture", "v:i"},
      {"glColor4f", "v:ffff"},           {"glColor4ub", "v:iiii"},
      {"glColorMask", "v:iiii"},         {"glColorPointer", "v:iiip"},
      {"glCompressedTexImage2D", "v:iiiiiiip"},
      {"glCopyTexImage2D", "v:iiiiiiii"},
      {"glCullFace", "v:i"},             {"glDeleteBuffers", "v:ip"},
      {"glDeleteTextures", "v:ip"},      {"glDepthFunc", "v:i"},
      {"glDepthMask", "v:i"},            {"glDepthRangef", "v:ff"},
      {"glDisable", "v:i"},              {"glDisableClientState", "v:i"},
      {"glDrawArrays", "v:iii"},         {"glDrawElements", "v:iiip"},
      {"glEnable", "v:i"},               {"glEnableClientState", "v:i"},
      {"glFinish", "v:"},                {"glFlush", "v:"},
      {"glFogf", "v:if"},                {"glFogfv", "v:ip"},
      {"glFrontFace", "v:i"},            {"glFrustumf", "v:ffffff"},
      {"glGenBuffers", "v:ip"},          {"glGenTextures", "v:ip"},
      {"glGetError", "i:"},              {"glGetFloatv", "v:ip"},
      {"glGetIntegerv", "v:ip"},         {"glHint", "v:ii"},
      {"glLightf", "v:iif"},             {"glLightfv", "v:iip"},
      {"glLightModelf", "v:if"},         {"glLightModelfv", "v:ip"},
      {"glLineWidth", "v:f"},            {"glLoadIdentity", "v:"},
      {"glLoadMatrixf", "v:p"},          {"glMaterialf", "v:iif"},
      {"glMaterialfv", "v:iip"},         {"glMatrixMode", "v:i"},
      {"glMultMatrixf", "v:p"},          {"glNormal3f", "v:fff"},
      {"glNormalPointer", "v:iip"},      {"glOrthof", "v:ffffff"},
      {"glPixelStorei", "v:ii"},         {"glPointSize", "v:f"},
      {"glPolygonOffset", "v:ff"},       {"glPopMatrix", "v:"},
      {"glPushMatrix", "v:"},            {"glReadPixels", "v:iiiiiip"},
      {"glRotatef", "v:ffff"},           {"glScalef", "v:fff"},
      {"glScissor", "v:iiii"},           {"glShadeModel", "v:i"},
      {"glStencilFunc", "v:iii"},        {"glStencilMask", "v:i"},
      {"glStencilOp", "v:iii"},          {"glTexCoordPointer", "v:iiip"},
      {"glTexEnvf", "v:iif"},            {"glTexEnvi", "v:iii"},
      {"glTexEnvfv", "v:iip"},           {"glTexImage2D", "v:iiiiiiiip"},
      {"glTexParameterf", "v:iif"},      {"glTexParameteri", "v:iii"},
      {"glTexSubImage2D", "v:iiiiiiiip"},
      {"glTranslatef", "v:fff"},         {"glVertexPointer", "v:iiip"},
      {"glViewport", "v:iiii"},
      {"glGenFramebuffersOES", "v:ip"},  {"glBindFramebufferOES", "v:ii"},
      {"glDeleteFramebuffersOES", "v:ip"},
      {"glFramebufferTexture2DOES", "v:iiiii"},
      {"glGenRenderbuffersOES", "v:ip"}, {"glBindRenderbufferOES", "v:ii"},
      {"glDeleteRenderbuffersOES", "v:ip"},
      {"glRenderbufferStorageOES", "v:iiii"},
      {"glFramebufferRenderbufferOES", "v:iiii"},
      {"glCheckFramebufferStatusOES", "i:i"},
  };
  return k;
}

/// Ten integer-class and eight float parameters; see gl_forward.h for why one type calls them all.
using Universal = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                    float, float, float, float, float, float, float, float,
                                    std::uint64_t, std::uint64_t);

void* HostFunction(const std::string& name) {
  static std::mutex mu;
  static std::map<std::string, void*> cache;
  static void* lib = dlopen("libGLESv1_CM.so", RTLD_NOW | RTLD_LOCAL);
  std::lock_guard<std::mutex> lock(mu);
  const auto it = cache.find(name);
  if (it != cache.end()) return it->second;
  void* fn = lib != nullptr ? dlsym(lib, name.c_str()) : nullptr;
  if (fn == nullptr) fn = reinterpret_cast<void*>(eglGetProcAddress(name.c_str()));
  cache[name] = fn;
  return fn;
}

}  // namespace

EglTarget::EglTarget(ANativeWindow* window) : window_{window} {
  if (window_ == nullptr) {
    error_ = "no window";
    return;
  }
  ANativeWindow_acquire(window_);
  display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
    error_ = EglErr("eglInitialize");
    return;
  }
  // GLES1, RGB888 with whatever alpha the device prefers, and a 16-bit depth buffer — what a 2010-era
  // GLSurfaceView asked for by default.
  const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                          EGL_RED_SIZE,        8,                  EGL_GREEN_SIZE,   8,
                          EGL_BLUE_SIZE,       8,                  EGL_DEPTH_SIZE,   16,
                          EGL_NONE};
  EGLConfig config = nullptr;
  EGLint n = 0;
  if (!eglChooseConfig(display_, attrs, &config, 1, &n) || n < 1) {
    error_ = EglErr("eglChooseConfig");
    return;
  }
  const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE};
  context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, ctx_attrs);
  if (context_ == EGL_NO_CONTEXT) {
    error_ = EglErr("eglCreateContext(GLES1)");
    return;
  }
  surface_ = eglCreateWindowSurface(display_, config, window_, nullptr);
  if (surface_ == EGL_NO_SURFACE) {
    error_ = EglErr("eglCreateWindowSurface");
    return;
  }
  if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
    error_ = EglErr("eglMakeCurrent");
    return;
  }
  eglQuerySurface(display_, surface_, EGL_WIDTH, &width_);
  eglQuerySurface(display_, surface_, EGL_HEIGHT, &height_);
  ok_ = true;
}

EglTarget::~EglTarget() {
  if (display_ != EGL_NO_DISPLAY) {
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
    if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
    eglTerminate(display_);
  }
  if (window_ != nullptr) ANativeWindow_release(window_);
}

bool EglTarget::Swap() {
  if (!ok_) return false;
  if (!eglSwapBuffers(display_, surface_)) {
    error_ = EglErr("eglSwapBuffers");
    return false;
  }
  return true;
}

bool GlForward::Known(const std::string& name) { return Signatures().count(name) != 0; }

bool GlForward::Call(const std::string& name, exec::MachineState& s, const mem::GuestMemory& mem,
                     std::string* err) {
  const auto sig = Signatures().find(name);
  if (sig == Signatures().end()) {
    if (err != nullptr) *err = name + " has no forwarding signature";
    return false;
  }
  void* fn = HostFunction(name);
  if (fn == nullptr) {
    if (err != nullptr) *err = name + " is not exported by this device's GLES1 driver";
    return false;
  }
  const char* spec = sig->second;
  const bool returns = spec[0] == 'i';
  const char* args = std::strchr(spec, ':') + 1;

  // Guest argument word i: r0..r3, then the guest stack at sp.
  const auto word = [&](unsigned i, bool* ok) -> std::uint32_t {
    if (i < 4) return s.r[i];
    std::uint32_t v = 0;
    if (!mem.Read(s.r[13] + 4u * (i - 4u), &v, 4)) *ok = false;
    return v;
  };
  std::uint64_t ints[10] = {};
  float floats[8] = {};
  unsigned ni = 0, nf = 0;
  bool ok = true;
  for (unsigned i = 0; args[i] != '\0'; ++i) {
    const std::uint32_t w = word(i, &ok);
    if (args[i] == 'f') {
      std::memcpy(&floats[nf++], &w, 4);
    } else if (args[i] == 'p') {
      // A null stays null; a small value with a buffer bound is an offset, not an address (GLES1.1 VBOs);
      // anything else must be guest memory, or the driver would read a host address nobody owns.
      void* host = nullptr;
      if (w != 0) host = mem.HostForAccess(w);
      if (host == nullptr && w >= 0x10000u) {
        if (err != nullptr) {
          char buf[96];
          std::snprintf(buf, sizeof buf, "%s: pointer argument 0x%x is not guest memory", name.c_str(), w);
          *err = buf;
        }
        return false;
      }
      ints[ni++] = host != nullptr ? reinterpret_cast<std::uint64_t>(host) : w;
    } else {
      // GLint/GLsizei are signed: sign-extend so a negative value reaches the driver as one.
      ints[ni++] = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(w)));
    }
  }
  if (!ok) {
    if (err != nullptr) *err = name + ": a stacked argument is not readable guest memory";
    return false;
  }
  const auto call = reinterpret_cast<Universal>(fn);
  // Integers beyond the eighth go to the stack in order: the 17th and 18th parameters of `Universal`
  // occupy exactly the stack slots a callee's ninth and tenth integer parameters are read from.
  const std::uint64_t r = call(ints[0], ints[1], ints[2], ints[3], ints[4], ints[5], ints[6], ints[7],
                               floats[0], floats[1], floats[2], floats[3], floats[4], floats[5], floats[6],
                               floats[7], ints[8], ints[9]);
  s.r[0] = returns ? static_cast<std::uint32_t>(r) : 0u;
  ++forwarded_;
  return true;
}

}  // namespace rb::launcher
