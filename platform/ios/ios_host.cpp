// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The iOS host slots: see ios_host.h.

#include "ios_host.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "egl_shim.h"
#include "gl_symbols.h"
#include "rb/gfx/thunk_table.h"

namespace rb::platform::ios {

namespace {

std::string Hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%x", v);
  return buf;
}

/// A guest NUL-terminated string for a diagnostic line, bounded and refusal-free.
std::string GuestStringOrEmpty(const rb::mem::GuestMemory& mem, std::uint32_t at) {
  std::string out;
  if (at == 0 || at >= mem.window_bytes()) return out;
  for (unsigned i = 0; i < 256; ++i) {
    char c = 0;
    if (!mem.Read(at + i, &c, 1) || c == '\0') break;
    out.push_back(c);
  }
  return out;
}

/// Calls that wait (a vsync, a context switch) and read no guest memory: run with the GGL released.
bool Waits(const std::string& name) {
  static const std::set<std::string> k = {"eglSwapBuffers", "eglMakeCurrent", "eglInitialize", "eglTerminate",
                                          "eglWaitGL", "eglWaitClient", "eglWaitNative", "eglSwapInterval",
                                          "eglCreateWindowSurface", "eglDestroySurface", "eglDestroyContext",
                                          "glFinish", "glFlush"};
  return k.count(name) != 0;
}

// GL/EGL names whose values matter to the hand-written thunks below (the guest's numbers).

}  // namespace

void* IosHost::HostSymbol(const std::string& name) {
  if (void* shim = EglShim::Symbol(name)) return shim;
  return GlSymbol(name);
}

IosHost::IosHost(rb::runtime::GuestProcess& process, std::function<void(const std::string&)> log)
    : p_(process), log_(std::move(log)) {
  // glGetString/glGetStringi return a host string: copied into guest memory, as a device's driver's is not.
  AddSpecial("glGetString", [this](exec::MachineState& s, std::string* why) {
    using Fn = const unsigned char* (*)(unsigned);
    const auto f = reinterpret_cast<Fn>(HostSymbol("glGetString"));
    if (f == nullptr) {
      *why = "glGetString is not exported by OpenGLES.framework";
      return false;
    }
    const unsigned char* text = f(s.r[0]);
    if (text == nullptr) {
      s.r[0] = 0;
      return true;
    }
    s.r[0] = GuestString("glGetString:" + std::to_string(s.r[0]), reinterpret_cast<const char*>(text));
    return true;
  });
  AddSpecial("glGetStringi", [this](exec::MachineState& s, std::string*) {
    using Fn = const unsigned char* (*)(unsigned, unsigned);
    const auto f = reinterpret_cast<Fn>(HostSymbol("glGetStringi"));
    const unsigned char* text = f != nullptr ? f(s.r[0], s.r[1]) : nullptr;
    if (text == nullptr) {
      s.r[0] = 0;
      return true;
    }
    s.r[0] = GuestString("glGetStringi:" + std::to_string(s.r[0]) + ":" + std::to_string(s.r[1]),
                         reinterpret_cast<const char*>(text));
    return true;
  });
  AddSpecial("eglQueryString", [this](exec::MachineState& s, std::string* why) {
    void* dpy = nullptr;
    if (!handles_.Get(s.r[0], &dpy)) {
      *why = "eglQueryString: display " + Hex(s.r[0]) + " was never issued";
      return false;
    }
    const char* host = EglShim::Instance().QueryString(dpy, static_cast<std::int32_t>(s.r[1]));
    s.r[0] = host == nullptr ? 0u : GuestString("eglQueryString:" + Hex(s.r[0]) + ":" + std::to_string(s.r[1]), host);
    return true;  // 0 is EGL's own answer for a bad name; eglGetError says why
  });
  // eglGetProcAddress: an address the guest can call — this registry's slot for that name, 0 if none.
  AddSpecial("eglGetProcAddress", [this](exec::MachineState& s, std::string*) {
    const std::string name = GuestStringOrEmpty(p_.memory(), s.r[0]);
    const auto at = name.empty() ? std::nullopt : Resolve(name);
    if (!at && !name.empty()) {
      if (refused_.insert("proc:" + name).second && log_) {
        log_("egl: eglGetProcAddress(\"" + name + "\") = NULL (no iOS implementation)");
      }
    }
    s.r[0] = at ? *at : 0u;
    return true;
  });
  // eglChooseConfig / eglGetConfigs: the configs are host objects, so each becomes a handle the guest passes back.
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
      std::int32_t n = 0;
      std::uint32_t ok = 0;
      if (choose) {
        using Fn = std::uint32_t (*)(void*, const std::int32_t*, void**, std::int32_t, std::int32_t*);
        const auto f = reinterpret_cast<Fn>(HostSymbol(which));
        const void* a = attribs == 0 ? nullptr : p_.memory().Host(attribs);
        ok = f != nullptr ? f(dpy, static_cast<const std::int32_t*>(a), out != 0 ? host.data() : nullptr,
                              static_cast<std::int32_t>(host.size()), &n)
                          : 0u;
      } else {
        using Fn = std::uint32_t (*)(void*, void**, std::int32_t, std::int32_t*);
        const auto f = reinterpret_cast<Fn>(HostSymbol(which));
        ok = f != nullptr ? f(dpy, out != 0 ? host.data() : nullptr, static_cast<std::int32_t>(host.size()), &n) : 0u;
      }
      for (std::int32_t i = 0; ok != 0 && out != 0 && i < n; ++i) {
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
          (ptrs[i] = static_cast<const char*>(p_.memory().Host(at))) == nullptr) {
        *why = "glShaderSource: string " + std::to_string(i) + " is not guest memory";
        return false;
      }
    }
    const void* lens = lengths == 0 ? nullptr : p_.memory().Host(lengths);
    using Fn = void (*)(unsigned, int, const char* const*, const int*);
    const auto f = reinterpret_cast<Fn>(HostSymbol("glShaderSource"));
    if (f == nullptr) {
      *why = "glShaderSource is not exported by OpenGLES.framework";
      return false;
    }
    f(shader, static_cast<int>(count), ptrs.data(), static_cast<const int*>(lens));
    return true;
  });
}

std::optional<std::uint32_t> IosHost::Resolve(const std::string& name) {
  if (const auto it = slots_.find(name); it != slots_.end()) return p_.abi().SlotFor(it->second);

  const auto special = specials_.find(name);
  const rb::gfx::ThunkEntry* entry = special == specials_.end() ? rb::gfx::FindThunk(name) : nullptr;
  if (special == specials_.end() && entry == nullptr) return std::nullopt;

  if (kThunkSlotBase + slots_.size() >= abi::AbiContext::kMaxSlots) {
    if (log_) log_("thunks: no slot left for " + name);
    return std::nullopt;
  }
  const std::uint32_t index = kThunkSlotBase + static_cast<std::uint32_t>(slots_.size());
  Special fn;
  if (special != specials_.end()) {
    fn = special->second;
  } else {
    // The host function is looked up on first call, then kept (a name may only resolve once the app runs).
    auto host = std::make_shared<void*>(nullptr);
    fn = [this, entry, name, host](exec::MachineState& s, std::string* why) {
      if (*host == nullptr) *host = HostSymbol(name);
      if (*host == nullptr) {
        *why = name + " has no iOS implementation (" + entry->lib + " on a device)";
        return false;
      }
      return abi::ThunkCaller::Call(entry->spec, *host, s, p_.memory(), handles_, why);
    };
  }
  slots_.emplace(name, index);
  p_.abi().Bind(index,
                [this, name, fn](exec::MachineState& s, std::string* why) {
                  if (fn(s, why)) return true;
                  s.r[0] = 0;
                  if (refused_.insert(name).second && log_) log_("thunks: " + name + " refused: " + *why);
                  return true;
                },
                Waits(name) ? abi::AbiContext::SlotLock::kReleasesGgl : abi::AbiContext::SlotLock::kHoldsGgl);
  return p_.abi().SlotFor(index);
}

std::uint32_t IosHost::GuestString(const std::string& key, const std::string& text) {
  if (const auto it = strings_.find(key); it != strings_.end()) return it->second;
  std::string e;
  std::uint32_t at = p_.MapFree(static_cast<std::uint32_t>(text.size() + 1), mem::kRead | mem::kWrite, &e);
  if (at != 0 && !p_.memory().Write(at, text.c_str(), text.size() + 1)) at = 0;
  if (at != 0) strings_.emplace(key, at);
  return at;
}

}  // namespace rb::platform::ios
