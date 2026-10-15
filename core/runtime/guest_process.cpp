/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * rb::runtime::GuestProcess — see the header. The libc boot that used to live only inside the launcher's
 * `Inspect()` (scope load, IFUNC resolution, cross-module IFUNC exports, the `__loader_shared_globals`
 * stub, the auxv decode, constructors) is here as a component, and the thread-pointer hack it ended with
 * (a zero page standing in for both the thread struct and `bionic_tls`) is replaced by bionic's real
 * main-thread structures.
 */
#include "rb/runtime/guest_process.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

#include "rb/linker/guest_linker.h"

namespace rb::runtime {
namespace {

std::string Hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%x", v);
  return buf;
}

std::uint32_t RoundUp(std::uint32_t v, std::uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

unsigned HostPageShift() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = mem::kGuestPageShift;
  while ((1L << shift) < ps) ++shift;
  return shift;
}

const char* ReasonName(exec::StopReason r) {
  switch (r) {
    case exec::StopReason::Budget: return "Budget";
    case exec::StopReason::Returned: return "Returned";
    case exec::StopReason::HostCall: return "HostCall";
    case exec::StopReason::Syscall: return "Syscall";
    case exec::StopReason::Suspended: return "Suspended";
    case exec::StopReason::Unimplemented: return "Unimplemented";
    case exec::StopReason::Fault: return "Fault";
    case exec::StopReason::Internal: return "Internal";
    default: return "Exited";
  }
}

// ------------------------------------------------------------------------------------ layout facts
//
// The facts below were read off the tablet's (SM-X710, Android 16) `libc.so` (md5 9deefb3e…) with
// `llvm-objdump --triple=thumbv7a`, and cross-checked against the same file running natively on that
// tablet (a 32-bit process's own main thread through `__libc_get_static_tls_bounds`, `dl_iterate_phdr` and
// TPIDRURW). Each is checked against this image's own instructions — a Thumb-2 encoding inside the named
// function — so a different build is a refusal that says which fact moved.

constexpr std::uint32_t kThreadSize = 0x2c8;       // sizeof(pthread_internal_t)
constexpr std::uint32_t kTpOffsetInTcb = 0xc;      // TP = tcb + 0xc: slots -3..-1 below TLS_SLOT_DTV
constexpr std::uint32_t kTcbSize = 0x2c;           // 11 slots, -3..7 (TLS_SLOT_ART_THREAD_SELF = 7)
constexpr std::uint32_t kBionicTlsLastByte = 0xb49;  // bionic_systrace_disabled; then char padding[2]
constexpr std::uint32_t kBionicTlsSize = 0xb4c;
constexpr std::uint32_t kStaticTlsAlign = 4;       // alignof(bionic_tcb) == alignof(bionic_tls) == 4
constexpr std::uint32_t kGlobalsStaticTlsSize = 0x420;
constexpr std::uint32_t kGlobalsOffsetTcb = 0x428;
constexpr std::uint32_t kGlobalsOffsetTls = 0x42c;
constexpr std::uint32_t kGlobalsTlsModuleCount = 0x464;
/// __allocate_thread_mapping(stack, guard): mmap_size = round(static_tls + stack + guard + 0x3000); the
/// static TLS sits 0x3000 below the top: 0x2000 of basename/dirname buffers (bionic_tls+0x414) and a
/// 0x1000 guard page.
constexpr std::uint32_t kMappingTop = 0x3000;
constexpr std::uint32_t kPthreadGuard = 0x1000;

// pthread_internal_t fields (the main thread's are written by the host, as the linker does).
constexpr std::uint32_t kThNext = 0x0, kThPrev = 0x4, kThTid = 0x8, kThCachedPid = 0xc, kThAttrFlags = 0x10;
constexpr std::uint32_t kThMmapBase = 0x60, kThMmapSize = 0x64, kThMmapBaseUnguarded = 0x68;
constexpr std::uint32_t kThMmapSizeUnguarded = 0x6c, kThBionicTls = 0x298, kThBionicTcb = 0x2a0;
constexpr std::uint32_t kBtTempBuffers = 0x414;    // bionic_tls: basename/dirname scratch (0x2000)
constexpr std::uint32_t kTcbBionicTls = 0x8;       // slot -1, TP-4
constexpr std::uint32_t kTcbStackGuard = 0x20;     // slot 5, TP+0x14
constexpr std::uint32_t kPthreadAttrFlagInherit = 0x4;  // what the device's main thread carries

struct Fact {
  const char* fn;     ///< the function whose code states it (a .symtab name)
  std::vector<std::uint16_t> code;
  const char* says;   ///< the fact, for the refusal
};

/// Halfword search inside the function's own bytes ([addr, addr+size)).
bool Contains(const mem::GuestMemory& mem, const loader::LocalSymbol& fn, const std::vector<std::uint16_t>& code) {
  const std::uint32_t size = fn.size != 0 ? fn.size : 0x400u;
  std::vector<std::uint16_t> body(size / 2);
  if (body.empty() || !mem.Read(fn.addr, body.data(), body.size() * 2)) return false;
  if (code.size() > body.size()) return false;
  for (std::size_t i = 0; i + code.size() <= body.size(); ++i) {
    if (std::equal(code.begin(), code.end(), body.begin() + static_cast<std::ptrdiff_t>(i))) return true;
  }
  return false;
}

}  // namespace

std::optional<BionicLayout> VerifyBionicLayout(const mem::GuestMemory& mem, const loader::GuestModule& libc,
                                               std::string* err) {
  const auto refuse = [&](const std::string& why) -> std::optional<BionicLayout> {
    if (err) *err = "bionic layout mismatch: " + why + " — this libc.so is not the build GuestProcess's layout"
                    " was derived from, and a guessed layout would corrupt threads silently";
    return std::nullopt;
  };
  if (!libc.local_symbols) return refuse("libc.so has no .symtab, so its internals cannot be found by name");

  static const Fact kFacts[] = {
      {"__libc_get_static_tls_bounds", {0xf8d0, 0x1428}, "StaticTlsLayout::offset_bionic_tcb at shared-globals +0x428 (`ldr.w r1, [r0, #0x428]`)"},
      {"__libc_get_static_tls_bounds", {0x390c}, "TP = bionic_tcb + 0xc (`subs r1, #0xc`)"},
      {"__libc_get_static_tls_bounds", {0xf8d0, 0x0420}, "the static TLS size at shared-globals +0x420 (`ldr.w r0, [r0, #0x420]`)"},
      {"pthread_create", {0xf8d0, 0x4428}, "pthread_create reads offset_bionic_tcb from +0x428"},
      {"pthread_create", {0xf8d0, 0xa42c}, "StaticTlsLayout::offset_bionic_tls at shared-globals +0x42c (`ldr.w r10, [r0, #0x42c]`)"},
      {"pthread_create", {0xf5a0, 0x7032}, "sizeof(pthread_internal_t) == 0x2c8 (`sub.w r0, r0, #0x2c8`)"},
      {"pthread_create", {0xf8c1, 0x42a0}, "pthread_internal_t::bionic_tcb at +0x2a0"},
      {"pthread_create", {0xf8c0, 0x1298}, "pthread_internal_t::bionic_tls at +0x298"},
      {"pthread_create", {0x6220}, "the stack guard at bionic_tcb + 0x20 = TP + 0x14 (`str r0, [r4, #0x20]`)"},
      {"pthread_create", {0x60a1}, "the bionic_tls slot at bionic_tcb + 8 = TP - 4 (`str r1, [r4, #8]`)"},
      {"pthread_create", {0xf104, 0x010c}, "clone's tls argument = bionic_tcb + 0xc (`add.w r1, r4, #0xc`)"},
      {"pthread_create", {0xf640, 0x7200}, "clone flags low half 0x0f00"},
      {"pthread_create", {0xf2c0, 0x023d}, "clone flags high half 0x003d (flags 0x003d0f00)"},
      {"pthread_create", {0xe9c6, 0x080c}, "pthread_internal_t::start_routine/arg at +0x30/+0x34"},
      {"__init_tcb", {0x6101, 0x4770}, "TLS_SLOT_THREAD_ID at bionic_tcb + 0x10 = TP + 4"},
      {"__errno", {0xee1d, 0x0f70, 0x6840, 0xf500, 0x7027}, "errno at pthread_internal_t + 0x29c"},
      {"gettid", {0x68a0}, "pthread_internal_t::tid at +0x8"},
      {"__pthread_internal_add", {0xe9c4, 0x0200}, "pthread_internal_t::next/prev at +0x0/+0x4"},
      {"__libc_preinit_impl", {0xf880, 0x5b49}, "bionic_tls's last field (bionic_systrace_disabled) at +0xb49"},
      {"__allocate_thread_mapping", {0xf8d0, 0x1420}, "__allocate_thread_mapping sizes static TLS from +0x420"},
      {"__allocate_thread_mapping", {0xf5a0, 0x5040}, "static TLS placed 0x3000 below the thread mapping's end"},
      {"basename", {0xf8d1, 0x4414}, "bionic_tls's temp-buffer pointer at +0x414"},
      {"__init_static_tls", {0xf8d5, 0x0464}, "the TLS module count at shared-globals +0x464"},
      {"__init_static_tls", {0xf8d5, 0x146c}, "the TLS module array at shared-globals +0x46c"},
  };
  for (const Fact& f : kFacts) {
    const loader::LocalSymbol* fn = libc.FindLocalSymbolEntry(f.fn);
    if (fn == nullptr) return refuse(std::string{f.fn} + " is not in libc.so's .symtab (needed for: " + f.says + ")");
    if (!Contains(mem, *fn, f.code)) {
      return refuse(std::string{f.says} + " — not found in " + f.fn + " at " + Hex(fn->addr));
    }
  }
  const loader::LocalSymbol* list = libc.FindLocalSymbolEntry("g_thread_list");
  if (list == nullptr || list->is_function || list->size != 4) {
    return refuse("g_thread_list is not a 4-byte object in libc.so's .symtab");
  }

  BionicLayout l;
  l.thread_size = kThreadSize;
  l.tp_offset_in_tcb = kTpOffsetInTcb;
  l.tcb_size = kTcbSize;
  l.bionic_tls_size = RoundUp(kBionicTlsLastByte + 1u + 2u /* char padding[2] */, kStaticTlsAlign);
  if (l.bionic_tls_size != kBionicTlsSize) return refuse("sizeof(bionic_tls) did not come out as 0xb4c");
  // StaticTlsLayout with no executable TLS segment and no module: reserve_exe_segment_and_tcb(nullptr)
  // reserves the tcb at 0, reserve_bionic_tls() follows it, finish_layout() rounds to the alignment.
  l.offset_bionic_tcb = 0;
  l.offset_bionic_tls = RoundUp(l.offset_bionic_tcb + l.tcb_size, kStaticTlsAlign);
  l.static_tls_size = RoundUp(l.offset_bionic_tls + l.bionic_tls_size, kStaticTlsAlign);

  // getauxval: `bl __libc_shared_globals` then `ldr.w rX, [rX, #imm]` — the auxv pointer's offset.
  const auto getauxval = libc.FindExport("getauxval");
  if (!getauxval) return refuse("libc.so exports no getauxval");
  std::uint16_t insn[16] = {};
  if (!mem.Read(getauxval->addr, insn, sizeof insn)) return refuse("getauxval's code is unreadable");
  for (unsigned i = 0; i + 3 < 16 && l.auxv_field == 0; ++i) {
    if ((insn[i] & 0xF800u) != 0xF000u || (insn[i + 1] & 0xD000u) != 0xD000u) continue;  // BL / BLX
    if ((insn[i + 2] & 0xFFF0u) != 0xF8D0u) continue;                                      // LDR.W imm12
    const unsigned rn = insn[i + 2] & 0xFu, rt = (insn[i + 3] >> 12) & 0xFu;
    const std::uint32_t off = insn[i + 3] & 0xFFFu;
    if (rn == rt && off != 0 && (off & 3u) == 0) l.auxv_field = off;
  }
  if (l.auxv_field == 0) return refuse("getauxval does not read the auxv as `bl; ldr.w rX, [rX, #imm]`");
  if (l.auxv_field >= kGlobalsStaticTlsSize && l.auxv_field < kGlobalsTlsModuleCount + 8u) {
    return refuse("getauxval's auxv field " + Hex(l.auxv_field) + " overlaps the static-TLS fields");
  }
  return l;
}

// ------------------------------------------------------------------------------------ GuestProcess

GuestProcess::~GuestProcess() = default;

void GuestProcess::Log(const std::string& line) const {
  if (opts_.log) opts_.log(line);
}

exec::Executor& GuestProcess::engine(int tier) {
  if (tier == 2) return *jit_;
  return *interp_;
}

std::unique_ptr<GuestProcess> GuestProcess::Create(const GuestProcessOptions& options, std::string* err) {
  const auto fail = [&](const std::string& why) -> std::unique_ptr<GuestProcess> {
    if (err) *err = "GuestProcess::Create: " + why;
    return nullptr;
  };
  if (options.window_bytes < (32u << 20)) return fail("the window must be at least 32 MiB");
  if (options.ctor_tier != 1 && options.ctor_tier != 2) return fail("ctor_tier must be 1 (I) or 2 (J)");

  std::unique_ptr<GuestProcess> p{new GuestProcess()};
  p->opts_ = options;
  mem::AddressModel model;
  model.kind = options.biased ? mem::AddressModel::kBiased : mem::AddressModel::kFlatIdentity;
  model.host_page_shift = HostPageShift();
  std::string e;
  p->mem_ = mem::GuestMemory::Create(options.window_bytes, model, &e);
  if (!p->mem_) return fail("GuestMemory::Create(" + std::to_string(options.window_bytes) + "): " + e);
  mem::GuestMemory& m = *p->mem_;

  // The host-entry window (return sentinel + slot table): reserved so no placement lands on it.
  if (!m.Map(kReturnAddress, 0x2000, mem::kNone, &e)) return fail("reserving the host-entry window: " + e);
  // Nothing at or above kAllocCeiling: reserved outright when the window reaches it, so neither this
  // allocator, the loader's placement scan nor the monitor's mmap2 can hand it out.
  p->ceiling_ = static_cast<GuestAddr>(std::min<std::size_t>(options.window_bytes, kAllocCeiling));
  if (options.window_bytes > kAllocCeiling &&
      !m.Map(kAllocCeiling, options.window_bytes - kAllocCeiling, mem::kNone, &e)) {
    return fail("reserving the window above " + Hex(kAllocCeiling) + ": " + e);
  }
  const GuestAddr trap = options.unresolved_trap & ~(mem::kGuestPage - 1u);
  if (options.unresolved_trap != 0 && trap < p->ceiling_ && !m.IsMapped(trap) &&
      !m.Map(trap, mem::kGuestPage, mem::kNone, &e)) {
    return fail("reserving the unresolved-import trap page " + Hex(trap) + ": " + e);
  }
  // The main thread's stack, 1 MiB below the ceiling, with a guard page under it.
  p->stack_top_ = p->ceiling_ - 0x0010'0000u;
  const GuestAddr stack_lo = p->stack_top_ - kMainStackBytes;
  if (!m.Map(stack_lo, kMainStackBytes, mem::kRead | mem::kWrite, &e) ||
      !m.Map(stack_lo - mem::kGuestPage, mem::kGuestPage, mem::kNone, &e)) {
    return fail("mapping the main stack below " + Hex(p->stack_top_) + ": " + e);
  }

  p->abi_ = std::make_unique<abi::AbiContext>(m, p->stack_top_, kReturnAddress);
  p->monitor_ = std::make_unique<syscall::Monitor>();
  p->monitor_->set_memory(&m);
  p->monitor_->set_ids(kPid, kMainTid);
  p->abi_->set_monitor(p->monitor_.get());

  // The JNIEnv maps its own table and arena from a page-aligned base; a free run is found for it now,
  // before any module is placed.
  const std::size_t jni_bytes = kJniArenaBytes + 2u * mem::kGuestPage;
  const GuestAddr jni_at = p->FindFree(jni_bytes);
  if (jni_at == 0) return fail("no free run of " + std::to_string(jni_bytes) + " bytes for the JNIEnv");
  std::string jerr;
  p->jni_ = std::make_unique<abi::JniEnv>(m, *p->abi_, jni_at, kJniArenaBytes, &jerr);
  if (!p->jni_->ok()) return fail("the JNIEnv could not be built: " + jerr);

  p->interp_ = std::make_unique<exec::InterpExecutor>(m, kReturnAddress, p->abi_.get());
  p->jit_ = std::make_unique<exec::JitExecutor>(m, kReturnAddress, p->abi_.get());
  if (options.ctor_tier == 2 && !p->jit_->ok()) return fail("Tier J is unavailable: " + p->jit_->error());
  p->abi_->set_executor(p->engine(options.ctor_tier));
  GuestProcess* self = p.get();
  p->monitor_->set_invalidator([self](GuestAddr from, GuestAddr to) {
    self->interp_->Invalidate(from, to);
    if (self->jit_->ok()) self->jit_->Invalidate(from, to);
  });
  return p;
}

GuestAddr GuestProcess::FindFree(std::size_t bytes) const {
  const std::uint32_t size = RoundUp(static_cast<std::uint32_t>(bytes), mem::kGuestPage);
  constexpr GuestAddr kFloor = 0x0001'0000u;
  for (GuestAddr cand = kFloor; static_cast<std::uint64_t>(cand) + size <= ceiling_; cand += mem::kGuestPage) {
    bool free = true;
    for (GuestAddr pg = cand; pg < cand + size; pg += mem::kGuestPage) {
      if (mem_->IsMapped(pg)) {
        free = false;
        cand = pg;  // resume past the occupied page
        break;
      }
    }
    if (free) return cand;
  }
  return 0;
}

GuestAddr GuestProcess::MapFree(std::size_t bytes, unsigned prot, std::string* err) {
  const GuestAddr at = FindFree(bytes);
  if (at == 0) {
    if (err) *err = "no free run of " + std::to_string(bytes) + " bytes below " + Hex(ceiling_);
    return 0;
  }
  std::string e;
  if (!mem_->Map(at, bytes, prot, &e)) {
    if (err) *err = "mapping " + std::to_string(bytes) + " bytes at " + Hex(at) + ": " + e;
    return 0;
  }
  return at;
}

std::optional<std::uint32_t> GuestProcess::RunIfunc(const loader::IfuncSlot& slot) {
  const std::string name = slot.name.empty() ? "<R_ARM_IRELATIVE>" : slot.name;
  abi_->set_executor(engine(opts_.ctor_tier));
  exec::RunResult rr{};
  exec::MachineState exit{};
  const abi::Ret32 ret = abi_->CallGuest(slot.resolver.tagged(), {abi::Value32::U32(slot.slot_value)},
                                         abi::Shorty::For("II"), kCallBudget, &rr, &exit);
  if (rr.reason == exec::StopReason::Returned && ret.w() != 0) {
    Log("ifunc: " + name + " -> " + Hex(ret.w()) + " (resolver " + Hex(slot.resolver.addr) + ")");
    return ret.w();
  }
  Log("ifunc: " + name + " NOT resolved (resolver " + Hex(slot.resolver.addr) + "): " +
      (rr.reason == exec::StopReason::Returned ? std::string{"returned 0"}
                                               : std::string{ReasonName(rr.reason)} + " " + rr.detail) +
      " — the slot still holds the resolver");
  return std::nullopt;
}

std::optional<std::uint32_t> GuestProcess::ExportOf(const loader::GuestModule& m, const std::string& name) {
  const auto fn = m.FindExport(name);
  if (!fn) return std::nullopt;
  if (!linker::UnresolvedIfuncExport(m, name)) return fn->tagged();
  // FindExport handed back the resolver itself: run it, once per name, as bionic's linker binds an
  // import of another module's IFUNC to what the resolver returns.
  const std::string base = name.substr(0, name.find('@'));
  if (const auto hit = export_ifunc_cache_.find(base); hit != export_ifunc_cache_.end()) return hit->second;
  loader::IfuncSlot slot{};
  slot.name = base;
  slot.resolver = *fn;
  slot.slot_value = fn->tagged();
  const auto impl = RunIfunc(slot);
  if (!impl) return fn->tagged();  // named in the log; the resolver stays bound
  export_ifunc_cache_[base] = *impl;
  return *impl;
}

std::optional<GuestAddr> GuestProcess::Resolve(const std::string& name) {
  for (const auto& m : modules_) {
    if (const auto a = ExportOf(*m, name)) return *a;
  }
  return std::nullopt;
}

std::optional<std::uint32_t> GuestProcess::ResolveImport(const std::string& name) {
  if (const auto a = Resolve(name)) return a;
  // The one linker-provided function libc imports that this process answers itself.
  if (name == "__loader_shared_globals" && stub_ != 0) return stub_;
  if (opts_.host_resolver) {
    const auto h = opts_.host_resolver(name);
    if (h && (opts_.unresolved_trap == 0 || *h != opts_.unresolved_trap)) return h;
  }
  if (std::find(unanswered_.begin(), unanswered_.end(), name) == unanswered_.end()) {
    unanswered_.push_back(name);
  }
  if (opts_.unresolved_trap != 0) return opts_.unresolved_trap;
  return std::nullopt;
}

loader::GuestModule* GuestProcess::LoadOne(const std::vector<std::uint8_t>& image, const std::string& soname,
                                           bool scope, std::string* err) {
  loader::LoadOptions opts;
  // A real libc carries TLS relocations this loader cannot apply; the scope tolerates them (they stay
  // recorded in `unsupported_relocs`). An app does not: a refused relocation fails its load by name.
  opts.tolerate_unresolved_relocs = scope;
  opts.unresolved_trap = opts_.unresolved_trap;
  opts.resolve = [this](const std::string& name) { return ResolveImport(name); };
  opts.resolve_ifunc = [this](const loader::IfuncSlot& s) { return RunIfunc(s); };
  std::string e;
  auto mod = loader::LoadFromBytes(*mem_, image, soname, opts, &e);
  if (!mod) {
    if (err) *err = soname + " did not load: " + e;
    return nullptr;
  }
  std::string tolerated;
  for (const std::string& one : mod->unsupported_relocs) tolerated += (tolerated.empty() ? "" : ", ") + one;
  Log("load: " + soname + " at " + Hex(mod->base) + " extent " + Hex(mod->extent) +
      (tolerated.empty() ? std::string{} : "; relocations tolerated, not applied: " + tolerated));
  modules_.push_back(std::make_unique<loader::GuestModule>(std::move(*mod)));
  return modules_.back().get();
}

bool GuestProcess::LoadScope(const std::vector<std::uint8_t>& libc, const std::vector<std::uint8_t>& libm,
                             const std::vector<std::uint8_t>& libz, const std::vector<std::uint8_t>& libstdcxx,
                             std::string* err) {
  const auto fail = [&](const std::string& why) {
    if (err) *err = "LoadScope: " + why;
    Log("LoadScope: " + why);
    return false;
  };
  if (scope_loaded_ || !modules_.empty()) return fail("the scope is loaded once per process, before any app");
  if (libc.empty()) return fail("no libc.so image");
  if (libm.empty()) return fail("no libm.so image");
  if (libz.empty()) return fail("no libz.so image");

  // The linker's shared globals (zeroed, two pages: libc writes up to +0x564) and the function that
  // returns them, `__loader_shared_globals`, as twelve bytes of ARM:
  //   ldr r0, [pc, #0] ; bx lr ; .word <globals>
  // ARM state (bit 0 clear), so a Thumb `blx` switches state and `bx lr` returns to the caller's mode.
  std::string e;
  globals_ = MapFree(2u * mem::kGuestPage, mem::kRead | mem::kWrite, &e);
  if (globals_ == 0) return fail("the shared-globals page: " + e);
  stub_ = MapFree(mem::kGuestPage, mem::kRead | mem::kWrite, &e);
  if (stub_ == 0) return fail("the __loader_shared_globals stub page: " + e);
  const std::uint32_t code[] = {0xe59f'0000u, 0xe12f'ff1eu, globals_};
  if (!mem_->Write(stub_, code, sizeof code) || !mem_->Protect(stub_, mem::kGuestPage, mem::kRead | mem::kExec, &e)) {
    return fail("writing the __loader_shared_globals stub: " + e);
  }
  auxv_ = MapFree(mem::kGuestPage, mem::kRead | mem::kWrite, &e);
  if (auxv_ == 0) return fail("the auxv page: " + e);

  libc_ = LoadOne(libc, "libc.so", true, &e);
  if (libc_ == nullptr) return fail(e);
  const auto layout = VerifyBionicLayout(*mem_, *libc_, &e);
  if (!layout) return fail("libc.so: " + e);
  layout_ = *layout;
  Log("bionic: pthread_internal_t " + Hex(layout_.thread_size) + ", bionic_tcb " + Hex(layout_.tcb_size) +
      " (TP = tcb + " + Hex(layout_.tp_offset_in_tcb) + "), bionic_tls " + Hex(layout_.bionic_tls_size) +
      "; static TLS " + Hex(layout_.static_tls_size) + " (tcb at " + Hex(layout_.offset_bionic_tcb) +
      ", bionic_tls at " + Hex(layout_.offset_bionic_tls) + ", no TLS module); auxv at globals + " +
      Hex(layout_.auxv_field));
  if (!PublishAuxv(&e)) return fail(e);

  if (LoadOne(libm, "libm.so", true, &e) == nullptr) return fail(e);
  if (LoadOne(libz, "libz.so", true, &e) == nullptr) return fail(e);
  if (!libstdcxx.empty() && LoadOne(libstdcxx, "libstdc++.so", true, &e) == nullptr) return fail(e);

  if (!SetUpMainThread(&e)) return fail(e);
  if (!unanswered_.empty()) {
    std::string names;
    for (const std::string& n : unanswered_) names += (names.empty() ? "" : ", ") + n;
    Log("scope: imports nothing answered (bound to " + Hex(opts_.unresolved_trap) + "): " + names);
  }
  scope_loaded_ = true;
  return true;
}

bool GuestProcess::PublishAuxv(std::string* err) {
  // What the kernel hands a process and the linker publishes through `__libc_shared_globals()->auxv`:
  // AT_PAGESZ, AT_HWCAP (VFP|NEON|VFPv3|TLS|VFPv4|IDIVA|IDIVT), AT_RANDOM (16 host-entropy bytes), AT_NULL.
  constexpr std::uint32_t kHwcap = 0x40u | 0x1000u | 0x2000u | 0x8000u | 0x1'0000u | 0x2'0000u | 0x4'0000u;
  at_random_ = auxv_ + 0x80u;
  std::random_device entropy;
  std::uint32_t random_words[4];
  for (std::uint32_t& w : random_words) w = entropy();
  if (random_words[0] == 0) random_words[0] = 0x0000'ff0au;  // a zero stack guard is a disabled one
  const std::uint32_t vec[] = {6u /* AT_PAGESZ */, mem::kGuestPage, 16u /* AT_HWCAP */, kHwcap,
                               25u /* AT_RANDOM */, at_random_, 0u /* AT_NULL */, 0u};
  std::uint32_t current = 0xffff'ffffu;
  if (!mem_->Read(globals_ + layout_.auxv_field, &current, 4) || current != 0) {
    if (err) *err = "the auxv field at globals + " + Hex(layout_.auxv_field) + " is not a zero word";
    return false;
  }
  if (!mem_->Write(auxv_, vec, sizeof vec) || !mem_->Write(at_random_, random_words, sizeof random_words) ||
      !mem_->Write(globals_ + layout_.auxv_field, &auxv_, 4)) {
    if (err) *err = "writing the auxv";
    return false;
  }
  Log("auxv: at " + Hex(auxv_) + " (AT_PAGESZ, AT_HWCAP " + Hex(kHwcap) + ", AT_RANDOM " + Hex(at_random_) +
      ", AT_NULL), published at globals + " + Hex(layout_.auxv_field));
  return true;
}

bool GuestProcess::CallSetup(const char* fn, const abi::Args32& args, std::string* err) {
  const loader::LocalSymbol* s = libc_->FindLocalSymbolEntry(fn);
  if (s == nullptr) {
    if (err) *err = std::string{fn} + " is not in libc.so's .symtab";
    return false;
  }
  abi_->set_executor(engine(opts_.ctor_tier));
  exec::RunResult rr{};
  abi_->CallGuest(s->addr | (s->thumb ? 1u : 0u), args, abi::Shorty::For("V"), kCallBudget, &rr);
  if (rr.reason != exec::StopReason::Returned) {
    if (err) *err = std::string{fn} + " did not return: " + ReasonName(rr.reason) + " " + rr.detail;
    return false;
  }
  return true;
}

bool GuestProcess::SetUpMainThread(std::string* err) {
  const auto fail = [&](const std::string& why) {
    if (err) *err = "main thread: " + why;
    return false;
  };
  const BionicLayout& l = layout_;
  // The static-TLS layout, published where `pthread_create`/`__init_static_tls` read it. No TLS module is
  // registered (count +0x464 stays 0): libc's own PT_TLS segment is not given a static offset here.
  const std::uint32_t fields[][2] = {{kGlobalsStaticTlsSize, l.static_tls_size},
                                     {kGlobalsOffsetTcb, l.offset_bionic_tcb},
                                     {kGlobalsOffsetTls, l.offset_bionic_tls}};
  for (const auto& f : fields) {
    if (!mem_->Write(globals_ + f[0], &f[1], 4)) return fail("writing shared-globals + " + Hex(f[0]));
  }

  // The thread mapping, shaped as __allocate_thread_mapping(0, PTHREAD_GUARD_SIZE) shapes one (and as the
  // device's own main thread shows it: mmap_size 0x5000 for its 0xba8 of static TLS): a guard page, the
  // read/write body, a guard page; static TLS ending 0x3000 below the top, then the 0x2000 scratch.
  const std::uint32_t size = RoundUp(l.static_tls_size + kPthreadGuard + kMappingTop, mem::kGuestPage);
  const GuestAddr base = FindFree(size);
  std::string e;
  if (base == 0 || !mem_->Map(base, kPthreadGuard, mem::kNone, &e) ||
      !mem_->Map(base + kPthreadGuard, size - kPthreadGuard - 0x1000u, mem::kRead | mem::kWrite, &e) ||
      !mem_->Map(base + size - 0x1000u, 0x1000u, mem::kNone, &e)) {
    return fail("mapping the " + Hex(size) + "-byte thread mapping: " + e);
  }
  thread_mapping_ = base;
  thread_mapping_size_ = size;
  const GuestAddr scratch = base + size - kMappingTop;
  const GuestAddr static_tls = scratch - l.static_tls_size;
  const GuestAddr tcb = static_tls + l.offset_bionic_tcb;
  const GuestAddr bionic_tls = static_tls + l.offset_bionic_tls;
  tp_ = tcb + l.tp_offset_in_tcb;

  // pthread_internal_t: on a device a static in the linker; here one page of its own.
  thread_ = MapFree(mem::kGuestPage, mem::kRead | mem::kWrite, &e);
  if (thread_ == 0) return fail("the pthread_internal_t page: " + e);
  const std::uint32_t words[][2] = {
      {kThNext, 0},
      {kThPrev, 0},
      {kThTid, static_cast<std::uint32_t>(kMainTid)},
      {kThCachedPid, static_cast<std::uint32_t>(kPid)},  // cached_pid_:31, vforked_:1 = 0
      {kThAttrFlags, kPthreadAttrFlagInherit},
      {kThMmapBase, base},
      {kThMmapSize, size},
      {kThMmapBaseUnguarded, base + kPthreadGuard},
      {kThMmapSizeUnguarded, size - kPthreadGuard - 0x1000u},
      {kThBionicTls, bionic_tls},
      {kThBionicTcb, tcb},
  };
  // start_routine (+0x30) stays 0 — that is bionic's `is_main()` — and so does the byte at +0x2c4, which
  // the device's own main thread also has as 0 (it is not a main-thread flag in this build).
  for (const auto& w : words) {
    if (!mem_->Write(thread_ + w[0], &w[1], 4)) return fail("writing pthread_internal_t + " + Hex(w[0]));
  }
  if (!mem_->Write(bionic_tls + kBtTempBuffers, &scratch, 4)) return fail("writing bionic_tls + 0x414");

  // bionic's own code fills the tcb's thread and DTV slots, exactly as pthread_create does.
  if (!CallSetup("__init_tcb", {abi::Value32::Ptr(tcb), abi::Value32::Ptr(thread_)}, &e)) return fail(e);
  if (!CallSetup("__init_tcb_dtv", {abi::Value32::Ptr(tcb)}, &e)) return fail(e);
  // TLS_SLOT_BIONIC_TLS and the stack guard (the linker's: from AT_RANDOM; libc's __libc_preinit copies
  // TP+0x14 into __stack_chk_guard).
  std::uint32_t guard = 0;
  if (!mem_->Read(at_random_, &guard, 4) || !mem_->Write(tcb + kTcbBionicTls, &bionic_tls, 4) ||
      !mem_->Write(tcb + kTcbStackGuard, &guard, 4)) {
    return fail("writing the tcb's bionic_tls and stack-guard slots");
  }

  // g_thread_list stays empty: libc links the main thread itself — `__libc_init_common` (run by
  // `__libc_preinit`, libc's first constructor) does `__pthread_internal_add(__get_thread())`, and on a
  // device the linker leaves the list to it too. Pre-linking here made the entry its own next/prev.
  const auto list = libc_->FindLocalSymbol("g_thread_list");
  std::uint32_t head = 0xffff'ffffu;
  if (!list || !mem_->Read(*list, &head, 4) || head != 0) return fail("g_thread_list is missing or not empty");

  abi_->state().tp = tp_;
  monitor_->set_clear_child_tid(thread_ + kThTid);  // set_tid_address(&main_thread.tid)
  Log("thread: main pthread_internal_t at " + Hex(thread_) + " (tid " + std::to_string(kMainTid) +
      "), static TLS at " + Hex(static_tls) + " in a " + Hex(size) + "-byte mapping at " + Hex(base) +
      ", TP " + Hex(tp_) + ", bionic_tls " + Hex(bionic_tls) + "; g_thread_list left to __libc_init_common");
  return true;
}

const loader::GuestModule* GuestProcess::LoadApp(const std::vector<std::uint8_t>& image,
                                                 const std::string& soname, std::string* err) {
  if (!scope_loaded_) {
    if (err) *err = "LoadApp(" + soname + "): the scope is not loaded";
    return nullptr;
  }
  if (!loader::LooksLikeAArch32Elf(image.data(), image.size())) {
    if (err) *err = "LoadApp(" + soname + "): not an ELF32 ARM image";
    return nullptr;
  }
  return LoadOne(image, soname, false, err);
}

bool GuestProcess::RunConstructors(std::string* failed) {
  if (!scope_loaded_) {
    if (failed) *failed = "the scope is not loaded";
    return false;
  }
  const auto run = [&](loader::GuestModule& m, loader::CodeAddress fn, const std::string& what) {
    abi_->set_executor(engine(opts_.ctor_tier));
    exec::RunResult rr{};
    exec::MachineState exit{};
    abi_->CallGuest(fn.tagged(), {}, abi::Shorty::For("V"), kCallBudget, &rr, &exit);
    if (rr.reason == exec::StopReason::Returned) {
      ++ctors_run_;
      return true;
    }
    const std::string name = m.soname.empty() ? m.path : m.soname;
    const std::string why = name + " " + what + " at " + Hex(fn.addr) + " did not return on Tier " +
                            (opts_.ctor_tier == 2 ? "J" : "I") + ": " + ReasonName(rr.reason) +
                            (rr.detail.empty() ? "" : " (" + rr.detail + ")") + "; pc " + Hex(exit.pc) +
                            " lr " + Hex(exit.r[14]);
    if (failed) *failed = why;
    Log("constructors: " + why);
    return false;
  };
  for (const auto& mp : modules_) {
    loader::GuestModule& m = *mp;
    if (m.constructors_called) continue;
    if (m.init.addr != 0 && !run(m, m.init, "DT_INIT")) return false;
    for (std::size_t i = 0; i < m.init_array.size(); ++i) {
      if (m.init_array[i].addr == 0) continue;
      if (!run(m, m.init_array[i], "DT_INIT_ARRAY[" + std::to_string(i) + "]")) return false;
    }
    m.constructors_called = true;
  }
  Log("constructors: " + std::to_string(ctors_run_) + " ran on Tier " + (opts_.ctor_tier == 2 ? "J" : "I"));
  return true;
}

}  // namespace rb::runtime
