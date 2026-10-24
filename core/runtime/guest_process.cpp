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
#include <system_error>

#include "rb/linker/guest_linker.h"
#include "rb/loader/elf_defs.h"
#include "rb/runtime/signal_frame.h"

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

// Two kinds of constant below, and the difference is the point:
//   [image]  asserted at load against this libc's own instructions (kFacts in VerifyBionicLayout); a
//            different build is a refusal naming the fact that moved.
//   [probe]  NOT derivable from libc.so — the linker (ld-android, not libc) computes the static-TLS layout,
//            so libc's code never states sizeof(bionic_tcb), alignof/sizeof(bionic_tls) or the reservation
//            order. These are the device's answers for its own main thread, measured by the committed
//            probe `tests/guest/tls_layout_probe.c` on R52Y106BECZ (it checks each value and exits 1 on a
//            mismatch; its header says how to run it). Re-run it against any new libc build.

constexpr std::uint32_t kThreadSize = 0x2c8;          // [image] sizeof(pthread_internal_t)
constexpr std::uint32_t kTpOffsetInTcb = 0xc;         // [image] TP = tcb + 0xc: slots -3..-1 below TLS_SLOT_DTV
constexpr std::uint32_t kTcbSize = 0x2c;              // [probe] offset_bionic_tls 0x2c with the tcb at 0: 11 slots
constexpr std::uint32_t kBionicTlsLastByte = 0xb49;   // [image] bionic_systrace_disabled (a lower bound only)
constexpr std::uint32_t kBionicTlsSize = 0xb4c;       // [probe] libc's TLS module at align8(0x2c + size) == 0xb78
constexpr std::uint32_t kStaticTlsAlign = 4;          // [probe] bionic_tls at 0x2c, not 0x30: 4-aligned
static_assert(kBionicTlsSize > kBionicTlsLastByte, "the probed size must cover the last field the image touches");
constexpr std::uint32_t kGlobalsStaticTlsSize = 0x420;   // [image]
constexpr std::uint32_t kGlobalsOffsetTcb = 0x428;       // [image]
constexpr std::uint32_t kGlobalsOffsetTls = 0x42c;       // [image]
constexpr std::uint32_t kGlobalsTlsModuleCount = 0x464;  // [image]
constexpr std::uint32_t kGlobalsTlsGeneration = 0x434;   // [image] TlsModules::generation (copied by __libc_preinit_impl)
constexpr std::uint32_t kGlobalsTlsStaticCount = 0x468;  // [image] TlsModules::static_module_count
constexpr std::uint32_t kGlobalsTlsModuleTable = 0x46c;  // [image] TlsModules::module_table
constexpr std::uint32_t kTlsModuleBytes = 0x20;          // [image] sizeof(TlsModule)
constexpr std::uint32_t kTlsGenerationFirst = 1;         // bionic's kTlsGenerationFirst (0 is "none")

/// Where the device's linker puts libc.so's TLS block in static TLS: after the tcb (at 0) and bionic_tls, at
/// the segment's own alignment. [probe] tls_layout_probe.c measured 0xb78 for this libc (align 8).
std::uint32_t LibcTlsOffset(const loader::TlsSegment& seg) {
  return RoundUp(RoundUp(kTcbSize, kStaticTlsAlign) + kBionicTlsSize, seg.align);
}
/// [image] __allocate_thread_mapping(stack, guard): mmap_size = round(static_tls + stack + guard + 0x3000);
/// the static TLS sits 0x3000 below the top: 0x2000 of basename/dirname buffers (bionic_tls+0x414) and a
/// 0x1000 guard page; the body [base+guard, end-0x1000) is the read/write part.
constexpr std::uint32_t kMappingTop = 0x3000;
constexpr std::uint32_t kPthreadGuard = 0x1000;  // [image] PTHREAD_GUARD_SIZE: pthread_create's (0, 0x1000) call

// [image] pthread_internal_t fields (the main thread's are written by the host, as the linker does).
constexpr std::uint32_t kThNext = 0x0, kThPrev = 0x4, kThTid = 0x8, kThCachedPid = 0xc, kThAttrFlags = 0x10;
constexpr std::uint32_t kThMmapBase = 0x60, kThMmapSize = 0x64, kThMmapBaseUnguarded = 0x68;
constexpr std::uint32_t kThMmapSizeUnguarded = 0x6c, kThBionicTls = 0x298, kThBionicTcb = 0x2a0;
constexpr std::uint32_t kBtTempBuffers = 0x414;    // [image] bionic_tls: basename/dirname scratch (0x2000)
constexpr std::uint32_t kTcbBionicTls = 0x8;       // [image] slot -1, TP-4
constexpr std::uint32_t kTcbStackGuard = 0x20;     // [image] slot 5, TP+0x14
/// [probe] the value the device's main thread carries (PTHREAD_ATTR_FLAG_INHERIT); [image] that bit 2 of
/// attr.flags is what __init_thread tests.
constexpr std::uint32_t kPthreadAttrFlagInherit = 0x4;

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
      {"__init_static_tls", {0x3720}, "TlsModule entries are 0x20 bytes (`adds r7, #0x20`)"},
      {"__init_static_tls", {0x694b, 0x1c5a}, "TlsModule::static_offset at +0x14, SIZE_MAX = not static (`ldr r3, [r1, #0x14]; adds r2, r3, #1`)"},
      {"__init_static_tls", {0x690a}, "TlsModule::segment.init_size at +0x10 (`ldr r2, [r1, #0x10]`)"},
      {"__init_static_tls", {0x68c9}, "TlsModule::segment.init_ptr at +0xc (`ldr r1, [r1, #0xc]`)"},
      {"__libc_preinit_impl", {0xf200, 0x4034}, "the TLS generation at shared-globals +0x434 (`addw r0, r0, #0x434`)"},
      {"__libc_iterate_dynamic_tls", {0xf8d6, 0x5468}, "the static TLS module count at shared-globals +0x468"},
      // The pthread_internal_t fields the host writes for the main thread, each where libc's own code puts it.
      {"pthread_create", {0x68f2, 0x4479, 0xf360, 0x021e, 0x60f2}, "pthread_internal_t::cached_pid_:31 at +0xc (`ldr r2,[r6,#0xc]; bfi r2,r0,#0,#31; str r2,[r6,#0xc]`)"},
      {"pthread_create", {0xf10b, 0x0110}, "pthread_internal_t::attr copied to +0x10 (`add.w r1, r11, #0x10`)"},
      {"__init_thread", {0x6920}, "__init_thread reads attr.flags at +0x10 (`ldr r0, [r4, #0x10]`)"},
      {"__init_thread", {0x0740}, "__init_thread tests attr.flags bit 2, PTHREAD_ATTR_FLAG_INHERIT (`lsls r0, r0, #0x1d`)"},
      {"pthread_create", {0xf8cb, 0x8060}, "pthread_internal_t::mmap_base at +0x60 (`str.w r8, [r11, #0x60]`)"},
      {"pthread_create", {0xf8cb, 0x1064}, "pthread_internal_t::mmap_size at +0x64 (`str.w r1, [r11, #0x64]`)"},
      {"pthread_create", {0xe9cb, 0x171a}, "pthread_internal_t::mmap_base_unguarded/mmap_size_unguarded at +0x68/+0x6c (`strd r1, r7, [r11, #104]`)"},
      {"__pthread_internal_remove_and_free", {0x6e61}, "the thread's unmap reads mmap_size at +0x64"},
      {"__pthread_internal_remove_and_free", {0x6e20}, "the thread's unmap reads mmap_base at +0x60"},
      // The thread mapping's shape, as __allocate_thread_mapping builds it.
      {"pthread_create", {0x2100, 0xf44f, 0x5280}, "PTHREAD_GUARD_SIZE 0x1000 (`__allocate_thread_mapping(0, 0x1000)`)"},
      {"__allocate_thread_mapping", {0xf511, 0x5180}, "mapping size adds 0x1000"},
      {"__allocate_thread_mapping", {0xf511, 0x5100}, "mapping size adds 0x2000 (0x3000 in all above static TLS)"},
      {"__allocate_thread_mapping", {0xeb07, 0x0806}, "mmap_base_unguarded = mmap_base + guard (`add.w r8, r7, r6`)"},
      {"__allocate_thread_mapping", {0x1ba8}, "mmap_size_unguarded = mmap_size - guard ... (`subs r0, r5, r6`)"},
      {"__allocate_thread_mapping", {0xf5a0, 0x5680}, "... - 0x1000, the top guard page (`sub.w r6, r0, #0x1000`)"},
      {"__allocate_thread_mapping", {0xe9c4, 0x1006}, "the static-TLS end (mapping end - 0x3000) recorded at ThreadMapping+0x1c"},
      {"pthread_create", {0xf8c1, 0x9414}, "bionic_tls+0x414 = that static-TLS end, the 0x2000 scratch (`str.w r9, [r1, #0x414]`)"},
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
  // [probe] facts (see the constants): libc.so does not state them; tests/guest/tls_layout_probe.c does.
  l.bionic_tls_size = kBionicTlsSize;
  // StaticTlsLayout with no executable TLS segment and no module: reserve_exe_segment_and_tcb(nullptr)
  // reserves the tcb at 0, reserve_bionic_tls() follows it, finish_layout() rounds to the alignment.
  l.offset_bionic_tcb = 0;
  l.offset_bionic_tls = RoundUp(l.offset_bionic_tcb + l.tcb_size, kStaticTlsAlign);
  l.static_tls_size = RoundUp(l.offset_bionic_tls + l.bionic_tls_size, kStaticTlsAlign);
  if (libc.tls && (libc.tls->align == 0 || (libc.tls->align & (libc.tls->align - 1)) != 0 || libc.tls->align > 0x1000)) {
    return refuse("libc.so's PT_TLS alignment " + Hex(libc.tls->align) + " is not a power of two up to a page");
  }
  if (libc.tls) {
    // reserve_solib_segment(libc's PT_TLS), then finish_layout(): the device's own 0xba8 for this libc.
    l.libc_tls = *libc.tls;
    l.libc_tls_offset = LibcTlsOffset(*libc.tls);
    // finish_layout rounds to the largest alignment reserved, so every thread's block keeps libc's alignment.
    l.static_tls_size = RoundUp(l.libc_tls_offset + libc.tls->memsz, std::max(kStaticTlsAlign, libc.tls->align));
  }

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
  if (l.auxv_field >= kGlobalsStaticTlsSize && l.auxv_field < kGlobalsTlsModuleTable + 4u) {
    return refuse("getauxval's auxv field " + Hex(l.auxv_field) + " overlaps the static-TLS fields");
  }
  return l;
}

// ------------------------------------------------------------------------------------ GuestProcess

GuestProcess::~GuestProcess() {
  // Order matters (lock discipline, M2 Task 5): first stop the guest (sticky group exit, every futex waiter and
  // sleeper woken) so an adopted host thread's exit hook that is mid-way through guest destructors stops at its
  // next check; then take `alive_`'s mutex exclusively (never held by anyone waiting for the GGL's holder) to cut every
  // later hook off; then join the clone()d threads. Adopted GuestThreads go with `adopted_`; their destructors
  // clear the binding cells of host threads still alive, which then read as unbound.
  if (monitor_ != nullptr) monitor_->RequestGroupExit("the guest process is being destroyed");
  futex_.Shutdown();
  {
    std::unique_lock<std::shared_mutex> lk(alive_->mu);  // waits out every exit hook in flight
    alive_->proc = nullptr;
  }
  StopThreads();
}

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
  p->alive_->proc = p.get();
  mem::AddressModel model;
  model.kind = options.biased ? mem::AddressModel::kBiased : mem::AddressModel::kFlatIdentity;
  model.host_page_shift = HostPageShift();
  std::string e;
  p->mem_ = mem::GuestMemory::Create(options.window_bytes, model, &e);
  if (!p->mem_) return fail("GuestMemory::Create(" + std::to_string(options.window_bytes) + "): " + e);
  mem::GuestMemory& m = *p->mem_;

  // The host-entry window (return sentinel + slot table): reserved so no placement lands on it.
  // The sentinel page, then the slot table (`AbiContext::SlotFor`: kMaxSlots four-byte entries).
  constexpr std::size_t kHostWindow = 0x1000u + std::size_t{abi::AbiContext::kMaxSlots} * 4u;
  if (!m.Map(kReturnAddress, kHostWindow, mem::kNone, &e)) return fail("reserving the host-entry window: " + e);
  // Nothing at or above kAllocCeiling: reserved outright when the window reaches it, so neither this
  // allocator, the loader's placement scan nor the monitor's mmap2 can hand it out.
  p->ceiling_ = static_cast<GuestAddr>(std::min<std::size_t>(options.window_bytes, kAllocCeiling));
  if (options.window_bytes > kAllocCeiling &&
      !m.Map(kAllocCeiling, options.window_bytes - kAllocCeiling, mem::kNone, &e)) {
    return fail("reserving the window above " + Hex(kAllocCeiling) + ": " + e);
  }
  if (options.unresolved_trap != 0) {
    const std::uint64_t end = std::uint64_t{options.unresolved_trap} + kTrapSpan;
    for (std::uint64_t at = options.unresolved_trap & ~(mem::kGuestPage - 1u); at < end; at += mem::kGuestPage) {
      const auto page = static_cast<GuestAddr>(at);
      if (page < p->ceiling_ && !m.IsMapped(page) && !m.Map(page, mem::kGuestPage, mem::kNone, &e)) {
        return fail("reserving the unresolved-import trap page " + Hex(page) + ": " + e);
      }
    }
  }
  // The main thread's stack, 1 MiB below the ceiling, with a guard page under it.
  p->stack_top_ = p->ceiling_ - 0x0010'0000u;
  const GuestAddr stack_lo = p->stack_top_ - kMainStackBytes;
  if (!m.Map(stack_lo, kMainStackBytes, mem::kRead | mem::kWrite, &e) ||
      !m.Map(stack_lo - mem::kGuestPage, mem::kGuestPage, mem::kNone, &e)) {
    return fail("mapping the main stack below " + Hex(p->stack_top_) + ": " + e);
  }

  p->abi_ = std::make_unique<abi::AbiContext>(m, p->stack_top_, kReturnAddress);
  p->abi_->main_thread().stack_lo = stack_lo;
  p->monitor_ = std::make_unique<syscall::Monitor>();
  p->monitor_->set_memory(&m);
  p->monitor_->set_ids(kPid, kMainTid);
  p->monitor_->set_alloc_ceiling(kAllocCeiling);  // mmap2/mremap never place at or above it
  p->abi_->set_monitor(p->monitor_.get());
  // Guest threads (Task 4): futex waits park on this process's table, and `clone` starts a host thread here.
  p->monitor_->set_futex_table(&p->futex_);
  {
    GuestProcess* owner = p.get();
    p->monitor_->set_thread_spawner([owner](std::unique_ptr<GuestThread> t) { return owner->Spawn(std::move(t)); });
    // Adopted ART threads (Task 5): a host thread with no binding that calls in gets a guest thread of its own.
    p->abi_->set_adopter([owner](std::string* why) { return owner->AdoptCurrentHostThread(why); });
    // A fault at a trap slot names the import bound there, whichever thread made the call.
    p->abi_->set_fault_namer([owner](GuestAddr pc) { return owner->TrapName(pc); });
    // C26: the restorer's `rt_sigreturn` restores the frame the delivery wrote. The monitor sits below
    // this layer and cannot know the frame layout, so the runtime owns this half.
    // The monitor needs to know a target's blocked mask before deciding to wake its futex wait (see
    // Monitor::QueueSignal): a blocked signal must not interrupt a wait the kernel would leave alone.
    p->monitor_->set_thread_lookup([owner](std::int32_t tid) -> GuestThread* {
      GuestThread& main = owner->abi_->main_thread();
      if (tid == main.tid) return &main;
      {
        std::lock_guard<std::mutex> lk(owner->threads_mu_);
        for (const auto& rec : owner->threads_) {
          if (!rec->done.load(std::memory_order_acquire) && rec->guest->tid == tid) return rec->guest.get();
        }
      }
      for (const auto& t : owner->adopted_) {  // GGL-protected; QueueSignal runs under the GGL
        if (t->tid == tid) return t.get();
      }
      return nullptr;
    });
    p->monitor_->set_signal_return([owner](GuestThread& thread) -> syscall::Outcome {
      std::string why;
      if (!ReturnFromSignal(thread, *owner->mem_, &why)) {
        return syscall::Outcome::Refused(syscall::err::kEnosys, "rt_sigreturn: " + why);
      }
      return syscall::Outcome::Emulated(0, why);
    });
    // C26: entry into a queued signal's handler, at the ABI's slice boundaries. Every thread runs there —
    // the main thread and adopted host threads included, not only `ThreadMain`'s clone()d ones — and a
    // futex cut short by EINTR passes through the boundary before it can retry. Quiet about "none" and
    // "blocked" (both are ordinary at every slice); loud about a handler that cannot be entered.
    p->abi_->set_signal_deliver([owner](GuestThread& thread) {
      std::string why;
      const std::uint32_t sp_before = thread.state.r[13];
      const std::size_t deliveries_before = thread.deliveries.size();
      if (DeliverPendingSignal(thread, *owner->mem_, *owner->monitor_, &why)) {
        owner->Log("signals: " + why);
        // An abort() is the game's own last word; its caller is what explains it. The crash handler's
        // dump showed the abort frame only (and a 0xff stack), so scan the interrupted stack for
        // module-shaped return addresses at delivery time (SIGABRT = 6).
        if (thread.deliveries.size() > deliveries_before && thread.deliveries.back().sig == 6) {
          owner->Log("signals: SIGABRT interrupted sp " + Hex(sp_before) + " stack: " +
                     owner->StackWords(sp_before, 1024, 12));
          // The abort's own words were written to guest fd 2 a few instructions earlier (Boehm GC's
          // "pthread_kill failed at suspend: errcode= %d" is the one that matters here); a console no one
          // reads leaves the SIGABRT without a cause.
          const std::string& err = owner->monitor_->captured_stderr();
          if (!err.empty()) {
            std::string tail = err.size() > 400 ? err.substr(err.size() - 400) : err;
            for (char& c : tail) {
              if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            }
            owner->Log("signals: last guest stderr: \"" + tail + "\"");
          }
        }
      } else if (why.find("no signal is pending") == std::string::npos &&
                 why.find("is blocked in this thread's mask") == std::string::npos) {
        owner->Log("signals: not delivered — " + why);
      }
    });
    // A synchronous memory fault the guest has a SIGSEGV handler for is a signal, not a dead process. The
    // kernel builds the frame on the faulting thread's stack with si_addr and resumes there; Mono's JIT
    // relies on that delivery for every null check (a managed NullReferenceException IS this handler).
    // A fault with no handler, a blocked SIGSEGV, one inside the handler, or a full delivery stack stays
    // fatal, which is also what the kernel's default action does.
    p->abi_->set_fault_deliver([owner](GuestThread& thread, const exec::RunResult& run) -> bool {
      constexpr int kSigsegv = 11;
      constexpr std::size_t kMaxNestedSignals = 8;
      if (thread.deliveries.size() >= kMaxNestedSignals) return false;
      if (!thread.deliveries.empty() && thread.deliveries.back().sig == kSigsegv) return false;  // a fault in the handler
      if ((thread.signal_mask & (1ull << (kSigsegv - 1))) != 0) return false;
      const syscall::Monitor::Disposition disp = owner->monitor_->disposition(kSigsegv);
      if (!disp.catches()) return false;
      const std::uint32_t interrupted_pc = thread.state.pc;
      const int code = run.fault_write ? sigframe::kSegvAccerr : sigframe::kSegvMaperr;
      std::string why;
      if (!DeliverSignal(thread, *owner->mem_, disp, kSigsegv, nullptr, &why, code, run.fault_addr)) {
        owner->Log("signals: SIGSEGV at " + Hex(run.fault_addr) + " not delivered — " + why);
        return false;
      }
      owner->Log("signals: SIGSEGV at " + Hex(run.fault_addr) + " (" +
                 (run.fault_write ? "write" : "read") + ", si_code " + std::to_string(code) +
                 ") delivered to handler " + Hex(disp.handler) + "; interrupted pc " +
                 Hex(interrupted_pc));
      return true;
    });
  }

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
    if (h && (opts_.unresolved_trap == 0 || *h - opts_.unresolved_trap >= kTrapSpan)) return h;
  }
  auto it = std::find(unanswered_.begin(), unanswered_.end(), name);
  if (it == unanswered_.end()) it = unanswered_.insert(unanswered_.end(), name);
  if (opts_.unresolved_trap == 0) return std::nullopt;
  // Its own slot, so a call names it (TrapName); one shared address named nothing (ABGo, 2026-10-17).
  const auto slot =
      std::min<std::size_t>(static_cast<std::size_t>(it - unanswered_.begin()), kTrapSpan / kTrapSlot - 1u);
  return opts_.unresolved_trap + static_cast<std::uint32_t>(kTrapSlot * slot);
}

std::string GuestProcess::TrapName(GuestAddr pc) const {
  if (opts_.unresolved_trap == 0 || pc - opts_.unresolved_trap >= kTrapSpan) return {};
  const std::size_t slot = (pc - opts_.unresolved_trap) / kTrapSlot;
  const GuestAddr at = opts_.unresolved_trap + static_cast<GuestAddr>(slot * kTrapSlot);
  if (slot >= unanswered_.size()) return "an unresolved-import trap slot no import is bound to (" + Hex(at) + ")";
  if (slot == kTrapSpan / kTrapSlot - 1u && unanswered_.size() > kTrapSpan / kTrapSlot) {
    return "one of the unanswered imports from " + unanswered_[slot] + " on (" +
           std::to_string(unanswered_.size() - slot) + " names share the trap's last slot " + Hex(at) + ")";
  }
  return "called the unanswered import " + unanswered_[slot] + " (nothing answers it; bound to trap slot " +
         Hex(at) + ")";
}

loader::GuestModule* GuestProcess::LoadOne(const std::vector<std::uint8_t>& image, const std::string& soname,
                                           bool scope, std::string* err) {
  loader::LoadOptions opts;
  // A real libc carries TLS relocations this loader cannot apply; the scope tolerates them (they stay
  // recorded in `unsupported_relocs`). An app does not: a refused relocation fails its load by name.
  opts.tolerate_unresolved_relocs = scope;
  opts.unresolved_trap = opts_.unresolved_trap;
  opts.unresolved_trap_span = kTrapSpan;
  opts.resolve = [this](const std::string& name) { return ResolveImport(name); };
  opts.resolve_ifunc = [this](const loader::IfuncSlot& s) { return RunIfunc(s); };
  if (scope && soname == "libc.so") {
    // libc's own thread-locals (GWP-ASan's, llvm-libc's randomness state), answered the way the device's linker
    // answers them: libc is TLS module 1 with a static block at LibcTlsOffset (SetUpMainThread registers it),
    // the tcb at static-TLS offset 0 and TP = tcb + kTpOffsetInTcb. Left unapplied, the initial-exec offset
    // read 0 and GWP-ASan (on in a random few processes) wrote its thread-locals over TLS_SLOT_THREAD_ID.
    opts.resolve_tls = [](std::uint32_t type, std::uint32_t addend,
                          const loader::TlsSegment& seg) -> std::optional<std::uint32_t> {
      if (type == loader::elf::R_ARM_TLS_TPOFF32) return addend + LibcTlsOffset(seg) - kTpOffsetInTcb;
      if (type == loader::elf::R_ARM_TLS_DTPMOD32) return 1u;
      if (type == loader::elf::R_ARM_TLS_DTPOFF32) return addend;
      return std::nullopt;
    };
  }
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
  // Ruling 1 (M2 Task 3): the main guest thread belongs to the host thread that boots the scope. Every guest
  // call this process makes on its behalf (IFUNC resolvers, the main-thread setup, constructors, and the
  // caller's own later calls) runs there; another host thread must be bound to its own GuestThread (or, for
  // a caller that boots on one thread and continues on another, rebind the main thread under its own lock).
  if (!runtime::BindCurrentHostThread(abi_->main_thread())) {
    return fail("the main guest thread is bound to another live host thread");
  }
  // Everything below writes guest memory and the module list, which the GGL protects (Task 3 ruling iii).
  Ggl::Held hold(abi_->ggl());

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

bool GuestProcess::RegisterLibcTls(std::string* err) {
  // What the linker's register_soinfo_tls + finish_layout leave in __libc_shared_globals()->tls_modules for a
  // process whose only TLS module is libc.so: one static module, visible from the first generation.
  const BionicLayout& l = layout_;
  std::string e;
  const GuestAddr table = MapFree(mem::kGuestPage, mem::kRead | mem::kWrite, &e);
  if (table == 0) {
    if (err) *err = "the TLS module table page: " + e;
    return false;
  }
  // TlsModule: segment {size, alignment {value, skew}, init_ptr, init_size}, static_offset, first_generation,
  // soinfo_ptr (none: no soinfo exists here; only dl_iterate_phdr-style lookups read it).
  const std::uint32_t module[kTlsModuleBytes / 4] = {l.libc_tls.memsz, l.libc_tls.align, 0u, l.libc_tls.vaddr,
                                                     l.libc_tls.filesz, l.libc_tls_offset, kTlsGenerationFirst, 0u};
  std::uint32_t generation = 0xffff'ffffu;
  if (!mem_->Read(globals_ + kGlobalsTlsGeneration, &generation, 4) || generation > kTlsGenerationFirst) {
    if (err) *err = "the TLS generation at shared-globals + " + Hex(kGlobalsTlsGeneration) + " reads " + Hex(generation) +
                    ", not a fresh process's 0 or 1";
    return false;
  }
  const std::uint32_t fields[][2] = {{kGlobalsTlsGeneration, kTlsGenerationFirst},
                                     {kGlobalsTlsModuleCount, 1u},
                                     {kGlobalsTlsStaticCount, 1u},
                                     {kGlobalsTlsModuleTable, table}};
  if (!mem_->Write(table, module, sizeof module)) {
    if (err) *err = "writing the TLS module table";
    return false;
  }
  for (const auto& f : fields) {
    if (!mem_->Write(globals_ + f[0], &f[1], 4)) {
      if (err) *err = "writing shared-globals + " + Hex(f[0]);
      return false;
    }
  }
  Log("tls: libc.so is static TLS module 1: " + Hex(l.libc_tls.memsz) + " bytes (align " + Hex(l.libc_tls.align) +
      ", image " + Hex(l.libc_tls.filesz) + " at " + Hex(l.libc_tls.vaddr) + ") at static-TLS offset " +
      Hex(l.libc_tls_offset) + " (TP + " + Hex(l.libc_tls_offset - l.offset_bionic_tcb - l.tp_offset_in_tcb) +
      "); static TLS " + Hex(l.static_tls_size) + "; module table at " + Hex(table));
  return true;
}

bool GuestProcess::SetUpMainThread(std::string* err) {
  const auto fail = [&](const std::string& why) {
    if (err) *err = "main thread: " + why;
    return false;
  };
  const BionicLayout& l = layout_;
  // The static-TLS layout, published where `pthread_create`/`__init_static_tls` read it, and libc's own PT_TLS
  // registered as static TLS module 1 the way the device's linker registers it (RegisterLibcTls).
  const std::uint32_t fields[][2] = {{kGlobalsStaticTlsSize, l.static_tls_size},
                                     {kGlobalsOffsetTcb, l.offset_bionic_tcb},
                                     {kGlobalsOffsetTls, l.offset_bionic_tls}};
  for (const auto& f : fields) {
    if (!mem_->Write(globals_ + f[0], &f[1], 4)) return fail("writing shared-globals + " + Hex(f[0]));
  }
  std::string e;
  if (l.libc_tls_offset != 0 && !RegisterLibcTls(&e)) return fail(e);

  // The thread mapping, shaped as __allocate_thread_mapping(0, PTHREAD_GUARD_SIZE) shapes one (and as the
  // device's own main thread shows it: mmap_size 0x5000 for its 0xba8 of static TLS): a guard page, the
  // read/write body, a guard page; static TLS ending 0x3000 below the top, then the 0x2000 scratch.
  const std::uint32_t size = RoundUp(l.static_tls_size + kPthreadGuard + kMappingTop, mem::kGuestPage);
  const GuestAddr base = FindFree(size);
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
  // libc's TLS block gets its initialization image, by libc's own code, as pthread_create does for a new thread.
  if (l.libc_tls_offset != 0 && !CallSetup("__init_static_tls", {abi::Value32::Ptr(static_tls)}, &e)) return fail(e);
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
  runtime::GuestThread& main = abi_->main_thread();
  main.pthread_struct = thread_;
  main.tid = kMainTid;
  monitor_->set_clear_child_tid(thread_ + kThTid);  // set_tid_address(&main_thread.tid)
  Log("thread: main pthread_internal_t at " + Hex(thread_) + " (tid " + std::to_string(kMainTid) +
      "), static TLS at " + Hex(static_tls) + " in a " + Hex(size) + "-byte mapping at " + Hex(base) +
      ", TP " + Hex(tp_) + ", bionic_tls " + Hex(bionic_tls) + "; g_thread_list left to __libc_init_common");
  return true;
}

const loader::GuestModule* GuestProcess::LoadApp(const std::vector<std::uint8_t>& image,
                                                 const std::string& soname, std::string* err) {
  // Loading maps and relocates guest memory and runs IFUNC resolvers: GGL-protected state, and once guest
  // threads exist another one may be running (Task 3 ruling iii).
  Ggl::Held hold(abi_->ggl());
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
  Ggl::Held hold(abi_->ggl());  // the module list and ctor bookkeeping are GGL-protected (Task 3 ruling iii)
  if (!scope_loaded_) {
    if (failed) *failed = "the scope is not loaded";
    return false;
  }
  const auto run = [&](loader::GuestModule& m, loader::CodeAddress fn, const std::string& what) {
    abi_->set_executor(engine(opts_.ctor_tier));
    exec::RunResult rr{};
    exec::MachineState exit{};
    abi_->CallGuest(fn.tagged(), {}, abi::Shorty::For("V"), kInitBudget, &rr, &exit);
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
    ctor_faults_[&m] = why;
    return false;
  };
  for (const auto& mp : modules_) {
    loader::GuestModule& m = *mp;
    if (m.constructors_called) continue;
    if (const auto fault = ctor_faults_.find(&m); fault != ctor_faults_.end()) {
      const std::string why = (m.soname.empty() ? m.path : m.soname) +
                              ": its constructors faulted in an earlier RunConstructors (" + fault->second +
                              "); the ones before the fault already ran, so none is run again";
      if (failed) *failed = why;
      Log("constructors: refused — " + why);
      return false;
    }
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

// ------------------------------------------------------------------------------------ guest threads (Task 4)

std::string GuestProcess::Spawn(std::unique_ptr<GuestThread> t) {
  // Called by the monitor's `clone` with the GGL held: the new host thread binds its GuestThread and then
  // waits for the GGL, so it runs no guest code before the parent's clone has returned and released it.
  if (monitor_->group_exited()) return "the process is exiting; no new thread starts";
  std::lock_guard<std::mutex> lk(threads_mu_);
  ReapLocked();
  auto rec = std::make_unique<HostThread>();
  rec->guest = std::move(t);
  HostThread* raw = rec.get();
  try {
    rec->host = std::thread([this, raw] { ThreadMain(raw); });
  } catch (const std::system_error& e) {
    return std::string{"the host could not start a thread: "} + e.what();
  }
  threads_.push_back(std::move(rec));
  threads_started_.fetch_add(1, std::memory_order_relaxed);
  return {};
}

void GuestProcess::ThreadMain(HostThread* rec) {
  GuestThread& t = *rec->guest;
  if (!BindCurrentHostThread(t)) {  // a fresh GuestThread: cannot be held elsewhere
    std::lock_guard<std::mutex> lk(threads_mu_);
    thread_faults_.push_back("guest thread tid " + std::to_string(t.tid) + " could not be bound to its host thread");
    rec->done.store(true, std::memory_order_release);
    return;
  }
  exec::RunResult rr{};
  // Re-entered while the turn's budget (or the per-turn syscall cap) runs out: a guest thread runs until it
  // calls `exit`, stops with the process, or faults.
  for (;;) {
    // Signal delivery happens inside `RunThread`, at every slice boundary (C26): it covers every kind of
    // guest thread and is far more responsive than this 2^30-instruction turn. Nothing to do here.
    rr = abi_->RunThread(t, kThreadTurnBudget);
    if (rr.reason == exec::StopReason::Budget && !monitor_->group_exited()) continue;
    break;
  }
  // Budget while the process is exiting is a clean stop (the turn ended as the group exit landed), not a fault.
  const bool clean = rr.reason == exec::StopReason::Exited ||
                     (rr.reason == exec::StopReason::Budget && monitor_->group_exited());
  if (!clean) {
    // A fault or a refusal that stopped a guest thread ends the process, as a fatal signal in one thread
    // does on a device — the other threads must not carry on around a dead one (a joiner would wait forever).
    std::string regs = " pc " + Hex(t.state.pc) + " lr " + Hex(t.state.r[14]) + " sp " + Hex(t.state.r[13]);
    for (unsigned i = 0; i < 13; ++i) regs += " r" + std::to_string(i) + " " + Hex(t.state.r[i]);
    const std::string why = "guest thread tid " + std::to_string(t.tid) + " stopped: " + ReasonName(rr.reason) +
                            (rr.detail.empty() ? "" : " (" + rr.detail + ")") + " at" + regs;
    {
      std::lock_guard<std::mutex> lk(threads_mu_);
      thread_faults_.push_back(why);
    }
    Ggl::Held hold(abi_->ggl());  // the log callback and the monitor are GGL-protected
    Log("threads: " + why + " — the process stops with it");
    monitor_->RequestGroupExit(why);
  }
  UnbindCurrentHostThread();
  rec->done.store(true, std::memory_order_release);
}

void GuestProcess::ReapLocked() {
  for (auto it = threads_.begin(); it != threads_.end();) {
    if ((*it)->done.load(std::memory_order_acquire)) {
      if ((*it)->host.joinable()) (*it)->host.join();  // past its last statement: returns at once
      it = threads_.erase(it);
    } else {
      ++it;
    }
  }
}

std::size_t GuestProcess::running_threads() const {
  std::lock_guard<std::mutex> lk(threads_mu_);
  std::size_t n = 0;
  for (const auto& r : threads_) n += r->done.load(std::memory_order_acquire) ? 0 : 1;
  return n;
}

bool GuestProcess::WaitForThreads(std::chrono::milliseconds limit) {
  const auto until = std::chrono::steady_clock::now() + limit;
  for (;;) {
    if (running_threads() == 0) return true;
    if (std::chrono::steady_clock::now() >= until) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::vector<std::string> GuestProcess::thread_faults() const {
  std::lock_guard<std::mutex> lk(threads_mu_);
  return thread_faults_;
}

std::string GuestProcess::StackWords(GuestAddr sp, std::size_t bytes, std::size_t max_frames) const {
  std::string out;
  std::size_t frames = 0;
  for (std::size_t i = 0; i < bytes / 4 && frames < max_frames; ++i) {
    std::uint32_t word = 0;
    if (!mem_->Read(sp + static_cast<std::uint32_t>(i) * 4u, &word, 4)) break;
    for (const auto& m : modules_) {
      // `data_start` is the first writable PT_LOAD. A word at or above it is a data pointer (a vtable,
      // a literal), not a return address; the scan names code only, so an abort's caller chain is not
      // buried under pointers into the image's data segment.
      if (m != nullptr && word >= m->base && word - m->base < m->extent &&
          (m->data_start == 0 || word < m->data_start)) {
        out += (out.empty() ? "" : " <- ") +
               (m->soname.empty() ? m->path : m->soname) + "+" + Hex(word - m->base);
        ++frames;
        break;
      }
    }
  }
  return out;
}

std::vector<std::string> GuestProcess::StallReport() {
  const auto name = [this](GuestAddr a) {
    for (const auto& m : modules_) {
      if (m != nullptr && a >= m->base && a - m->base < m->extent) {
        return (m->soname.empty() ? m->path : m->soname) + "+" + Hex(a - m->base);
      }
    }
    return Hex(a);
  };
  // Return addresses still on the thread's stack, named against the modules — a scan, not an unwind, the
  // same technique the fatal-stop report uses. It names the call chain that reached the wait.
  const auto stack = [this](const GuestThread& t) { return StackWords(t.state.r[13], 512, 6); };
  const auto one = [this, &name, &stack](const GuestThread& t, const char* kind, bool with_stack) {
    static const char* const kWaitNames[] = {"running", "futex", "sleep", "host(held GGL)",
                                             "host(GGL released)"};
    const int wk = t.wait_kind.load();
    std::string s = std::string{kind} + " tid " + std::to_string(t.tid);
    if (t.name[0] != '\0') s += std::string{" '"} + t.name.data() + "'";
    s += ": pc " + name(t.state.pc) + ", lr " + name(t.state.r[14]) + ", sp " + Hex(t.state.r[13]) +
         ", last syscall " + std::to_string(t.last_syscall.load()) + ", " +
         (wk >= 0 && wk <= 4 ? kWaitNames[wk] : "wait?");
    if (wk == static_cast<int>(GuestThread::WaitKind::kFutex)) {
      std::uint32_t word = 0xdead'beefu;
      if (!mem_->Read(t.wait_arg.load(), &word, 4)) word = 0xdead'beefu;
      s += " " + Hex(t.wait_arg.load()) + " (word " + Hex(word) + ")";
      // The words around the wait, by the wait object's own layout: a 52-byte choreographer object has its
      // 8-byte {value, refcount} cell at +0x20, its mutex at +0x24 and its condvar at +0x28 — so a dump
      // taken at the condvar's address shows the cell the waiter tests. A cell that stays zero names a
      // waiter nobody signals; a non-zero cell names a lost wake.
      std::uint32_t around[8] = {};
      if (mem_->Read(t.wait_arg.load() - 0x20u, around, sizeof around)) {
        s += ", near:";
        for (std::size_t k = 0; k < 8; ++k) s += " " + Hex(around[k]);
      }
    }
    if (wk >= static_cast<int>(GuestThread::WaitKind::kHostHeld)) {
      s += " slot " + std::to_string(t.wait_slot.load());
      const int step = t.host_step.load();
      if (step >= 0) s += " step " + std::to_string(step);
      const std::uint64_t since = t.host_since_ms.load();
      if (since != 0) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        s += " for " + std::to_string(now >= static_cast<std::int64_t>(since) ? now - static_cast<std::int64_t>(since) : 0) +
             " ms";
      }
    }
    if (with_stack) {
      const std::string frames = stack(t);
      if (!frames.empty()) s += ", stack: " + frames;
    }
    // A thread spinning through the JNI bridge with a pending Java exception says so here: the count only
    // grows on threads whose ExceptionCheck saw one, and a burst identifies the call that keeps throwing.
    if (const std::uint32_t pend = t.jni_pending.load(); pend != 0) {
      s += ", jni-pending " + std::to_string(pend);
    }
    return s;
  };

  std::vector<std::string> out;
  // `adopted_` is GGL-protected. This report is taken only after guest progress has stopped, and it is a
  // diagnostic: reading the list without taking the GGL (which a stalled thread could hold) is deliberate.
  for (const auto& t : adopted_) out.push_back(one(*t, "adopted", true));
  out.push_back(one(abi_->main_thread(), "main", true));
  std::size_t clones = 0;
  {
    std::lock_guard<std::mutex> lk(threads_mu_);
    for (const auto& rec : threads_) {
      if (rec->done.load(std::memory_order_acquire)) continue;
      ++clones;
      // A startup stall can hide its culprit past any fixed cap (Unity parks 31 workers), so the report
      // lists every live clone; it is a diagnostic printed once per stall window, not a steady-state log.
      // Parked clones carry their stack too: the chain that reached the wait is the one fact a futex
      // address and word cannot give (C46's UnityMain).
      out.push_back(one(*rec->guest, "clone", rec->guest->wait_kind.load() ==
                                             static_cast<int>(GuestThread::WaitKind::kFutex)));
    }
  }
  out.push_back(std::to_string(clones) + " clone'd thread(s), " + std::to_string(adopted_.size()) +
                " adopted; futex waits queued " +
                std::to_string(futex_.waiters()) + ", blocked " + std::to_string(futex_.blocked()) +
                ", woken " + std::to_string(futex_.woken()) + ", timeouts " + std::to_string(futex_.timed_out()));
  // The busiest futex addresses by wake count: a producer that wakes an address nobody waits on (a
  // mismatched condvar or a lost wake) shows here as wakes with matched 0, next to the parked workers'
  // address with its queue depth.
  for (const auto& [addr, st] : futex_.TopWakeAddrs(6)) {
    out.push_back("futex 0x" + Hex(addr) + ": wakes " + std::to_string(st.wakes) + ", matched " +
                  std::to_string(st.matched) + ", queued " + std::to_string(st.queued));
  }
  return out;
}

void GuestProcess::StopThreads() {
  if (threads_started_.load(std::memory_order_relaxed) == 0) return;
  // Every guest thread stops at its next check and every parked one is woken; then their host threads are
  // joined. The caller does not hold the GGL (it is a destructor), so they can take it to reach that check.
  if (monitor_ != nullptr) monitor_->RequestGroupExit("the guest process is being destroyed");
  futex_.Shutdown();
  // Take the list under the GGL (so a `clone` that was mid-way has finished adding its thread), join outside it,
  // and repeat until the list is still empty when looked at under the GGL. `Spawn` and `Monitor::Clone` refuse
  // once the group exited, so this ends; the loop is what makes "no thread outlives the process" not depend on
  // that ordering argument alone.
  for (;;) {
    std::list<std::unique_ptr<HostThread>> all;
    {
      std::unique_ptr<Ggl::Held> barrier;
      if (abi_ != nullptr) barrier = std::make_unique<Ggl::Held>(abi_->ggl());
      std::lock_guard<std::mutex> lk(threads_mu_);
      all.swap(threads_);
    }
    if (all.empty()) break;
    for (auto& r : all) {
      if (r->host.joinable()) r->host.join();
    }
  }
}

// ------------------------------------------------------------------------------------ adopted threads (Task 5)
//
// An ART thread (any host thread with no binding) entering guest code for the first time gets what bionic's
// pthread_create would have built for it, so `pthread_self()`, `errno`, `gettid()` and the TLS keys work on it:
// a thread mapping shaped as `__allocate_thread_mapping(1 MiB, PTHREAD_GUARD_SIZE)` shapes one, the
// `pthread_internal_t` carved just below the static TLS (pthread_create's `sub.w r0, r0, #0x2c8`), and libc's own
// init functions run on it. Detached: an ART thread is never joined, and at its exit the host does the part of
// `pthread_exit` that matters to the guest (key destructors, unlink, unmap).

namespace {

// [image] the pthread_attr_t inside pthread_internal_t (+0x10): flags +0, stack_base +4, stack_size +8,
// guard_size +0xc (the facts below assert each where libc's own code uses it).
constexpr std::uint32_t kThAttrStackBase = 0x14, kThAttrStackSize = 0x18, kThAttrGuardSize = 0x1c;
constexpr std::uint32_t kPthreadAttrFlagDetached = 0x1;  // [image] __init_thread: bit 0 -> join_state 3
constexpr std::uint32_t kThJoinState = 0x28;
constexpr std::uint32_t kThreadDetached = 3;  // [image] THREAD_DETACHED

const Fact kAdoptionFacts[] = {
    {"__init_thread", {0x07c2}, "__init_thread tests attr.flags bit 0, PTHREAD_ATTR_FLAG_DETACHED (`lsls r2, r0, #0x1f`)"},
    {"__init_thread", {0x2203}, "__init_thread's THREAD_DETACHED is 3 (`movne r2, #0x3`)"},
    {"__init_thread", {0x62a2}, "pthread_internal_t::join_state at +0x28 (`str r2, [r4, #0x28]`)"},
    {"__init_thread", {0xf011, 0x0f0c}, "__init_thread makes no scheduler call unless attr.flags bits 2/3 ... (`tst.w r1, #0xc`)"},
    {"__init_thread", {0x6a20}, "... or attr.sched_policy (+0x20) is non-zero (`ldreq r0, [r4, #0x20]`)"},
    {"pthread_attr_setstack", {0xe9cc, 0x1201}, "pthread_attr_t::stack_base/stack_size at +4/+8 (`strdeq r1, r2, [r12, #4]`)"},
    {"pthread_attr_getguardsize", {0x68c0}, "pthread_attr_t::guard_size at +0xc (`ldr r0, [r0, #0xc]`)"},
    {"pthread_getattr_np", {0xf100, 0x0110}, "pthread_getattr_np copies the attr from +0x10"},
    {"pthread_getattr_np", {0x6885}, "pthread_getattr_np's main-thread test reads tid (+0x8) against getpid(): an adopted tid != pid is not main"},
    {"__init_tcb_stack_guard", {0x6201}, "__init_tcb_stack_guard stores the guard at bionic_tcb + 0x20 (`str r1, [r0, #0x20]`)"},
    {"pthread_key_clean_all", {0xee1d, 0x0f70, 0xf850, 0x4c04}, "pthread_key_clean_all reads the key data from bionic_tls (TP - 4)"},
    {"__pthread_internal_remove", {0xe9d4, 0x0100}, "__pthread_internal_remove unlinks through next/prev at +0x0/+0x4"},
};

}  // namespace

std::string GuestProcess::AdoptionFacts() {
  if (adoption_checked_) return adoption_refusal_;
  adoption_checked_ = true;
  for (const char* fn : {"__init_tcb", "__init_tcb_dtv", "__init_tcb_stack_guard", "__init_thread",
                         "__pthread_internal_add", "__pthread_internal_remove", "pthread_key_clean_all"}) {
    if (libc_->FindLocalSymbolEntry(fn) == nullptr) {
      adoption_refusal_ = std::string{fn} + " is not in libc.so's .symtab (an adopted thread is built and torn down "
                                            "by libc's own code, and this one cannot be found)";
      return adoption_refusal_;
    }
  }
  for (const Fact& f : kAdoptionFacts) {
    const loader::LocalSymbol* fn = libc_->FindLocalSymbolEntry(f.fn);
    if (fn == nullptr) {
      adoption_refusal_ = std::string{f.fn} + " is not in libc.so's .symtab (needed for: " + f.says + ")";
      return adoption_refusal_;
    }
    if (!Contains(*mem_, *fn, f.code)) {
      adoption_refusal_ = "bionic layout mismatch: " + std::string{f.says} + " — not found in " + f.fn + " at " +
                          Hex(fn->addr);
      return adoption_refusal_;
    }
  }
  const loader::LocalSymbol* lock = libc_->FindLocalSymbolEntry("g_thread_list_lock");
  if (lock == nullptr || lock->is_function || lock->size != 0x28) {
    adoption_refusal_ = "g_thread_list_lock is not a 0x28-byte pthread_rwlock_t in libc.so's .symtab";
  }
  return adoption_refusal_;
}

bool GuestProcess::CallOn(GuestThread& t, const char* fn, const abi::Args32& args, std::uint32_t* r0,
                          std::string* err) {
  const loader::LocalSymbol* s = libc_->FindLocalSymbolEntry(fn);
  if (s == nullptr) {
    if (err) *err = std::string{fn} + " is not in libc.so's .symtab";
    return false;
  }
  exec::Executor* const before = t.executor;
  t.executor = &engine(opts_.ctor_tier);  // this thread's choice only: the process default is not touched
  exec::RunResult rr{};
  // The thread's own work (its init, its key destructors at exit): re-entered at each turn's end like a clone()d
  // thread's ThreadMain, never cut off by a single call budget (fix round 1).
  const abi::Ret32 ret = abi_->CallGuestToCompletion(s->addr | (s->thumb ? 1u : 0u), args, abi::Shorty::For("I"),
                                                     kThreadTurnBudget, &rr);
  t.executor = before;
  if (rr.reason != exec::StopReason::Returned) {
    if (err) *err = std::string{fn} + " did not return: " + ReasonName(rr.reason) + " " + rr.detail;
    return false;
  }
  if (r0 != nullptr) *r0 = ret.w();
  return true;
}

GuestThread* GuestProcess::AdoptCurrentHostThread(std::string* err) {
  Ggl::Held hold(abi_->ggl());
  const auto refuse = [&](const std::string& why) -> GuestThread* {
    if (err) *err = "adopting a host thread: " + why;
    return nullptr;
  };
  if (!scope_loaded_) return refuse("the scope is not loaded (libc's thread code is not there yet)");
  if (monitor_->group_exited()) return refuse("the guest process has exited (" + monitor_->group_exit_reason() + ")");
  if (const std::string why = AdoptionFacts(); !why.empty()) return refuse(why);

  // g_thread_list_lock (a pthread_rwlock_t; its state word at +0 is 0 when nobody holds it): with the GGL held no
  // guest code runs, so a non-zero state is a holder parked mid-update — linking now would race it.
  const GuestAddr list_lock = *libc_->FindLocalSymbol("g_thread_list_lock");
  std::uint32_t lock_state = 0xffff'ffffu;
  if (!mem_->Read(list_lock, &lock_state, 4)) return refuse("g_thread_list_lock is unreadable");
  if (lock_state != 0) {
    return refuse("g_thread_list_lock is held (state " + Hex(lock_state) +
                  ") by a guest thread; the thread list cannot be joined now");
  }

  const BionicLayout& l = layout_;
  // __allocate_thread_mapping(stack, guard): guard page, stack, static TLS, 0x2000 scratch, top guard page.
  const std::uint32_t size =
      RoundUp(l.static_tls_size + kAdoptedStackBytes + kPthreadGuard + kMappingTop, mem::kGuestPage);
  const GuestAddr base = FindFree(size);
  std::string e;
  if (base == 0) return refuse("no free run of " + Hex(size) + " bytes for the thread mapping");
  const auto unmap = [&] { mem_->Unmap(base, size, nullptr); };
  if (!mem_->Map(base, kPthreadGuard, mem::kNone, &e) ||
      !mem_->Map(base + kPthreadGuard, size - kPthreadGuard - 0x1000u, mem::kRead | mem::kWrite, &e) ||
      !mem_->Map(base + size - 0x1000u, 0x1000u, mem::kNone, &e)) {
    unmap();
    return refuse("mapping the " + Hex(size) + "-byte thread mapping at " + Hex(base) + ": " + e);
  }
  const GuestAddr scratch = base + size - kMappingTop;
  const GuestAddr static_tls = scratch - l.static_tls_size;
  const GuestAddr tcb = static_tls + l.offset_bionic_tcb;
  const GuestAddr bionic_tls = static_tls + l.offset_bionic_tls;
  // pthread_create carves the struct off the stack top, 16-aligned; the stack grows down from it.
  const GuestAddr thread = (static_tls - l.thread_size) & ~0xfu;
  const GuestAddr stack_base = base + kPthreadGuard;
  const GuestAddr stack_top = thread;

  auto owned = std::make_unique<GuestThread>();
  GuestThread& t = *owned;
  t.tid = monitor_->AllocateThreadId();
  const auto undo = [&](const std::string& why) {
    monitor_->ReleaseThreadId(t.tid);
    owned.reset();  // clears this host thread's binding cell, if it was bound
    // Only a struct that is certainly not on g_thread_list may lose its memory (fix round 1): a failed
    // __pthread_internal_add may have linked it before it stopped, and a freed linked node makes the next list
    // walk fault inside libc, under g_thread_list_lock.
    if (MaybeLinked(thread)) {
      const std::string kept = "adopting a host thread: " + why + " — and its pthread_internal_t at " + Hex(thread) +
                               " may be on g_thread_list, so its " + Hex(size) + "-byte mapping at " + Hex(base) +
                               " is kept (leaked), not freed";
      adoption_faults_.push_back(kept);
      ++kept_mappings_;
      Log("threads: " + kept);
    } else {
      unmap();
    }
    return refuse(why);
  };
  const std::uint32_t words[][2] = {
      {kThTid, static_cast<std::uint32_t>(t.tid)},
      {kThCachedPid, static_cast<std::uint32_t>(kPid)},
      {kThAttrFlags, kPthreadAttrFlagDetached},
      {kThAttrStackBase, stack_base},
      {kThAttrStackSize, stack_top - stack_base},
      {kThAttrGuardSize, kPthreadGuard},
      {kThMmapBase, base},
      {kThMmapSize, size},
      {kThMmapBaseUnguarded, base + kPthreadGuard},
      {kThMmapSizeUnguarded, size - kPthreadGuard - 0x1000u},
      {kThBionicTls, bionic_tls},
      {kThBionicTcb, tcb},
  };
  for (const auto& w : words) {
    if (!mem_->Write(thread + w[0], &w[1], 4)) return undo("writing pthread_internal_t + " + Hex(w[0]));
  }
  if (!mem_->Write(bionic_tls + kBtTempBuffers, &scratch, 4) || !mem_->Write(tcb + kTcbBionicTls, &bionic_tls, 4)) {
    return undo("writing bionic_tls + 0x414 / the tcb's bionic_tls slot");
  }

  t.state.r[13] = stack_top;
  t.state.tp = tcb + l.tp_offset_in_tcb;
  t.state.pc = kReturnAddress;
  t.state.r[15] = kReturnAddress;
  t.state.mode = isa::Mode::Thumb;
  t.stack_lo = stack_base;
  t.stack_hi = stack_top;
  t.pthread_struct = thread;
  t.adopted = true;
  t.mapping = base;
  t.mapping_size = size;
  t.owner = abi_.get();
  if (!BindCurrentHostThread(t)) return undo("the new guest thread could not be bound to this host thread");

  // libc's own code fills the rest, on the new thread (its TP and stack), exactly as pthread_create does.
  std::uint32_t r0 = 0;
  if (layout_.libc_tls_offset != 0 &&
      !CallOn(t, "__init_static_tls", {abi::Value32::Ptr(static_tls)}, nullptr, &e)) {  // as pthread_create does
    return undo(e);
  }
  if (!CallOn(t, "__init_tcb", {abi::Value32::Ptr(tcb), abi::Value32::Ptr(thread)}, nullptr, &e) ||
      !CallOn(t, "__init_tcb_dtv", {abi::Value32::Ptr(tcb)}, nullptr, &e) ||
      !CallOn(t, "__init_tcb_stack_guard", {abi::Value32::Ptr(tcb)}, nullptr, &e)) {
    return undo(e);
  }
  if (!CallOn(t, "__init_thread", {abi::Value32::Ptr(thread)}, &r0, &e)) return undo(e);
  if (r0 != 0) return undo("__init_thread returned " + std::to_string(static_cast<std::int32_t>(r0)));
  std::uint32_t join_state = 0;
  if (!mem_->Read(thread + kThJoinState, &join_state, 4) || join_state != kThreadDetached) {
    return undo("__init_thread left join_state " + Hex(join_state) + ", not THREAD_DETACHED");
  }
  if (!CallOn(t, "__pthread_internal_add", {abi::Value32::Ptr(thread)}, nullptr, &e)) return undo(e);

  adopted_.push_back(std::move(owned));
  threads_adopted_.fetch_add(1, std::memory_order_relaxed);
  GuestThread* const raw = &t;
  AddHostThreadExitHook([alive = alive_, raw] {
    // Shared: adopted exits run concurrently (one's destructors may wait on another's). Before the GGL; the
    // destructor takes it exclusively, so no hook runs on a destroyed process.
    std::shared_lock<std::shared_mutex> lk(alive->mu);
    if (alive->proc != nullptr) alive->proc->RetireAdopted(raw);
  });
  Log("threads: adopted a host thread as tid " + std::to_string(t.tid) + ": pthread_internal_t at " + Hex(thread) +
      ", TP " + Hex(t.state.tp) + ", stack " + Hex(stack_base) + ".." + Hex(stack_top) + " in a " + Hex(size) +
      "-byte mapping at " + Hex(base) + "; detached, linked into g_thread_list");
  return raw;
}

bool GuestProcess::MaybeLinked(GuestAddr thread) {
  const auto head = libc_->FindLocalSymbol("g_thread_list");
  std::uint32_t at = 0;
  if (!head || !mem_->Read(*head, &at, 4)) return true;  // cannot tell: assume linked
  for (unsigned n = 0; at != 0; ++n) {
    if (at == thread || n >= 4096) return true;
    if (!mem_->Read(at, &at, 4)) return true;
  }
  return false;
}

void GuestProcess::RetireAdopted(GuestThread* t) {
  Ggl::Held hold(abi_->ggl());
  const auto it = std::find_if(adopted_.begin(), adopted_.end(), [t](const auto& p) { return p.get() == t; });
  if (it == adopted_.end()) return;
  const std::string who = "adopted tid " + std::to_string(t->tid) + " (pthread_internal_t " + Hex(t->pthread_struct) + ")";
  std::string note = "threads: " + who + " exited";
  std::string fault;  // a named failure of the guest-visible exit, reported in adoption_faults()
  const bool live = !monitor_->group_exited();
  bool bound_elsewhere = false;
  if (!live) {
    note += " after the process stopped: no guest destructor ran";
  } else if (!BindCurrentHostThread(*t)) {
    bound_elsewhere = true;
    fault = who + " exited while its guest thread was bound to another host thread: its TLS-key destructors did "
                  "not run and it was not unlinked";
  } else {
    // pthread_exit's guest-visible half, in its order: the C++ thread_local destructors (__cxa_thread_finalize),
    // the TLS-key destructors (run to completion, re-entered across turns), then the unlink — attempted even when a
    // destructor failed, so the list never keeps a node whose memory goes — and the dynamic TLS blocks.
    std::string e;
    if (!CallOn(*t, "__cxa_thread_finalize", {}, nullptr, &e)) {
      fault = who + ": a thread_local destructor did not complete — " + e;
    }
    if (!CallOn(*t, "pthread_key_clean_all", {}, nullptr, &e)) {
      fault += (fault.empty() ? who + ": " : "; ") + std::string{"a TLS-key destructor did not complete — "} + e;
    }
    if (!CallOn(*t, "__pthread_internal_remove", {abi::Value32::Ptr(t->pthread_struct)}, nullptr, &e)) {
      fault += (fault.empty() ? who + ": " : "; ") + std::string{"__pthread_internal_remove: "} + e;
    } else if (!CallOn(*t, "_Z18__free_dynamic_tlsP10bionic_tcb",
                       {abi::Value32::Ptr(t->state.tp - kTpOffsetInTcb)}, nullptr, &e)) {
      fault += (fault.empty() ? who + ": " : "; ") + std::string{"__free_dynamic_tls: "} + e;
    }
    if (fault.empty()) note += ": thread_local and key destructors ran, unlinked from g_thread_list, dynamic TLS freed";
  }
  monitor_->ReleaseThreadId(t->tid);
  // Free the memory only when the struct is certainly off the list (or the process is over: no guest code runs
  // again). A linked node must stay mapped, or the next list walk faults inside libc under g_thread_list_lock.
  if (!monitor_->group_exited() && MaybeLinked(t->pthread_struct)) {  // re-read: a destructor may have exited it
    fault += (fault.empty() ? who + ": " : "; ") + std::string{"still on g_thread_list, so its "} +
             Hex(t->mapping_size) + "-byte mapping at " + Hex(t->mapping) + " is kept (leaked), not freed";
    ++kept_mappings_;
  } else {
    std::string e;
    if (!mem_->Unmap(t->mapping, t->mapping_size, &e)) note += "; unmapping its mapping: " + e;
    interp_->Invalidate(t->mapping, t->mapping + t->mapping_size);
    if (jit_->ok()) jit_->Invalidate(t->mapping, t->mapping + t->mapping_size);
    note += "; its " + Hex(t->mapping_size) + "-byte mapping freed";
  }
  if (!fault.empty()) {
    adoption_faults_.push_back(fault);
    Log("threads: REFUSED/FAULTED exit: " + fault);
  } else {
    Log(note);
  }
  if (bound_elsewhere || !live) {
    // Another live host thread may hold it (perhaps inside RunThread with the GGL released, or still unwinding
    // after the group exit): freeing it here would leave that thread a dangling GuestThread. Keep it until the
    // process goes.
    stranded_.splice(stranded_.end(), adopted_, it);
  } else {
    adopted_.erase(it);  // the GuestThread's destructor clears this host thread's binding cell
  }
}

std::vector<std::string> GuestProcess::adoption_faults() const {
  Ggl::Held hold(const_cast<GuestProcess*>(this)->abi_->ggl());
  return adoption_faults_;
}

std::size_t GuestProcess::kept_mappings() const {
  Ggl::Held hold(const_cast<GuestProcess*>(this)->abi_->ggl());
  return kept_mappings_;
}

std::size_t GuestProcess::adopted_threads() const {
  Ggl::Held hold(const_cast<GuestProcess*>(this)->abi_->ggl());
  return adopted_.size();
}

}  // namespace rb::runtime
