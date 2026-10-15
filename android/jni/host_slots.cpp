// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See host_slots.h. Moved out of launcher.cpp (where `Inspect` bound these per run) so the in-app runtime
// answers the same names the same way.
#include "host_slots.h"

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
    });
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
    });
  }
}

std::optional<std::uint32_t> HostSlots::Resolve(const std::string& name) const {
  if (const auto dl = DlSlotIndex(name)) return p_.abi().SlotFor(kDlSlotBase + static_cast<std::uint32_t>(*dl));
  if (const auto lg = LogSlotIndex(name)) return p_.abi().SlotFor(kLogSlotBase + static_cast<std::uint32_t>(*lg));
  return std::nullopt;
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
