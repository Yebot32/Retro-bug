// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See host_slots.h. Moved out of launcher.cpp (where `Inspect` bound these per run) so the in-app runtime
// answers the same names the same way.
#include "host_slots.h"
#include "thunks.h"

#include <dlfcn.h>

#include <cctype>
#include <cstdio>
#include <cstring>

namespace rb::launcher {
namespace {

std::string Hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%x", v);
  return buf;
}

std::string PrioName(int prio) {
  static const char* const kPrio[] = {"?", "?", "V", "D", "I", "W", "E", "F", "S"};
  return (prio >= 0 && prio <= 8) ? kPrio[prio] : "?";
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

/// Where a guest printf's variadic arguments live: optionally one core register first (r3 for
/// `__android_log_print(prio, tag, fmt, ...)`), then guest memory — the stack, or a `va_list`'s area.
/// AAPCS puts a 64-bit value 8-aligned and never splits it into r3, which is the one rule that matters.
struct GuestVarargs {
  const mem::GuestMemory* mem = nullptr;
  bool reg_free = false;
  std::uint32_t reg = 0;
  std::uint32_t at = 0;
  bool ok = true;
  std::uint32_t Next32() {
    if (reg_free) {
      reg_free = false;
      return reg;
    }
    std::uint32_t v = 0;
    if (!mem->Read(at, &v, 4)) ok = false;
    at += 4;
    return v;
  }
  std::uint64_t Next64() {
    reg_free = false;
    at = (at + 7u) & ~7u;
    std::uint64_t v = 0;
    if (!mem->Read(at, &v, 8)) ok = false;
    at += 8;
    return v;
  }
};

/// A guest `printf` format rendered on the host: flags, width and precision (including `*`), the h/hh/l/
/// ll/z/j/t/L lengths, and d i u o x X c s p f F e E g G a A %. `%s` reads a bounded guest string; `%n`
/// writes nothing (a log call has no business storing through a guest pointer here). A conversion this
/// does not know is copied through as text, so the line still says what the guest wrote.
std::string GuestFormat(const mem::GuestMemory& mem, std::uint32_t fmt_at, GuestVarargs& va) {
  const std::string fmt = GuestStringOrEmpty(mem, fmt_at);
  std::string out;
  for (std::size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') {
      out.push_back(fmt[i]);
      continue;
    }
    std::string spec = "%";
    std::size_t j = i + 1;
    while (j < fmt.size() && std::strchr("-+ #0", fmt[j]) != nullptr) spec.push_back(fmt[j++]);
    if (j < fmt.size() && fmt[j] == '*') {
      spec += std::to_string(static_cast<std::int32_t>(va.Next32()));
      ++j;
    } else {
      while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) spec.push_back(fmt[j++]);
    }
    if (j < fmt.size() && fmt[j] == '.') {
      spec.push_back(fmt[j++]);
      if (j < fmt.size() && fmt[j] == '*') {
        spec += std::to_string(static_cast<std::int32_t>(va.Next32()));
        ++j;
      } else {
        while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) spec.push_back(fmt[j++]);
      }
    }
    bool wide = false;  // a 64-bit integer argument
    while (j < fmt.size() && std::strchr("hlLqjzt", fmt[j]) != nullptr) {
      if (fmt[j] == 'l' && j + 1 < fmt.size() && fmt[j + 1] == 'l') {
        wide = true;
        ++j;
      } else if (fmt[j] == 'q' || fmt[j] == 'j' || fmt[j] == 'L') {
        wide = true;
      }
      ++j;  // h, hh, l, z, t are 32-bit on this ABI and promote to int
    }
    if (j >= fmt.size()) {
      out += fmt.substr(i);
      break;
    }
    const char c = fmt[j];
    char buf[512];
    buf[0] = 0;
    if (c == '%') {
      out.push_back('%');
    } else if (std::strchr("diuoxX", c) != nullptr) {
      const std::uint64_t raw = wide ? va.Next64() : va.Next32();
      const std::string s = spec + "ll" + c;
      if (c == 'd' || c == 'i') {
        const long long v = wide ? static_cast<long long>(raw)
                                 : static_cast<long long>(static_cast<std::int32_t>(raw));
        std::snprintf(buf, sizeof buf, s.c_str(), v);
      } else {
        std::snprintf(buf, sizeof buf, s.c_str(), static_cast<unsigned long long>(raw));
      }
      out += buf;
    } else if (c == 'c') {
      std::snprintf(buf, sizeof buf, (spec + "c").c_str(), static_cast<int>(va.Next32() & 0xffu));
      out += buf;
    } else if (c == 'p') {
      std::snprintf(buf, sizeof buf, "0x%x", va.Next32());
      out += buf;
    } else if (std::strchr("fFeEgGaA", c) != nullptr) {
      const std::uint64_t bits = va.Next64();  // `...` promoted float to double
      double d = 0;
      std::memcpy(&d, &bits, 8);
      std::snprintf(buf, sizeof buf, (spec + c).c_str(), d);
      out += buf;
    } else if (c == 's') {
      const std::uint32_t p = va.Next32();
      const std::string s = p == 0 ? std::string{"(null)"} : GuestStringOrEmpty(mem, p);
      std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
      out += buf;
    } else if (c == 'n') {
      va.Next32();
    } else {
      out += fmt.substr(i, j - i + 1);
    }
    i = j;
  }
  return out;
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

/// A guest NUL-terminated string for diagnostic reports, bounded and refusal-free: a report line may
/// not fault the run it describes, so an unreadable or absurd path yields the empty string. Only used
/// for naming what a guest asked the host for.
std::string GuestStringOrEmpty(const mem::GuestMemory& mem, std::uint32_t at) {
  std::string out;
  if (at == 0 || at >= mem.window_bytes()) return out;
  for (unsigned i = 0; i < 256; ++i) {
    char c = 0;
    if (!mem.Read(at + i, &c, 1) || c == '\0') break;
    out.push_back(c);
  }
  return out;
}

std::vector<std::string> NeededSonames(const std::vector<std::uint8_t>& img) {
  std::vector<std::string> out;
  const auto u16 = [&](std::size_t at) -> std::uint32_t {
    return at + 2 <= img.size() ? (img[at] | (img[at + 1] << 8)) : 0u;
  };
  const auto u32 = [&](std::size_t at) -> std::uint32_t {
    if (at + 4 > img.size()) return 0u;
    std::uint32_t v = 0;
    std::memcpy(&v, img.data() + at, 4);
    return v;
  };
  if (img.size() < 52) return out;
  const std::uint32_t phoff = u32(28), phentsize = u16(42), phnum = u16(44);
  std::uint32_t dyn_off = 0, dyn_sz = 0;
  const auto file_of = [&](std::uint32_t vaddr) -> std::size_t {
    for (std::uint32_t i = 0; i < phnum; ++i) {
      const std::size_t ph = phoff + std::size_t{i} * phentsize;
      if (u32(ph) != 1u) continue;  // PT_LOAD
      const std::uint32_t off = u32(ph + 4), va = u32(ph + 8), filesz = u32(ph + 16);
      if (vaddr >= va && vaddr - va < filesz) return off + (vaddr - va);
    }
    return 0;
  };
  for (std::uint32_t i = 0; i < phnum; ++i) {
    const std::size_t ph = phoff + std::size_t{i} * phentsize;
    if (u32(ph) == 2u) {  // PT_DYNAMIC
      dyn_off = u32(ph + 4);
      dyn_sz = u32(ph + 16);
    }
  }
  std::uint32_t strtab = 0;
  std::vector<std::uint32_t> needed;
  for (std::size_t at = dyn_off; dyn_off != 0 && at + 8 <= std::size_t{dyn_off} + dyn_sz; at += 8) {
    const std::uint32_t tag = u32(at), val = u32(at + 4);
    if (tag == 0u) break;
    if (tag == 1u) needed.push_back(val);  // DT_NEEDED
    if (tag == 5u) strtab = val;           // DT_STRTAB
  }
  const std::size_t str = strtab != 0 ? file_of(strtab) : 0;
  if (str == 0) return out;
  for (const std::uint32_t n : needed) {
    std::size_t at = str + n;
    std::string name;
    while (at < img.size() && img[at] != 0 && name.size() < 256) name.push_back(static_cast<char>(img[at++]));
    if (!name.empty()) out.push_back(name);
  }
  return out;
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
          // on; one that does not faults on the NULL it asked to be lied to about.
          c.last_path = GuestStringOrEmpty(mem, s.r[0]);
          s.r[0] = 0;
          return true;
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

std::optional<std::uint32_t> HostSlots::Resolve(const std::string& name) {
  if (const auto dl = DlSlotIndex(name)) return p_.abi().SlotFor(kDlSlotBase + static_cast<std::uint32_t>(*dl));
  if (const auto lg = LogSlotIndex(name)) return p_.abi().SlotFor(kLogSlotBase + static_cast<std::uint32_t>(*lg));
  if (!thunks_on_) return std::nullopt;
  for (std::size_t i = 0; i < thunk_calls_.size(); ++i) {
    if (thunk_calls_[i].name == name) return p_.abi().SlotFor(kThunkSlotBase + static_cast<std::uint32_t>(i));
  }
  const auto special = specials_.find(name);
  const ThunkEntry* entry = special == specials_.end() ? FindThunk(name) : nullptr;
  if (special == specials_.end() && entry == nullptr) return std::nullopt;  // no thunk: stays trapped, by name
  if (thunk_calls_.size() >= kThunkSlots) {
    if (live_log_) live_log_("thunks: no slot left for " + name + " (" + std::to_string(kThunkSlots) + " in use)");
    return std::nullopt;
  }
  const std::size_t index = thunk_calls_.size();
  thunk_calls_.push_back(ThunkCall{name, 0, {}, 0});
  Special fn;
  if (special != specials_.end()) {
    fn = special->second;
  } else {
    // The host function is looked up on first call (a library may only be loadable once the app runs), then kept.
    auto host = std::make_shared<void*>(nullptr);
    fn = [this, entry, name, host](exec::MachineState& s, std::string* why) {
      if (*host == nullptr) *host = HostSymbol(entry->lib, name);
      if (*host == nullptr) {
        *why = name + " is not exported by this device's " + entry->lib;
        return false;
      }
      return abi::ThunkCaller::Call(entry->spec, *host, s, p_.memory(), handles_, why);
    };
  }
  // `kHoldsGgl` (M2 Task 4, ruling i, as the launcher's GL slots): the host reads guest memory through translated
  // pointers — the call's own, and client arrays an earlier gl*Pointer registered, read at draw time — which
  // another guest thread could unmap with the GGL released; and the counters below are run state. The index,
  // not a reference, is captured: a later push_back may move the vector.
  p_.abi().Bind(kThunkSlotBase + static_cast<std::uint32_t>(index), [this, index, fn](exec::MachineState& s,
                                                                                       std::string*) {
    ThunkCall& c = thunk_calls_[index];
    ++c.calls;
    std::string why;
    if (fn(s, &why)) return true;
    s.r[0] = 0;
    if (c.refusals++ == 0) {
      c.refused = why;
      if (live_log_) live_log_("thunks: " + c.name + " refused: " + why);
    }
    return true;
  }, abi::AbiContext::SlotLock::kHoldsGgl);
  return p_.abi().SlotFor(kThunkSlotBase + static_cast<std::uint32_t>(index));
}

std::uint32_t HostSlots::GuestString(const std::string& key, const std::string& text) {
  if (const auto it = guest_strings_.find(key); it != guest_strings_.end()) return it->second;
  std::string e;
  std::uint32_t at = p_.MapFree(static_cast<std::uint32_t>(text.size() + 1), mem::kRead | mem::kWrite, &e);
  if (at != 0 && !p_.memory().Write(at, text.c_str(), text.size() + 1)) at = 0;
  if (at != 0) guest_strings_.emplace(key, at);
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
    if (host == nullptr) {
      *why = "glGetString(0x" + Hex(s.r[0]) + ") returned NULL (no current context on this thread?)";
      return false;
    }
    s.r[0] = GuestString("gl:" + std::to_string(s.r[0]), reinterpret_cast<const char*>(host));
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
    s.r[0] = host == nullptr ? 0u : GuestString("egl:" + Hex(s.r[0]) + ":" + std::to_string(s.r[1]), host);
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
          (ptrs[i] = static_cast<const char*>(p_.memory().Host(at))) == nullptr) {
        *why = "glShaderSource: string " + std::to_string(i) + " is not guest memory";
        return false;
      }
    }
    const void* lens = lengths == 0 ? nullptr : p_.memory().Host(lengths);
    using Fn = void (*)(unsigned, int, const char* const*, const int*);
    const auto f = reinterpret_cast<Fn>(HostSymbol("libGLESv2.so", "glShaderSource"));
    if (f == nullptr) {
      *why = "glShaderSource is not exported by this device's libGLESv2.so";
      return false;
    }
    f(shader, static_cast<int>(count), ptrs.data(), static_cast<const int*>(lens));
    return true;
  });
  // eglGetProcAddress: a guest address the guest can call — this registry's own slot for that name, NULL if none.
  AddSpecial("eglGetProcAddress", [this](exec::MachineState& s, std::string*) {
    const std::string name = GuestStringOrEmpty(p_.memory(), s.r[0]);
    const auto at = name.empty() ? std::nullopt : Resolve(name);
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
        const void* a = attribs == 0 ? nullptr : p_.memory().Host(attribs);
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
}

void HostSlots::ReportThunks(const std::function<void(const std::string&)>& say) const {
  if (thunk_calls_.empty()) return;
  unsigned calls = 0;
  for (const ThunkCall& c : thunk_calls_) calls += c.calls;
  say("thunks: " + std::to_string(thunk_calls_.size()) + " import(s) on host slots " + std::to_string(kThunkSlotBase) +
      ".., forwarded to this device's libraries, " + std::to_string(calls) + " call(s), " +
      std::to_string(handles_.size()) + " host object(s) behind handles");
  for (const ThunkCall& c : thunk_calls_) {
    if (c.refusals != 0) say("  " + c.name + ": " + std::to_string(c.refusals) + " refused — " + c.refused);
  }
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
