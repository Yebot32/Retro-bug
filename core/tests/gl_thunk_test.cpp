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

#ifdef __aarch64__
#include <GLES3/gl3.h>
#include <set>
#include "host_slots.h"
#endif

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

  explicit Rig(int client_version = 2) {
    mem::AddressModel model;
    model.kind = mem::AddressModel::kBiased;
    std::string err;
    m = mem::GuestMemory::Create(std::size_t{1} << 20, model, &err);
    m->Map(kStack, 0x2000, mem::kRead | mem::kWrite, &err);
    m->Map(kBuf, 0x2000, mem::kRead | mem::kWrite, &err);
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) return;
    const EGLint rt = client_version >= 3 ? 0x40 /* EGL_OPENGL_ES3_BIT_KHR */ : EGL_OPENGL_ES2_BIT;
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, rt,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, attrs, &cfg, 1, &n) || n < 1) return;
    const EGLint pb[] = {EGL_WIDTH, kSide, EGL_HEIGHT, kSide, EGL_NONE};
    surf = eglCreatePbufferSurface(dpy, cfg, pb);
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, client_version, EGL_NONE};
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

void oes_3d_entry_points_work_through_the_table() {
  // Unity asks eglGetProcAddress for the OES spelling and stores the answer in its GLES device without a null
  // check (Crash Drive 2 and Angry Birds Epic both crashed on the null slot). These are the same entry points as
  // the core glTexImage3D/glTexSubImage3D, which the table already names; this checks the spelling resolves and
  // that the driver takes a 3D upload through it.
  const launcher::ThunkEntry* img = launcher::FindThunk("glTexImage3DOES");
  const launcher::ThunkEntry* sub = launcher::FindThunk("glTexSubImage3DOES");
  CHECK_CTX(img != nullptr, "glTexImage3DOES: no table entry");
  CHECK_CTX(sub != nullptr, "glTexSubImage3DOES: no table entry");
  CHECK(img != nullptr && std::string{img->spec} == "v:uiiiiiiuuo");
  CHECK(sub != nullptr && std::string{sub->spec} == "v:uiiiiiiiuuo");
  CHECK_CTX(img == nullptr || launcher::HostSymbol(img->lib, "glTexImage3DOES") != nullptr,
            "this driver exposes no glTexImage3DOES");
  CHECK_CTX(sub == nullptr || launcher::HostSymbol(sub->lib, "glTexSubImage3DOES") != nullptr,
            "this driver exposes no glTexSubImage3DOES");
  Rig r(3);
  if (!r.ok) {
    std::printf("SKIP  no EGL ES3 pbuffer context on this device\n");
    return;
  }
  bool ok = false;
  std::string err;
  constexpr std::uint32_t kTexture3D = 0x806F, kRgba8 = 0x8058;
  std::uint32_t tex = 0;
  r.Call("glGenTextures", {1, kBuf + 0x1800}, &ok, &err);
  CHECK_CTX(ok, err);
  r.m->Read(kBuf + 0x1800, &tex, 4);
  r.Call("glBindTexture", {kTexture3D, tex}, &ok, &err);
  CHECK_CTX(ok, err);
  const std::uint8_t px[4] = {1, 2, 3, 255};
  r.m->Write(kBuf + 0x1900, px, 4);
  r.Call("glTexImage3DOES", {kTexture3D, 0, kRgba8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kBuf + 0x1900}, &ok,
         &err);
  CHECK_CTX(ok, err);
  const std::uint32_t glerr = r.Call("glGetError", {}, &ok, &err);
  CHECK_CTX(glerr == GL_NO_ERROR, "glTexImage3DOES was refused: 0x" + std::to_string(glerr));
}

#ifdef __aarch64__
void unity_style_queries_and_buffer_rebinding_use_the_runtime_slots() {
  Rig rig(3);
  if (!rig.ok) {
    std::printf("SKIP  no EGL ES3 pbuffer context on this device\n");
    return;
  }
  runtime::GuestProcessOptions options;
  options.biased = true;
  options.window_bytes = 64u << 20;
  options.enable_jit = false;
  options.ctor_tier = 1;
  std::string why;
  auto process = runtime::GuestProcess::Create(options, &why);
  CHECK_CTX(process != nullptr, why);
  if (!process) return;
  CHECK(runtime::BindCurrentHostThread(process->abi().main_thread()));
  launcher::HostSlots slots(*process, 33);
  slots.enable_thunks();
  auto& mem = process->memory();
  const auto output = process->MapFree(mem::kGuestPage, mem::kRead | mem::kWrite, &why);
  CHECK_CTX(output != 0, why);
  if (output == 0) return;
  const auto call = [&](const char* name, std::initializer_list<std::uint32_t> words) -> std::uint32_t {
    const auto entry = slots.Resolve(name);
    CHECK_CTX(entry.has_value(), name);
    if (!entry) return 0;
    abi::Args32 args;
    for (auto word : words) args.push_back(abi::Value32::U32(word));
    exec::RunResult result;
    const auto ret = process->abi().CallGuest(*entry, args, abi::Shorty::For("I"), 1000, &result);
    CHECK_CTX(result.reason == exec::StopReason::Returned, std::string{name} + ": " + result.detail);
    return ret.w();
  };

  const auto version = call("glGetString", {GL_VERSION});
  CHECK(version != 0 && version == call("glGetString", {GL_VERSION}));
  CHECK(slots.CallsOf("glGetString") == 2);
  GLint major = 0, minor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  call("glGetIntegerv", {GL_MINOR_VERSION, output});
  std::uint32_t value = 0;
  CHECK(mem.Read(output, &value, sizeof value) && value == static_cast<std::uint32_t>(major == 3 && minor > 1 ? 1 : minor));
  CHECK(call("glGetString", {0xffffffffu}) == 0);
  CHECK(glGetError() == GL_INVALID_ENUM);
  const auto first = slots.GuestString("retrobug-extension-one");
  const auto second = slots.GuestString("retrobug-extension-two");
  CHECK(first != 0 && second - first == sizeof("retrobug-extension-one"));
  CHECK(first == slots.GuestString("retrobug-extension-one"));
  const auto* key_code = launcher::FindThunk("AKeyEvent_getKeyCode");
  CHECK(key_code != nullptr && std::strcmp(key_code->spec, "i:h") == 0);
  CHECK(key_code != nullptr && launcher::HostSymbol(key_code->lib, "AKeyEvent_getKeyCode") != nullptr);
  CHECK(slots.Resolve("AKeyEvent_getKeyCode").has_value());

  GLint extension_count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &extension_count);
  std::set<std::uint32_t> extension_pages;
  for (GLint i = 0; i < extension_count; ++i) {
    const auto guest = call("glGetStringi", {GL_EXTENSIONS, static_cast<std::uint32_t>(i)});
    CHECK(guest != 0);
    if (guest != 0) extension_pages.insert(guest & ~(mem::kGuestPage - 1u));
  }
  CHECK(extension_count > 0 && extension_pages.size() < static_cast<std::size_t>(extension_count));
  std::printf("GLES extension strings: %d entries, %zu guest pages\n", extension_count, extension_pages.size());

  GLuint buffers[2] = {};
  glGenBuffers(2, buffers);
  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  glBufferData(GL_ARRAY_BUFFER, 32, nullptr, GL_DYNAMIC_DRAW);
  const auto first_map = call("glMapBufferRange", {GL_ARRAY_BUFFER, 0, 32, GL_MAP_WRITE_BIT});
  glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
  glBufferData(GL_ARRAY_BUFFER, 32, nullptr, GL_DYNAMIC_DRAW);
  const auto second_map = call("glMapBufferRange", {GL_ARRAY_BUFFER, 8, 16, GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT});
  CHECK(first_map != 0 && second_map != 0 && first_map != second_map);
  const std::uint32_t first_word = 0x13572468u, second_word = 0x98765432u;
  CHECK(mem.Write(first_map, &first_word, sizeof first_word));
  CHECK(mem.Write(second_map + 4, &second_word, sizeof second_word));
  call("glFlushMappedBufferRange", {GL_ARRAY_BUFFER, 4, sizeof second_word});
  CHECK(glGetError() == GL_NO_ERROR);

  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  call("glGetBufferPointerv", {GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, output});
  CHECK(mem.Read(output, &value, sizeof value) && value == first_map);
  CHECK(call("glUnmapBuffer", {GL_ARRAY_BUFFER}) == GL_TRUE);
  glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
  CHECK(call("glUnmapBuffer", {GL_ARRAY_BUFFER}) == GL_TRUE);
  const auto read_second = call("glMapBufferRange", {GL_ARRAY_BUFFER, 12, sizeof second_word, GL_MAP_READ_BIT});
  CHECK(read_second != 0 && mem.Read(read_second, &value, sizeof value) && value == second_word);
  CHECK(call("glUnmapBuffer", {GL_ARRAY_BUFFER}) == GL_TRUE);
  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  const auto read_first = call("glMapBufferRange", {GL_ARRAY_BUFFER, 0, sizeof first_word, GL_MAP_READ_BIT});
  CHECK(read_first != 0 && mem.Read(read_first, &value, sizeof value) && value == first_word);
  CHECK(call("glUnmapBuffer", {GL_ARRAY_BUFFER}) == GL_TRUE);
  glDeleteBuffers(2, buffers);
  CHECK(glGetError() == GL_NO_ERROR);

  // Use an offset that is also a real guest address for both vertex and index data.
  glGenBuffers(2, buffers);
  const GLfloat vertices[] = {-1, -1, 3, -1, -1, 3};
  const GLushort indices[] = {0, 1, 2};
  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  glBufferData(GL_ARRAY_BUFFER, output + sizeof vertices, nullptr, GL_STATIC_DRAW);
  glBufferSubData(GL_ARRAY_BUFFER, output, sizeof vertices, vertices);
  call("glVertexAttribPointer", {0, 2, GL_FLOAT, GL_FALSE, 0, output});
  void* pointer = nullptr;
  glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
  CHECK(reinterpret_cast<std::uintptr_t>(pointer) == output);
  glEnableVertexAttribArray(0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, output + sizeof indices, nullptr, GL_STATIC_DRAW);
  glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, output, sizeof indices, indices);

  const char* vertex_source = "attribute vec2 xy; void main() { gl_Position = vec4(xy, 0.0, 1.0); }";
  const char* fragment_source = "precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }";
  const GLuint vertex = glCreateShader(GL_VERTEX_SHADER), fragment = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(vertex, 1, &vertex_source, nullptr);
  glShaderSource(fragment, 1, &fragment_source, nullptr);
  glCompileShader(vertex);
  glCompileShader(fragment);
  const GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glBindAttribLocation(program, 0, "xy");
  glLinkProgram(program);
  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  CHECK(linked == GL_TRUE);
  glUseProgram(program);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  call("glDrawElements", {GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, output});
  std::uint8_t pixel[4] = {};
  glReadPixels(kSide / 2, kSide / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  CHECK(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
  CHECK(glGetError() == GL_NO_ERROR);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  glDeleteProgram(program);
  glDeleteBuffers(2, buffers);
}
#endif

}  // namespace

int main() {
  a_pbuffer_clears_and_reads_back_through_the_table();
  egl_objects_come_back_as_handles();
  oes_3d_entry_points_work_through_the_table();
#ifdef __aarch64__
  unity_style_queries_and_buffer_rebinding_use_the_runtime_slots();
#endif
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
