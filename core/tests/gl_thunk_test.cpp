// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// rb_gl_thunk_tests (M4 Task 2): the GL/EGL thunk table (android/jni/thunks.cpp) against this device's driver, on
// an offscreen EGL pbuffer. The context is made natively; every GL/EGL call after that goes the way a guest call
// does: the table's signature, the device symbol (HostSymbol), and ThunkCaller over guest registers and stacked
// words, with soft-float floats in core registers and output arrays in guest memory. Run on a device.
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>

#include "rb/abi/thunk.h"
#include "rb/mem/guest_memory.h"
#include "thunks.h"

namespace {

using namespace rb;

int checks = 0, failures = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}
#define CHECK(...)                                   \
  do {                                               \
    ++checks;                                        \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)
#define CHECK_CTX(cond, detail)                \
  do {                                         \
    ++checks;                                  \
    if (!(cond)) fail(#cond, __LINE__, detail); \
  } while (0)

constexpr std::uint32_t kStack = 0x10000u;
constexpr std::uint32_t kBuf = 0x20000u;
constexpr int kSide = 16;

std::uint32_t Bits(float f) {
  std::uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  return u;
}

struct Rig {
  std::unique_ptr<mem::GuestMemory> m;
  abi::HandleTable handles;
  EGLDisplay dpy = EGL_NO_DISPLAY;
  EGLContext ctx = EGL_NO_CONTEXT;
  EGLSurface surf = EGL_NO_SURFACE;
  bool ok = false;

  Rig() {
    mem::AddressModel model;
    model.kind = mem::AddressModel::kBiased;
    std::string err;
    m = mem::GuestMemory::Create(std::size_t{1} << 20, model, &err);
    m->Map(kStack, 0x2000, mem::kRead | mem::kWrite, &err);
    m->Map(kBuf, 0x2000, mem::kRead | mem::kWrite, &err);
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) return;
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, attrs, &cfg, 1, &n) || n < 1) return;
    const EGLint pb[] = {EGL_WIDTH, kSide, EGL_HEIGHT, kSide, EGL_NONE};
    surf = eglCreatePbufferSurface(dpy, cfg, pb);
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
    ok = surf != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, surf, surf, ctx);
  }
  ~Rig() {
    if (dpy != EGL_NO_DISPLAY) {
      eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
      if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
      eglTerminate(dpy);
    }
  }

  /// The guest's `name(args...)` through the table: r0..r3, then stacked words. Returns r0; `ok` false (with the
  /// reason in `err`) when the table, the device or ThunkCaller refused.
  std::uint32_t Call(const char* name, std::initializer_list<std::uint32_t> args, bool* ok, std::string* err) {
    const launcher::ThunkEntry* e = launcher::FindThunk(name);
    if (e == nullptr) {
      *ok = false;
      *err = std::string{name} + ": no table entry";
      return 0;
    }
    void* fn = launcher::HostSymbol(e->lib, name);
    if (fn == nullptr) {
      *ok = false;
      *err = std::string{name} + ": not exported by " + e->lib;
      return 0;
    }
    exec::MachineState s{};
    s.r[13] = kStack;
    unsigned i = 0;
    for (std::uint32_t v : args) {
      if (i < 4) {
        s.r[i] = v;
      } else {
        m->Write(kStack + 4u * (i - 4u), &v, 4);
      }
      ++i;
    }
    *ok = abi::ThunkCaller::Call(e->spec, fn, s, *m, handles, err);
    return s.r[0];
  }
};

void a_pbuffer_clears_and_reads_back_through_the_table() {
  Rig r;
  if (!r.ok) {
    std::printf("SKIP  no EGL pbuffer context on this device\n");
    return;
  }
  bool ok = false;
  std::string err;
  // glClearColor(1, 0.5, 0, 1): four soft-float floats in r0..r3.
  r.Call("glClearColor", {Bits(1.0f), Bits(0.5f), Bits(0.0f), Bits(1.0f)}, &ok, &err);
  CHECK_CTX(ok, err);
  r.Call("glClear", {GL_COLOR_BUFFER_BIT}, &ok, &err);
  CHECK_CTX(ok, err);
  // glReadPixels(0, 0, 16, 16, RGBA, UNSIGNED_BYTE, pixels): seven words, three stacked; pixels is guest memory.
  std::uint8_t zero[kSide * kSide * 4] = {};
  r.m->Write(kBuf, zero, sizeof zero);
  r.Call("glReadPixels", {0, 0, kSide, kSide, GL_RGBA, GL_UNSIGNED_BYTE, kBuf}, &ok, &err);
  CHECK_CTX(ok, err);
  std::uint8_t px[4] = {};
  r.m->Read(kBuf + 4u * (kSide * 8 + 8), px, 4);  // a centre pixel
  CHECK_CTX(px[0] == 255 && (px[1] == 127 || px[1] == 128) && px[2] == 0 && px[3] == 255,
            "pixel " + std::to_string(px[0]) + "," + std::to_string(px[1]) + "," + std::to_string(px[2]) + "," +
                std::to_string(px[3]));
  // glGetIntegerv(GL_VIEWPORT, out): an int array written into guest memory.
  r.Call("glGetIntegerv", {GL_VIEWPORT, kBuf + 0x1000}, &ok, &err);
  CHECK_CTX(ok, err);
  std::int32_t vp[4] = {};
  r.m->Read(kBuf + 0x1000, vp, sizeof vp);
  CHECK(vp[0] == 0 && vp[1] == 0 && vp[2] == kSide && vp[3] == kSide);
  CHECK(r.Call("glGetError", {}, &ok, &err) == GL_NO_ERROR && ok);
  // A wrong enum is the driver's error, read back through the table too.
  r.Call("glClear", {0xFFFFFFFFu}, &ok, &err);
  CHECK(r.Call("glGetError", {}, &ok, &err) == GL_INVALID_VALUE);
}

void egl_objects_come_back_as_handles() {
  Rig r;
  if (!r.ok) return;
  bool ok = false;
  std::string err;
  // eglGetCurrentContext() -> a handle; the table maps it back to the host context.
  const std::uint32_t h = r.Call("eglGetCurrentContext", {}, &ok, &err);
  CHECK_CTX(ok, err);
  void* host = nullptr;
  CHECK(h >= abi::HandleTable::kBase && r.handles.Get(h, &host) && host == r.ctx);
  // eglQueryContext(dpy, ctx, EGL_CONTEXT_CLIENT_VERSION, out): handles in, an int out in guest memory.
  const std::uint32_t hd = r.handles.Put(r.dpy);
  CHECK(r.Call("eglQueryContext", {hd, h, EGL_CONTEXT_CLIENT_VERSION, kBuf + 0x1100}, &ok, &err) == EGL_TRUE && ok);
  std::int32_t version = 0;
  r.m->Read(kBuf + 0x1100, &version, 4);
  CHECK_CTX(version >= 2, "client version " + std::to_string(version));
}

}  // namespace

int main() {
  a_pbuffer_clears_and_reads_back_through_the_table();
  egl_objects_come_back_as_handles();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
