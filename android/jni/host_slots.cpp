// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See host_slots.h. Moved out of launcher.cpp (where `Inspect` bound these per run) so the in-app runtime
// answers the same names the same way.
#include "host_slots.h"
#include "thunks.h"
#include "rb/abi/guest_log.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cctype>
#include <mutex>
#include <set>
#include <cstdio>
#include <cstring>

namespace rb::launcher {
namespace {

using abi::GuestFormat;
using abi::GuestVarargs;
using abi::PrioName;

std::string Hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%x", v);
  return buf;
}

/// liblog: what a game calls is three functions; answering them puts the game's own log lines in the report.
enum class LogKind { kPrint, kVprint, kWrite };
struct LogEntry {
  const char* name;
  LogKind kind;
};
constexpr LogEntry kLogTable[] = {
    {"__android_log_print", LogKind::kPrint},
    {"__android_log_vprint", LogKind::kVprint},
    {"__android_log_write", LogKind::kWrite},
};
std::optional<std::size_t> LogSlotIndex(const std::string& name) {
  for (std::size_t i = 0; i < sizeof(kLogTable) / sizeof(kLogTable[0]); ++i) {
    if (name == kLogTable[i].name) return i;
  }
  return std::nullopt;
}

/// The dynamic-linker surface this build answers, and the answer each name gives. A fixed table rather
/// than `IsGlName`'s prefix shape: this is an interface, not a namespace — those names are what libc's
/// loader-side callers actually import, and an unlisted `dl*`-looking name must stay trapped rather
/// than collect a stub (a trap names itself since the scope-unanswered fix; a wrong stub does not).
enum class DlKind {
  kDlopen,
  kDlopenExt,
  kDlsym,
  kDlclose,
  kDlerror,
  kDladdr,
  kExidx,
  kTargetSdk,
  kAddDtor,
  kRemoveDtor,
  kNamespace,
};
struct DlEntry {
  const char* name;
  DlKind kind;
};
constexpr DlEntry kDlTable[] = {
    {"dlopen", DlKind::kDlopen},
    {"android_dlopen_ext", DlKind::kDlopenExt},
    {"dlsym", DlKind::kDlsym},
    {"dlclose", DlKind::kDlclose},
    {"dlerror", DlKind::kDlerror},
    {"dladdr", DlKind::kDladdr},
    {"dl_unwind_find_exidx", DlKind::kExidx},
    {"android_get_application_target_sdk_version", DlKind::kTargetSdk},
    {"__loader_add_thread_local_dtor", DlKind::kAddDtor},
    {"__loader_remove_thread_local_dtor", DlKind::kRemoveDtor},
    {"__loader_android_get_exported_namespace", DlKind::kNamespace},
};

/// The table index of `name`, or `nullopt` — see `kDlTable` for why this is an exact match.
std::optional<std::size_t> DlSlotIndex(const std::string& name) {
  for (std::size_t i = 0; i < sizeof(kDlTable) / sizeof(kDlTable[0]); ++i) {
    if (name == kDlTable[i].name) return i;
  }
  return std::nullopt;
}

}  // namespace

static_assert(HostSlots::kDlSlotBase + HostSlots::kDlSlots <= HostSlots::kLogSlotBase &&
                  HostSlots::kLogSlotBase + sizeof(kLogTable) / sizeof(kLogTable[0]) <= abi::AbiContext::kMaxSlots,
              "the liblog host slots would alias the dl window or be unreachable past kMaxSlots");
static_assert(sizeof(kDlTable) / sizeof(kDlTable[0]) <= HostSlots::kDlSlots, "the dl table outgrew its window");

/// The launcher-facing spelling of the shared bounded reader (rb/abi/guest_log.h).
std::string GuestStringOrEmpty(const mem::GuestMemory& mem, std::uint32_t at) {
  return abi::GuestStringOrEmpty(mem, at);
}

HostSlots::HostSlots(runtime::GuestProcess& p, int target_sdk, std::function<void(const std::string&)> live_log)
    : p_(p), target_sdk_(target_sdk), live_log_(std::move(live_log)) {
  // Both tables are bound `kHoldsGgl`: they are the linker's and liblog's part of the process (they read the
  // module list and bump this object's counters and log record, which the GGL protects), they never wait on
  // another guest thread, and logcat's write does not block on the guest.
  abi::AbiContext& ctx = p_.abi();
  for (std::size_t i = 0; i < sizeof(kDlTable) / sizeof(kDlTable[0]); ++i) {
    dl_calls_.push_back(DlCall{kDlTable[i].name, 0, {}, 0});
    const std::size_t index = i;
    ctx.Bind(kDlSlotBase + static_cast<std::uint32_t>(i), [this, index](exec::MachineState& s, std::string* serr) {
      DlCall& c = dl_calls_[index];
      ++c.calls;
      mem::GuestMemory& mem = p_.memory();
      switch (kDlTable[index].kind) {
        case DlKind::kExidx: {
          // `_Unwind_Ptr dl_unwind_find_exidx(_Unwind_Ptr pc, int* pcount)`: the module whose range holds
          // pc gives its `.ARM.exidx` and entry count; no module, NULL and 0 — bionic's answer too.
          // Answering NULL for everything made every C++ throw terminate (no frame had unwind info).
          const std::uint32_t pc = s.r[0] & ~1u;
          std::uint32_t at = 0, count = 0;
          for (const auto& m : p_.modules()) {
            if (pc >= m->base && pc - m->base < m->extent && m->exidx != 0) {
              at = m->exidx;
              count = m->exidx_count;
              break;
            }
          }
          if (s.r[1] != 0 && !mem.Write(s.r[1], &count, 4)) {
            if (serr != nullptr) *serr = "dl_unwind_find_exidx: pcount 0x" + Hex(s.r[1]) + " is not writable";
            return false;
          }
          if (at != 0) ++c.answered;
          s.r[0] = at;
          return true;
        }
        case DlKind::kDlopen:
        case DlKind::kDlopenExt:
          // NULL: the library this build cannot provide. A caller that checks — netd's does — carries
          // on; one that does not faults on the NULL it asked to be lied to about. With the thunks on,
          // libnetd_client.so is the exception: bionic's resolver needs it to reach netd (dnsproxyd), and
          // without it every getaddrinfo fails (there are no nameservers to fall back on).
          c.last_path = GuestStringOrEmpty(mem, s.r[0]);
          if (thunks_on_ && c.last_path.size() >= 17 &&
              c.last_path.compare(c.last_path.size() - 17, 17, "libnetd_client.so") == 0) {
            s.r[0] = kNetdHandle;
            return true;
          }
          // The game's own libraries (Unity's libmain dlopens libunity, libmono, by full path): the runtime loads
          // them from assets/rb32/. Anything else stays NULL, which callers that probe check.
          {
            std::string why;
            const std::uint32_t h = dlopen_hook_ ? dlopen_hook_(c.last_path, &why) : 0u;
            if (h == 0 && !why.empty() && live_log_) live_log_("dl: dlopen(\"" + c.last_path + "\") = NULL: " + why);
            s.r[0] = h;
          }
          return true;
        case DlKind::kDlsym: {
          // The stand-in libnetd_client.so exports one entry point: netdClientInitDnsOpenProxy.
          const std::string sym = GuestStringOrEmpty(mem, s.r[1]);
          std::optional<std::uint32_t> at;
          if (s.r[0] == kNetdHandle && sym == "netdClientInitDnsOpenProxy") at = Resolve("rb_netdClientInitDnsOpenProxy");
          else if (dlsym_hook_) at = dlsym_hook_(s.r[0], sym);
          s.r[0] = at ? *at : 0u;
          return true;
        }
        case DlKind::kTargetSdk:
          s.r[0] = static_cast<std::uint32_t>(target_sdk_);
          return true;
        default:
          s.r[0] = 0;  // NULL / 0 / "no error since last clear": every caller checks
          return true;
      }
    }, abi::AbiContext::SlotLock::kHoldsGgl);
  }
  for (std::size_t i = 0; i < sizeof(kLogTable) / sizeof(kLogTable[0]); ++i) {
    const LogKind kind = kLogTable[i].kind;
    ctx.Bind(kLogSlotBase + static_cast<std::uint32_t>(i), [this, kind](exec::MachineState& s, std::string*) {
      const mem::GuestMemory& mem = p_.memory();
      // (prio, tag, fmt, ...) / (prio, tag, fmt, va_list) / (prio, tag, msg)
      LogLine line{static_cast<int>(s.r[0]), GuestStringOrEmpty(mem, s.r[1]), {}};
      if (kind == LogKind::kWrite) {
        line.text = GuestStringOrEmpty(mem, s.r[2]);
      } else {
        GuestVarargs va;
        va.mem = &mem;
        if (kind == LogKind::kPrint) {
          va.reg_free = true;
          va.reg = s.r[3];
          va.at = s.r[13];
        } else {
          va.at = s.r[3];  // AAPCS va_list: one pointer into the argument area
        }
        line.text = GuestFormat(mem, s.r[2], va);
        if (!va.ok) line.text += "  [some arguments were unreadable guest memory]";
      }
      while (!line.text.empty() && (line.text.back() == '\n' || line.text.back() == '\r')) line.text.pop_back();
      if (live_log_) live_log_("guest log: " + PrioName(line.prio) + "/" + line.tag + ": " + line.text);
      if (log_.size() < kLogMax) {
        log_.push_back(std::move(line));
      } else {
        ++log_dropped_;
      }
      s.r[0] = 1;  // liblog returns a positive byte count on success; 1 is truthful for "written"
      return true;
    }, abi::AbiContext::SlotLock::kHoldsGgl);
  }
}

namespace {
/// Calls that wait (a vsync, a context switch, the GPU draining) and read no guest memory the GGL protects: run with
/// the lock released, or every other guest thread (a game's loader and network threads) waits out each frame.
bool Waits(const std::string& name) {
  static const std::set<std::string> k = {"eglSwapBuffers", "eglMakeCurrent", "eglInitialize", "eglTerminate",
                                          "eglWaitGL", "eglWaitClient", "eglWaitNative", "eglSwapInterval",
                                          "eglCreateWindowSurface", "eglDestroySurface", "eglDestroyContext",
                                          "glFinish", "glFlush"};
  return k.count(name) != 0;
}

std::uint32_t GlOffsetBinding(std::string_view name) {
  if (name == "glVertexAttribPointer" || name == "glVertexAttribIPointer" || name == "glVertexPointer" ||
      name == "glColorPointer" || name == "glTexCoordPointer" || name == "glNormalPointer") {
    return GL_ARRAY_BUFFER_BINDING;
  }
  if (name == "glDrawElements" || name == "glDrawRangeElements" || name == "glDrawElementsInstanced" ||
      name == "glDrawElementsInstancedEXT") {
    return GL_ELEMENT_ARRAY_BUFFER_BINDING;
  }
  return 0;
}

bool GlBufferBound(std::uint32_t binding) {
  GLint buffer = 0;
  glGetIntegerv(binding, &buffer);
  return buffer != 0;
}
}  // namespace

std::optional<std::uint32_t> HostSlots::Resolve(const std::string& name) {
  if (const auto dl = DlSlotIndex(name)) return p_.abi().SlotFor(kDlSlotBase + static_cast<std::uint32_t>(*dl));
  if (const auto lg = LogSlotIndex(name)) return p_.abi().SlotFor(kLogSlotBase + static_cast<std::uint32_t>(*lg));
  if (!thunks_on_) return std::nullopt;
  if (const auto it = thunk_indices_.find(name); it != thunk_indices_.end()) {
    return p_.abi().SlotFor(kThunkSlotBase + static_cast<std::uint32_t>(it->second));
  }
  const auto special = specials_.find(name);
  const ThunkEntry* entry = special == specials_.end() ? FindThunk(name) : nullptr;
  if (special == specials_.end() && entry == nullptr) {
    for (const Resolver& r : resolvers_) {
      if (auto at = r(name)) return at;
    }
    return std::nullopt;  // no thunk: stays trapped, by name
  }
  if (thunk_calls_.size() >= kThunkSlots) {
    if (live_log_) live_log_("thunks: no slot left for " + name + " (" + std::to_string(kThunkSlots) + " in use)");
    return std::nullopt;
  }
  const std::size_t index = thunk_calls_.size();
  ThunkCall* counter;
  {
    std::lock_guard<std::mutex> lk(counters_mu_);
    counter = &thunk_calls_.emplace_back(name);
    thunk_indices_.emplace(name, index);
  }
  Special fn;
  if (special != specials_.end()) {
    fn = special->second;
  } else {
    // The host function is looked up on first call (a library may only be loadable once the app runs), then kept.
    auto host = std::make_shared<std::atomic<void*>>(nullptr);
    const auto binding = GlOffsetBinding(name);
    fn = [this, entry, name, host, binding](exec::MachineState& s, std::string* why) {
      void* function = host->load(std::memory_order_relaxed);
      if (function == nullptr) {
        function = HostSymbol(entry->lib, name);
        if (function == nullptr) {
          *why = name + " is not exported by this device's " + entry->lib;
          return false;
        }
        host->store(function, std::memory_order_relaxed);
      }
      return abi::ThunkCaller::Call(entry->spec, function, s, p_.memory(), handles_, why,
                                    binding != 0 ? GlBufferBound : nullptr, binding);
    };
  }
  // `kHoldsGgl` (M2 Task 4, ruling i, as the launcher's GL slots): the host reads guest memory through translated
  // pointers — the call's own, and client arrays an earlier gl*Pointer registered, read at draw time — which
  // another guest thread could unmap with the GGL released. The deque keeps the counter's address stable.
  p_.abi().Bind(kThunkSlotBase + static_cast<std::uint32_t>(index), [this, counter, fn](exec::MachineState& s,
                                                                                       std::string*) {
    ThunkCall& c = *counter;
    c.calls.fetch_add(1, std::memory_order_relaxed);
    std::string why;
    if (fn(s, &why)) return true;
    s.r[0] = 0;
    bool first = false;
    {
      std::lock_guard<std::mutex> lk(counters_mu_);
      first = c.refusals++ == 0;
      if (first) c.refused = why;
    }
    if (first && live_log_) live_log_("thunks: " + c.name + " refused: " + why);
    return true;
  }, Waits(name) ? abi::AbiContext::SlotLock::kReleasesGgl : abi::AbiContext::SlotLock::kHoldsGgl);
  return p_.abi().SlotFor(kThunkSlotBase + static_cast<std::uint32_t>(index));
}

std::uint32_t HostSlots::GuestString(std::string_view text) {
  if (const auto it = guest_strings_.find(text); it != guest_strings_.end()) return it->second;
  std::string stored{text};
  const std::size_t bytes = stored.size() + 1;
  std::string e;
  std::uint32_t at;
  if (bytes > mem::kGuestPage) {
    at = p_.MapFree(bytes, mem::kRead | mem::kWrite, &e);
  } else {
    if (bytes > string_page_left_) {
      const auto page = p_.MapFree(mem::kGuestPage, mem::kRead | mem::kWrite, &e);
      if (page == 0) return 0;
      string_page_next_ = page;
      string_page_left_ = mem::kGuestPage;
    }
    at = string_page_next_;
    string_page_next_ += static_cast<std::uint32_t>(bytes);
    string_page_left_ -= bytes;
  }
  if (at == 0 || !p_.memory().Write(at, stored.c_str(), bytes)) return 0;
  guest_strings_.emplace(std::move(stored), at);
  return at;
}

void HostSlots::enable_thunks() {
  thunks_on_ = true;
  // ---- the GL/EGL calls a signature cannot express
  // glGetString / eglQueryString: the answer is a host pointer to a constant string — copied once into guest memory.
  AddSpecial("glGetString", [this](exec::MachineState& s, std::string* why) {
    using Fn = const unsigned char* (*)(unsigned);
    const auto f = reinterpret_cast<Fn>(HostSymbol("libGLESv2.so", "glGetString"));
    const unsigned char* host = f != nullptr ? f(s.r[0]) : nullptr;
    if (!f) { *why = "glGetString is unavailable"; return false; }
    if (host == nullptr) { s.r[0] = 0; return true; }
    std::string_view text = reinterpret_cast<const char*>(host);
    // The guest gets the ES 2.0, 3.0 and 3.1 entry points (thunks.cpp): an ES 3.2 context is reported as 3.1, so an
    // engine that reads the version does not take a 3.2-only path. Nothing of the host's string is kept for a
    // substring search to find a higher version in.
    if (s.r[0] == 0x1F02u /* GL_VERSION */ && text.compare(0, 12, "OpenGL ES 3.") == 0 && text.compare(12, 1, "2") >= 0) {
      text = "OpenGL ES 3.1 RetroBug";
    } else if (s.r[0] == 0x8B8Cu /* GL_SHADING_LANGUAGE_VERSION */ && text.compare(0, 20, "OpenGL ES GLSL ES 3.") == 0 &&
               text.compare(20, 1, "2") >= 0) {
      text = "OpenGL ES GLSL ES 3.10";
    }
    s.r[0] = GuestString(text);
    return true;
  });
  AddSpecial("glGetIntegerv", [this](exec::MachineState& s, std::string* why) {
    const auto name = s.r[0];
    const auto out = s.r[1];
    if (!abi::ThunkCaller::Call("v:up", reinterpret_cast<void*>(glGetIntegerv), s, p_.memory(), handles_, why)) {
      return false;
    }
    // Keep the integer version query consistent with GL_VERSION. ES 2 contexts may reject this enum.
    if (name == GL_MINOR_VERSION && out != 0) {
      const auto* version = glGetString(GL_VERSION);
      if (version != nullptr && std::strncmp(reinterpret_cast<const char*>(version), "OpenGL ES 3.", 12) == 0) {
        GLint minor = 0;
        if (p_.memory().Read(out, &minor, sizeof minor) && minor > 1) {
          minor = 1;
          p_.memory().Write(out, &minor, sizeof minor);
        }
      }
    }
    return true;
  });
  // glGetStringi(name, index): an engine that enumerates extensions one by one (Unity: GL_NUM_EXTENSIONS, then
  // glGetStringi(GL_EXTENSIONS, i)) gets each host string, copied once into guest memory like glGetString's.
  AddSpecial("glGetStringi", [this](exec::MachineState& s, std::string* why) {
    using Fn = const unsigned char* (*)(unsigned, unsigned);
    static Fn f = nullptr;
    if (f == nullptr) f = reinterpret_cast<Fn>(HostSymbol("libGLESv3.so", "glGetStringi"));
    if (f == nullptr) f = reinterpret_cast<Fn>(HostSymbol("libGLESv2.so", "glGetStringi"));
    const unsigned char* host = f != nullptr ? f(s.r[0], s.r[1]) : nullptr;
    if (!f) { *why = "glGetStringi is unavailable"; return false; }
    s.r[0] = host == nullptr ? 0u : GuestString(reinterpret_cast<const char*>(host));
    return true;
  });
  // glMapBufferRange / glFlushMappedBufferRange / glUnmapBuffer: the driver's mapping is host memory the guest cannot
  // address, so the guest gets a shadow in its own window — filled from the mapping when read access was asked for,
  // written back on flush and on unmap when write access was requested. Record mappings by the driver's
  // pointer: rebinding a target may select a different buffer, and a shared buffer may use another target.
  struct GlMap { std::uint32_t guest, mapped, len, access; };
  auto maps = std::make_shared<std::map<void*, GlMap>>();
  using MapBuffer = void* (*)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
  using GetBufferPointer = void (*)(GLenum, GLenum, void**);
  using UnmapBuffer = GLboolean (*)(GLenum);
  const auto map_buffer = reinterpret_cast<MapBuffer>(HostSymbol("libGLESv3.so", "glMapBufferRange"));
  const auto get_buffer_pointer = reinterpret_cast<GetBufferPointer>(HostSymbol("libGLESv3.so", "glGetBufferPointerv"));
  const auto unmap_buffer = reinterpret_cast<UnmapBuffer>(HostSymbol("libGLESv3.so", "glUnmapBuffer"));
  const auto bound_mapping = [get_buffer_pointer](GLenum target) -> void* {
    void* host = nullptr;
    if (get_buffer_pointer) get_buffer_pointer(target, GL_BUFFER_MAP_POINTER, &host);
    return host;
  };
  AddSpecial("glMapBufferRange", [this, maps, map_buffer, get_buffer_pointer, unmap_buffer](exec::MachineState& s, std::string* why) {
    if (!map_buffer || !get_buffer_pointer || !unmap_buffer) {
      *why = "this device has no GLES buffer mapping functions";
      return false;
    }
    const std::uint32_t target = s.r[0], access = s.r[3];
    const GLintptr off = static_cast<std::int32_t>(s.r[1]);
    const GLsizeiptr len = static_cast<std::int32_t>(s.r[2]);
    void* host = map_buffer(target, off, len, access);
    if (host == nullptr) { s.r[0] = 0; return true; }  // the GL error is the driver's to report
    if (len <= 0) {
      unmap_buffer(target);
      s.r[0] = 0;
      return true;
    }
    std::string e;
    if (const auto stale = maps->find(host); stale != maps->end()) {
      // A buffer deleted without an explicit unmap may have released this driver address.
      p_.memory().Unmap(stale->second.guest, stale->second.mapped, &e);
      maps->erase(stale);
    }
    std::uint32_t mapped = static_cast<std::uint32_t>((static_cast<std::size_t>(len) + mem::kGuestPage - 1) & ~std::size_t{mem::kGuestPage - 1});
    const std::uint32_t guest = p_.MapFree(mapped, mem::kRead | mem::kWrite, &e);
    if (guest == 0 || ((access & GL_MAP_READ_BIT) != 0 && !p_.memory().Write(guest, host, static_cast<std::size_t>(len)))) {
      if (guest != 0) p_.memory().Unmap(guest, mapped, &e);
      unmap_buffer(target);
      *why = "glMapBufferRange: cannot stage " + std::to_string(len) + " bytes in guest memory: " + e;
      return false;
    }
    (*maps)[host] = GlMap{guest, mapped, static_cast<std::uint32_t>(len), access};
    s.r[0] = guest;
    return true;
  });
  AddSpecial("glFlushMappedBufferRange", [this, maps, bound_mapping](exec::MachineState& s, std::string* why) {
    using Fn = void (*)(GLenum, GLintptr, GLsizeiptr);
    static const auto f = reinterpret_cast<Fn>(HostSymbol("libGLESv3.so", "glFlushMappedBufferRange"));
    if (f == nullptr) { *why = "this device's libGLESv3 has no glFlushMappedBufferRange"; return false; }
    const GLintptr off = static_cast<std::int32_t>(s.r[1]);
    const GLsizeiptr len = static_cast<std::int32_t>(s.r[2]);
    if (!maps->empty() && off >= 0 && len > 0) {
      void* host = bound_mapping(s.r[0]);
      const auto it = maps->find(host);
      if (it != maps->end() && (it->second.access & GL_MAP_FLUSH_EXPLICIT_BIT) != 0 &&
          static_cast<std::uint64_t>(off + len) <= it->second.len &&
          !p_.memory().Read(it->second.guest + static_cast<std::uint32_t>(off), static_cast<char*>(host) + off,
                            static_cast<std::size_t>(len))) {
        *why = "glFlushMappedBufferRange: guest shadow is unreadable";
        return false;
      }
    }
    f(s.r[0], off, len);
    return true;
  });
  AddSpecial("glUnmapBuffer", [this, maps, bound_mapping, unmap_buffer](exec::MachineState& s, std::string* why) {
    if (!unmap_buffer) { *why = "this device's libGLESv3 has no glUnmapBuffer"; return false; }
    void* host = maps->empty() ? nullptr : bound_mapping(s.r[0]);
    const auto it = maps->find(host);
    bool copied = true;
    if (it != maps->end()) {
      const GlMap m = it->second;
      if ((m.access & GL_MAP_WRITE_BIT) != 0 && (m.access & GL_MAP_FLUSH_EXPLICIT_BIT) == 0) {
        copied = p_.memory().Read(m.guest, host, m.len);
      }
      std::string e;
      p_.memory().Unmap(m.guest, m.mapped, &e);
      maps->erase(it);
    }
    s.r[0] = unmap_buffer(s.r[0]);
    if (!copied) *why = "glUnmapBuffer: guest shadow is unreadable";
    return copied;
  });
  AddSpecial("glGetBufferPointerv", [this, maps, get_buffer_pointer](exec::MachineState& s, std::string* why) {
    if (!get_buffer_pointer) { *why = "this device's libGLESv3 has no glGetBufferPointerv"; return false; }
    // Preserve the guest's output on an invalid GL query.
    void* const untouched = &s;
    void* host = untouched;
    get_buffer_pointer(s.r[0], s.r[1], &host);
    if (host == untouched) return true;
    const auto it = maps->find(host);
    const std::uint32_t guest = it == maps->end() ? 0 : it->second.guest;
    if (p_.memory().Write(s.r[2], &guest, sizeof guest)) return true;
    *why = "glGetBufferPointerv: result is not writable guest memory";
    return false;
  });
  // glGetUniformIndices / glTransformFeedbackVaryings: an array of guest string pointers, read into host strings.
  AddSpecial("glGetUniformIndices", [this](exec::MachineState& s, std::string* why) {
    using Fn = void (*)(unsigned, int, const char* const*, unsigned*);
    static Fn f = nullptr;
    if (f == nullptr) f = reinterpret_cast<Fn>(HostSymbol("libGLESv3.so", "glGetUniformIndices"));
    if (f == nullptr) { *why = "this device's libGLESv3 has no glGetUniformIndices"; return false; }
    const int count = static_cast<std::int32_t>(s.r[1]);
    if (count < 0 || count > 4096) { *why = "glGetUniformIndices: count " + std::to_string(count); return false; }
    auto& mem = p_.memory();
    std::vector<std::string> names(static_cast<std::size_t>(count));
    std::vector<const char*> ptrs(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
      std::uint32_t at = 0;
      mem.Read(s.r[2] + 4u * static_cast<std::uint32_t>(i), &at, 4);
      names[static_cast<std::size_t>(i)] = GuestStringOrEmpty(mem, at);
      ptrs[static_cast<std::size_t>(i)] = names[static_cast<std::size_t>(i)].c_str();
    }
    std::vector<unsigned> out(static_cast<std::size_t>(count));
    f(s.r[0], count, ptrs.data(), out.data());
    if (count > 0 && !mem.Write(s.r[3], out.data(), out.size() * 4)) { *why = "glGetUniformIndices: indices not writable"; return false; }
    return true;
  });
  AddSpecial("glTransformFeedbackVaryings", [this](exec::MachineState& s, std::string* why) {
    using Fn = void (*)(unsigned, int, const char* const*, unsigned);
    static Fn f = nullptr;
    if (f == nullptr) f = reinterpret_cast<Fn>(HostSymbol("libGLESv3.so", "glTransformFeedbackVaryings"));
    if (f == nullptr) { *why = "this device's libGLESv3 has no glTransformFeedbackVaryings"; return false; }
    const int count = static_cast<std::int32_t>(s.r[1]);
    if (count < 0 || count > 4096) { *why = "glTransformFeedbackVaryings: count " + std::to_string(count); return false; }
    auto& mem = p_.memory();
    std::vector<std::string> names(static_cast<std::size_t>(count));
    std::vector<const char*> ptrs(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
      std::uint32_t at = 0;
      mem.Read(s.r[2] + 4u * static_cast<std::uint32_t>(i), &at, 4);
      names[static_cast<std::size_t>(i)] = GuestStringOrEmpty(mem, at);
      ptrs[static_cast<std::size_t>(i)] = names[static_cast<std::size_t>(i)].c_str();
    }
    f(s.r[0], count, ptrs.data(), s.r[3]);
    return true;
  });
  // glGetVertexAttribPointerv(index, pname, void** out): the host answers what glVertexAttribPointer was given — a
  // translated guest pointer (client array) or a buffer offset. A pointer into the guest window goes back as its guest
  // address; anything else is an offset, which fits in 32 bits.
  AddSpecial("glGetVertexAttribPointerv", [this](exec::MachineState& s, std::string* why) {
    using Fn = void (*)(unsigned, unsigned, void**);
    const auto f = reinterpret_cast<Fn>(HostSymbol("libGLESv2.so", "glGetVertexAttribPointerv"));
    if (f == nullptr) {
      *why = "this device's libGLESv2 has no glGetVertexAttribPointerv";
      return false;
    }
    void* host = nullptr;
    f(s.r[0], s.r[1], &host);
    std::uint32_t guest = 0;
    if (host != nullptr && !p_.memory().Translate(host, &guest)) guest = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(host));
    if (s.r[2] != 0 && !p_.memory().Write(s.r[2], &guest, 4)) {
      *why = "glGetVertexAttribPointerv: the out pointer " + Hex(s.r[2]) + " is not writable guest memory";
      return false;
    }
    return true;
  });
  // GLES1's glGetPointerv(pname, void** out): the same answer for the client-array pointers (Where's My Mickey).
  AddSpecial("glGetPointerv", [this](exec::MachineState& s, std::string* why) {
    using Fn = void (*)(unsigned, void**);
    const auto f = reinterpret_cast<Fn>(HostSymbol("libGLESv1_CM.so", "glGetPointerv"));
    if (f == nullptr) {
      *why = "this device's libGLESv1_CM has no glGetPointerv";
      return false;
    }
    void* host = nullptr;
    f(s.r[0], &host);
    std::uint32_t guest = 0;
    if (host != nullptr && !p_.memory().Translate(host, &guest)) guest = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(host));
    if (s.r[1] != 0 && !p_.memory().Write(s.r[1], &guest, 4)) {
      *why = "glGetPointerv: the out pointer " + Hex(s.r[1]) + " is not writable guest memory";
      return false;
    }
    return true;
  });
  AddSpecial("eglQueryString", [this](exec::MachineState& s, std::string* why) {
    void* dpy = nullptr;
    if (!handles_.Get(s.r[0], &dpy)) {
      *why = "eglQueryString: display " + Hex(s.r[0]) + " was never issued";
      return false;
    }
    using Fn = const char* (*)(void*, int);
    const auto f = reinterpret_cast<Fn>(HostSymbol("libEGL.so", "eglQueryString"));
    const char* host = f != nullptr ? f(dpy, static_cast<int>(s.r[1])) : nullptr;
    s.r[0] = host == nullptr ? 0u : GuestString(host);
    return true;  // NULL is EGL's own answer for a bad name (eglGetError says why)
  });
  // glShaderSource(shader, count, const char* const* strings, const GLint* lengths): an array of guest pointers.
  AddSpecial("glShaderSource", [this](exec::MachineState& s, std::string* why) {
    const std::uint32_t shader = s.r[0], count = s.r[1], strings = s.r[2], lengths = s.r[3];
    if (count > 4096) {
      *why = "glShaderSource: count " + std::to_string(count) + " is absurd";
      return false;
    }
    std::vector<const char*> ptrs(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      std::uint32_t at = 0;
      if (!p_.memory().Read(strings + 4u * i, &at, 4) ||
          (ptrs[i] = static_cast<const char*>(p_.memory().HostForAccess(at))) == nullptr) {
        *why = "glShaderSource: string " + std::to_string(i) + " is not guest memory";
        return false;
      }
    }
    const void* lens = lengths == 0 ? nullptr : p_.memory().HostForAccess(lengths);
    using Fn = void (*)(unsigned, int, const char* const*, const int*);
    const auto f = reinterpret_cast<Fn>(HostSymbol("libGLESv2.so", "glShaderSource"));
    if (f == nullptr) {
      *why = "glShaderSource is not exported by this device's libGLESv2.so";
      return false;
    }
    f(shader, static_cast<int>(count), ptrs.data(), static_cast<const int*>(lens));
    return true;
  });
  // Unity uses this extension when Android advertises it. Keep the driver's pointers on the host side.
  const auto resolve_frame = [](const char* name) { return eglGetProcAddress(name); };
  const auto frame_supported = reinterpret_cast<PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC>(
      resolve_frame("eglGetFrameTimestampSupportedANDROID"));
  const auto next_frame = reinterpret_cast<PFNEGLGETNEXTFRAMEIDANDROIDPROC>(resolve_frame("eglGetNextFrameIdANDROID"));
  const auto frame_timestamps = reinterpret_cast<PFNEGLGETFRAMETIMESTAMPSANDROIDPROC>(
      resolve_frame("eglGetFrameTimestampsANDROID"));
  const auto frame_handles = [this](const exec::MachineState& s, EGLDisplay& display, EGLSurface& surface,
                                    std::string* why) {
    if (handles_.Get(s.r[0], &display) && handles_.Get(s.r[1], &surface)) return true;
    *why = "EGL frame timestamps: display or surface handle was never issued";
    return false;
  };
  if (frame_supported && next_frame && frame_timestamps) {
    AddSpecial("eglGetFrameTimestampSupportedANDROID",
               [frame_handles, frame_supported](exec::MachineState& s, std::string* why) {
      EGLDisplay display = EGL_NO_DISPLAY;
      EGLSurface surface = EGL_NO_SURFACE;
      if (!frame_handles(s, display, surface, why)) return false;
      s.r[0] = frame_supported(display, surface, static_cast<EGLint>(s.r[2]));
      return true;
    });
    AddSpecial("eglGetNextFrameIdANDROID", [this, frame_handles, next_frame](exec::MachineState& s, std::string* why) {
      EGLDisplay display = EGL_NO_DISPLAY;
      EGLSurface surface = EGL_NO_SURFACE;
      if (!frame_handles(s, display, surface, why)) return false;
      EGLuint64KHR id = 0;
      const EGLBoolean ok = next_frame(display, surface, &id);
      if (ok && !p_.memory().Write(s.r[2], &id, sizeof(id))) {
        *why = "eglGetNextFrameIdANDROID: frame ID output is not writable";
        return false;
      }
      s.r[0] = ok;
      return true;
    });
    AddSpecial("eglGetFrameTimestampsANDROID",
               [this, frame_handles, frame_timestamps](exec::MachineState& s, std::string* why) {
      EGLDisplay display = EGL_NO_DISPLAY;
      EGLSurface surface = EGL_NO_SURFACE;
      if (!frame_handles(s, display, surface, why)) return false;
      // The 64-bit frame ID occupies r2/r3. The count and both A32 pointers are on the guest stack.
      struct StackArgs { EGLint count; std::uint32_t names, values; } args{};
      if (!p_.memory().Read(s.r[13], &args, sizeof(args))) {
        *why = "eglGetFrameTimestampsANDROID: arguments are not readable";
        return false;
      }
      constexpr EGLint kMaxQueries = 256;
      if (args.count < 0 || args.count > kMaxQueries) {
        s.r[0] = EGL_FALSE;
        return true;
      }
      EGLint names[kMaxQueries];
      EGLnsecsANDROID values[kMaxQueries];
      const auto count = static_cast<std::size_t>(args.count);
      if (count && !p_.memory().Read(args.names, names, count * sizeof(*names))) {
        *why = "eglGetFrameTimestampsANDROID: timestamp names are not readable";
        return false;
      }
      const EGLuint64KHR id = s.r[2] | (static_cast<EGLuint64KHR>(s.r[3]) << 32);
      const EGLBoolean ok = frame_timestamps(display, surface, id, args.count, names, values);
      if (ok && count && !p_.memory().Write(args.values, values, count * sizeof(*values))) {
        *why = "eglGetFrameTimestampsANDROID: timestamp output is not writable";
        return false;
      }
      s.r[0] = ok;
      return true;
    });
  }

  // eglGetProcAddress: a guest address the guest can call — this registry's own slot for that name, NULL if none.
  AddSpecial("eglGetProcAddress", [this](exec::MachineState& s, std::string*) {
    const std::string name = GuestStringOrEmpty(p_.memory(), s.r[0]);
    const auto at = name.empty() ? std::nullopt : Resolve(name);
    if (!at && !name.empty() && live_log_) {  // once each: a NULL the engine may call later
      static std::set<std::string> said;
      if (said.insert(name).second) live_log_("egl: eglGetProcAddress(\"" + name + "\") = NULL (no host thunk)");
    }
    s.r[0] = at ? *at : 0u;
    return true;
  });
  // eglChooseConfig / eglGetConfigs: the configs come back as an array of host pointers, which become handles.
  const auto configs = [this](const char* which, bool choose) {
    return [this, which, choose](exec::MachineState& s, std::string* why) {
      void* dpy = nullptr;
      if (!handles_.Get(s.r[0], &dpy)) {
        *why = std::string{which} + ": display " + Hex(s.r[0]) + " was never issued";
        return false;
      }
      // choose: (dpy, attribs, configs, size, num_config on the stack); get: (dpy, configs, size, num_config)
      const std::uint32_t attribs = choose ? s.r[1] : 0;
      const std::uint32_t out = choose ? s.r[2] : s.r[1];
      std::uint32_t size = choose ? s.r[3] : s.r[2];
      std::uint32_t num = choose ? 0u : s.r[3];
      if (choose && !p_.memory().Read(s.r[13], &num, 4)) {
        *why = std::string{which} + ": the num_config argument on the stack is unreadable";
        return false;
      }
      if (size > 4096) size = 4096;
      std::vector<void*> host(out != 0 ? size : 0);
      int n = 0;
      unsigned ok = 0;
      if (choose) {
        using Fn = unsigned (*)(void*, const int*, void**, int, int*);
        const auto f = reinterpret_cast<Fn>(HostSymbol("libEGL.so", which));
        const void* a = attribs == 0 ? nullptr : p_.memory().HostForAccess(attribs);
        ok = f != nullptr ? f(dpy, static_cast<const int*>(a), out != 0 ? host.data() : nullptr,
                             static_cast<int>(host.size()), &n)
                          : 0u;
      } else {
        using Fn = unsigned (*)(void*, void**, int, int*);
        const auto f = reinterpret_cast<Fn>(HostSymbol("libEGL.so", which));
        ok = f != nullptr ? f(dpy, out != 0 ? host.data() : nullptr, static_cast<int>(host.size()), &n) : 0u;
      }
      for (int i = 0; ok != 0 && out != 0 && i < n; ++i) {
        const std::uint32_t h = handles_.Put(host[static_cast<std::size_t>(i)]);
        p_.memory().Write(out + 4u * static_cast<std::uint32_t>(i), &h, 4);
      }
      if (num != 0) p_.memory().Write(num, &n, 4);
      s.r[0] = ok;
      return true;
    };
  };
  AddSpecial("eglChooseConfig", configs("eglChooseConfig", true));
  AddSpecial("eglGetConfigs", configs("eglGetConfigs", false));

  // ---- the stand-in libnetd_client.so (dlopen/dlsym above): bionic's netdClientInit calls
  // netdClientInitDnsOpenProxy(&dispatch.dnsOpenProxy); the function it installs opens a stream to netd's
  // dnsproxyd, as the real library does, and the guest libc then speaks netd's protocol over it (host I/O).
  AddSpecial("rb_netdDnsOpenProxy", [](exec::MachineState& s, std::string*) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, "/dev/socket/dnsproxyd", sizeof addr.sun_path - 1);
    if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
      ::close(fd);
      s.r[0] = static_cast<std::uint32_t>(-1);
      return true;
    }
    s.r[0] = static_cast<std::uint32_t>(fd);
    return true;
  });
  AddSpecial("rb_netdClientInitDnsOpenProxy", [this](exec::MachineState& s, std::string* why) {
    const auto at = Resolve("rb_netdDnsOpenProxy");
    if (!at || s.r[0] == 0 || !p_.memory().Write(s.r[0], &*at, 4)) {
      *why = "netdClientInitDnsOpenProxy: the dispatch slot at " + Hex(s.r[0]) + " is not writable";
      return false;
    }
    return true;
  });
}

std::uint64_t HostSlots::CallsOf(const std::string& name) {
  std::lock_guard<std::mutex> lk(counters_mu_);
  const auto it = thunk_indices_.find(name);
  return it == thunk_indices_.end() ? 0 : thunk_calls_[it->second].calls.load(std::memory_order_relaxed);
}

void HostSlots::ReportThunks(const std::function<void(const std::string&)>& say) const {
  std::size_t imports;
  std::uint64_t calls = 0;
  std::vector<std::string> refusals;
  {
    std::lock_guard<std::mutex> lk(counters_mu_);
    imports = thunk_calls_.size();
    for (const ThunkCall& c : thunk_calls_) {
      calls += c.calls.load(std::memory_order_relaxed);
      if (c.refusals != 0) refusals.push_back("  " + c.name + ": " + std::to_string(c.refusals) + " refused — " + c.refused);
    }
  }
  if (imports == 0) return;
  say("thunks: " + std::to_string(imports) + " import(s) on host slots " + std::to_string(kThunkSlotBase) +
      ".., forwarded to this device's libraries, " + std::to_string(calls) + " call(s), " +
      std::to_string(handles_.size()) + " host object(s) behind handles");
  for (const auto& line : refusals) say(line);
}

void HostSlots::ReportDl(const std::function<void(const std::string&)>& say, std::uint32_t trap) const {
  unsigned called = 0, calls = 0;
  for (const DlCall& e : dl_calls_) {
    if (e.calls != 0) ++called;
    calls += e.calls;
  }
  say("dl: " + std::to_string(dl_calls_.size()) + " dynamic-linker import(s) are answered by host slots " +
      std::to_string(kDlSlotBase) + ".." + std::to_string(kDlSlotBase + dl_calls_.size() - 1) +
      " (this build ships no linker): dlopen/android_dlopen_ext NULL, dl_unwind_find_exidx from the placed"
      " modules' PT_ARM_EXIDX, dlsym/dladdr/exported-namespace NULL, dlclose 0, dlerror NULL, target SDK " +
      std::to_string(target_sdk_) + ", the two thread_local dtor hooks counted no-ops. Disclosed, not silently stubbed.");
  if (called == 0) {
    say("dl: none has been called yet — the set is bound, so a call arrives answered instead of faulting at 0x" +
        Hex(trap));
    return;
  }
  say("dl: " + std::to_string(called) + " of them have been called, " + std::to_string(calls) + " calls in total:");
  for (const DlCall& e : dl_calls_) {
    if (e.calls == 0) continue;
    if (!e.last_path.empty()) {
      say("  " + e.name + "(\"" + e.last_path + "\") x" + std::to_string(e.calls));
    } else if (e.name == "dl_unwind_find_exidx") {
      say("  " + e.name + " x" + std::to_string(e.calls) + " (" + std::to_string(e.answered) +
          " found a module's index; the rest were a pc no module holds)");
    } else {
      say("  " + e.name + " x" + std::to_string(e.calls));
    }
  }
}

void HostSlots::ReportLog(const std::function<void(const std::string&)>& say) const {
  if (log_.empty()) return;
  say("guest log: " + std::to_string(log_.size()) + " line(s) the guest wrote through __android_log_*" +
      (log_dropped_ != 0 ? ", " + std::to_string(log_dropped_) + " more dropped past the cap" : std::string{}) + ":");
  for (const LogLine& l : log_) say("  " + PrioName(l.prio) + "/" + l.tag + ": " + l.text);
}

}  // namespace rb::launcher
