/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C02 `rb::exec` — Tier J: Dynarmic's AArch64 back end behind the `Executor` seam.
 * See rb/exec/jit.h for the two-tier reasoning, the budget units, and why there is no fastmem.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/exec/jit.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

#include "dynarmic/frontend/A32/a32_ir_emitter.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/ir/terminal.h"

#include "rb/exec/interp.h"
#include "rb/isa/front_end.h"

namespace rb::exec {

namespace {

constexpr std::uint32_t kCpsrTFlag = 1u << 5;

/// `HaltReason` is a bitmask enum with `operator&` but no conversion to bool, so a test has to spell out
/// that it means "any of these bits". Named rather than an inline cast because dynarmic exports a `Has`
/// found by ADL on its own enum, and two candidates for one unqualified call is a compile error.
[[nodiscard]] constexpr bool HaltHas(Dynarmic::HaltReason got, Dynarmic::HaltReason want) {
  return (static_cast<std::uint32_t>(got) & static_cast<std::uint32_t>(want)) != 0;
}

/// The widths of Dynarmic's register views, asserted against the types rather than against a call:
/// `jit_->Regs().size()` is not a constant expression. `remove_reference` because those accessors hand
/// back lvalue references, and `sizeof` rather than `size()` because libc++ declares `std::array::size`
/// as a non-static member, so even `T::size()` needs an object.
using JitRegs = std::remove_reference_t<decltype(std::declval<Dynarmic::A32::Jit&>().Regs())>;
using JitExtRegs = std::remove_reference_t<decltype(std::declval<Dynarmic::A32::Jit&>().ExtRegs())>;
static_assert(sizeof(JitRegs) == 16 * sizeof(std::uint32_t), "the guest register file is 16 words");
static_assert(sizeof(JitExtRegs) == 64 * sizeof(std::uint32_t),
              "VFPv3-D32 is 64 single-precision words");

/// `UserConfig::code_cache_size`'s own comment gives the bounds (about 8 MiB minimum, 128 MiB maximum on
/// an arm64 host, the ceiling being how far an arm64 jump can reach). The low end is used because this is
/// a per-instance allocation in a design where a user can have several guest instances alive, not because
/// 8 MiB is enough for a big app — `RbCodeCache` (C04) is where sizing policy belongs.
constexpr std::size_t kCodeCacheBytes = 8u << 20;

/**
 * The size in bytes of the instruction at `pc`.
 *
 * The fallback hand-back has to advance *exactly one* guest encoding, and for Thumb that is decidable
 * from the first halfword: bit pattern 0b111 in bits 13-15 (with 11-12 not 0b00) means a 32-bit
 * Thumb-32 encoding. Reading the halfword here rather than re-deriving it from the IR is deliberate —
 * `InterpreterFallback` is called precisely when translation declined, so there may be no IR to consult.
 */
unsigned EncodingSizeAt(mem::GuestMemory& memory, GuestAddr pc, isa::Mode mode) {
  if (mode == isa::Mode::Arm) return 4;
  std::uint16_t hw = 0;
  if (!memory.Read(pc, &hw, 2)) return 2;  // unmapped: the caller's next read will fault and report it
  const bool wide = (hw & 0xE000u) == 0xE000u && (hw & 0x1800u) != 0;
  return wide ? 4 : 2;
}

const char* ExceptionName(Dynarmic::A32::Exception e) {
  using Dynarmic::A32::Exception;
  switch (e) {
    case Exception::UndefinedInstruction: return "undefined instruction encoding";
    case Exception::UnpredictableInstruction: return "unpredictable instruction";
    case Exception::DecodeError: return "decoder error";
    case Exception::SendEvent: return "SEV";
    case Exception::SendEventLocal: return "SEVL";
    case Exception::WaitForInterrupt: return "WFI";
    case Exception::WaitForEvent: return "WFE";
    case Exception::Yield: return "YIELD";
    case Exception::Breakpoint: return "BKPT";
    case Exception::PreloadData: return "PLD";
    case Exception::PreloadDataWithIntentToWrite: return "PLDW";
    case Exception::PreloadInstruction: return "PLI";
    case Exception::NoExecuteFault: return "instruction fetch from non-executable memory";
  }
  return "unknown exception";
}

/// Lowercase hex, no prefix — the same shape `interp.cpp` uses, so a refusal from either tier prints an
/// address a reader can compare across the two without converting it.
std::string Hex(std::uint32_t v) {
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%x", v);
  return buf;
}

/**
 * The six ARM-state encodings whose translators end in `InterpretThisInstruction()`, written as the
 * pattern strings Dynarmic's own decoder uses.
 *
 * Copied verbatim from `frontend/A32/decoder/arm.inc` rather than transcribed into hex `(mask, value)`
 * pairs, because that conversion is exactly the kind of work that produces a plausible wrong answer: one
 * bit misplaced here either vetoes a legal instruction forever or lets a declined one reach the back-end
 * abort, and neither outcome shows up as a failing build. `MakeForm` performs the conversion at compile
 * time from the same literal, and the length `static_assert` is what turns a truncated copy-paste into a
 * compiler error instead of a narrow mask.
 *
 * All six are ARM state (`arm_*`), which is why the veto below can skip Thumb outright.
 */
struct DeclinedForm {
  std::uint32_t mask;
  std::uint32_t value;
  unsigned ones;  ///< bits set in `mask`, which is how Dynarmic ranks decoder specificity
  const char* body;
};

constexpr unsigned OnesOf(std::uint32_t m) {
  unsigned n = 0;
  for (; m; m &= m - 1) ++n;
  return n;
}

template <std::size_t N>
constexpr DeclinedForm MakeForm(const char (&pattern)[N], const char* body) {
  static_assert(N - 1 == 32, "a Dynarmic decoder pattern is 32 characters of 0/1/-");
  std::uint32_t mask = 0, value = 0;
  for (unsigned i = 0; i < 32; ++i) {
    const char c = pattern[i];
    if (c == '-') continue;
    mask |= 1u << (31 - i);
    if (c == '1') value |= 1u << (31 - i);
  }
  return {mask, value, OnesOf(mask), body};
}

/**
 * Verbatim from `decoder/arm.inc`. The order here is *not* what decides which body a message names:
 * `decoder/arm.h:45-47` stable-sorts dynarmic's table by `count_ones` of each **mask** — not of the
 * value bits — and first match wins, so the loop in `PreCodeReadHook` applies the same rule (more mask
 * bits first, ties keeping `arm.inc` order).
 *
 * That distinction is not pedantry: `arm_LDM_usr` and `arm_LDM_eret` both pin six mask bits, so they
 * tie and `arm_LDM_usr` is reached first for any encoding where both match. Concretely,
 * `ldmdb r0, {r4, r5, pc}^` is `arm_LDM_usr` as far as dynarmic is concerned, and only adding the
 * writeback bit separates the two. Which body a declined instruction "was" is not observable from the
 * embedder side either way — all six end in the same `Term::Interpret` — so the name below is an
 * inference from this table, and the C05 lane pins it down by using only encodings where exactly one
 * pattern matches.
 */
constexpr DeclinedForm kDeclinedForms[] = {
    MakeForm("----100--101--------------------", "arm_LDM_usr"),
    MakeForm("----100--1-1----1---------------", "arm_LDM_eret"),
    MakeForm("----100--100--------------------", "arm_STM_usr"),
    MakeForm("111100010000---00000000---0-----", "arm_CPS"),
    MakeForm("1111100--0-1----0000101000000000", "arm_RFE"),
    MakeForm("1111100--1-0110100000101000-----", "arm_SRS"),
};

/**
 * Coprocessor 15 for an AArch32 guest: TPIDRURW modelled, everything else refused by name.
 *
 * Registering *something* is the load-bearing part. With `Config::coprocessors[15]` empty, Dynarmic's
 * AArch64 back end compiles every `mrc`/`mcr`/`cdp` into `EmitCoprocessorException()`, which is
 * `ASSERT_FALSE("Should raise coproc exception here")` (`emit_arm64_a32_coprocessor.cpp:23`) — and mcl's
 * `ASSERT` is not `NDEBUG`-gated, so `std::terminate` runs inside `Run()` and the whole RetroBug process
 * dies. Same failure class §Step 21 removed for `Term::Interpret`, found here on real code and far sooner
 * than anyone expected: the device's `libc.so` begins `__errno()` with
 *
 *     76dec:  ee1d 0f70   mrc p15, #0x0, r0, c13, c0, #0x3
 *     76df0:  6840        ldr r0, [r0, #0x4]
 *     76df2:  f500 7027   add.w r0, r0, #0x29c
 *
 * so one overflowing `atoi` — one `errno` write — was enough to abort `rb_guest_libc_tests` mid-run.
 * `llvm-objdump -d` over the whole image counts 98 coprocessor accesses and they are **all** this one
 * register: no other coprocessor number, no other `c13` slot, and no `mcr` at all. So modelling the
 * thread-pointer read is not a corner case priced against a crash-prone default; it is the entire set of
 * what libc asks for. The write form is here because it is that register's other half and a guest that
 * sets `tp` must not find a hole — `[UNVERIFIED]` that any corpus code does, since none was observed.
 *
 * A refusal is a *stop*, not a zero: an unmodelled coprocessor must not hand back a value the guest then
 * dereferences, which is what the memory callbacks are able to get away with only because
 * `check_halt_on_memory_access` re-tests after every data access. There is no such re-test for a
 * coprocessor result, so the halt comes first and the guest never sees a plausible register.
 */
class Arm15Coprocessor final : public Dynarmic::A32::Coprocessor {
 public:
  // An alias, not a using-declaration: in class scope `Namespace::Name` is only legal when the qualifier
  // is a base class, so the latter is a compile error here and the former is not.
  using CoprocReg = Dynarmic::A32::CoprocReg;

  explicit Arm15Coprocessor(JitExecutor& exec) : exec_{exec} {}

  std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, CoprocReg CRd, CoprocReg CRn,
                                                   CoprocReg CRm, unsigned opc2) override {
    return Callback{&Refuse, Site(false, false, Describe("CDP", two, opc1, CRn, CRm, opc2))};
  }

  CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm,
                                             unsigned opc2) override {
    if (IsThreadPointer(two, opc1, CRn, CRm, opc2)) {
      return Callback{&WriteThreadPointer, Site(false, true, {})};
    }
    return Callback{&Refuse, Site(false, false, Describe("MCR", two, opc1, CRn, CRm, opc2))};
  }

  CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm,
                                            unsigned opc2) override {
    if (IsThreadPointer(two, opc1, CRn, CRm, opc2)) {
      return Callback{&ReadThreadPointer, Site(true, false, {})};
    }
    return Callback{&Refuse, Site(false, false, Describe("MRC", two, opc1, CRn, CRm, opc2))};
  }

  CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
    return Callback{&Refuse, Site(false, false, DescribePair("MCRR", two, opc, CRm))};
  }

  CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
    return Callback{&Refuse, Site(false, false, DescribePair("MRRC", two, opc, CRm))};
  }

  std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg CRd,
                                           std::optional<std::uint8_t>) override {
    return Callback{&Refuse,
                    Site(false, false,
                         std::string{"LDC c"} + std::to_string(static_cast<unsigned>(CRd)))};
  }

  std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg CRd,
                                            std::optional<std::uint8_t>) override {
    return Callback{&Refuse,
                    Site(false, false,
                         std::string{"STC c"} + std::to_string(static_cast<unsigned>(CRd)))};
  }

 private:
  /// `mrc`/`mcr p15, #0, rX, c13, c0, #3`: ARMv7's user-mode thread ID register, which is what bionic's
  /// `__get_tls()` compiles to and therefore what `__errno()`, `assert`, and the property and locale
  /// accessors all begin with.
  static bool IsThreadPointer(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    return !two && opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc2 == 3;
  }

  static std::string Describe(const char* mnemonic, bool two, unsigned opc1, CoprocReg CRn,
                              CoprocReg CRm, unsigned opc2) {
    return std::string{mnemonic} + (two ? "2" : "") + " p15, #" + std::to_string(opc1) + ", c" +
           std::to_string(static_cast<unsigned>(CRn)) + ", c" +
           std::to_string(static_cast<unsigned>(CRm)) + ", #" + std::to_string(opc2);
  }

  static std::string DescribePair(const char* mnemonic, bool two, unsigned opc, CoprocReg CRm) {
    return std::string{mnemonic} + (two ? "2" : "") + " p15, #" + std::to_string(opc) + ", c" +
           std::to_string(static_cast<unsigned>(CRm));
  }

  JitExecutor::CoprocSite* Site(bool tp_read, bool tp_write, std::string detail) {
    return exec_.CoprocSiteFor(tp_read, tp_write, std::move(detail));
  }

  static std::uint64_t ReadThreadPointer(void* user_arg, std::uint32_t, std::uint32_t) {
    const auto* site = static_cast<const JitExecutor::CoprocSite*>(user_arg);
    MachineState* s = site->exec->live_state();
    return s != nullptr ? s->tp : 0u;
  }

  static std::uint64_t WriteThreadPointer(void* user_arg, std::uint32_t value, std::uint32_t) {
    const auto* site = static_cast<const JitExecutor::CoprocSite*>(user_arg);
    MachineState* s = site->exec->live_state();
    if (s != nullptr) s->tp = value;
    return 0;
  }

  static std::uint64_t Refuse(void* user_arg, std::uint32_t, std::uint32_t) {
    const auto* site = static_cast<const JitExecutor::CoprocSite*>(user_arg);
    site->exec->RefuseCoprocessor(*site);
    return 0;
  }

  JitExecutor& exec_;
};

}  // namespace

JitExecutor::JitExecutor(mem::GuestMemory& memory, GuestAddr return_address,
                         const HostDispatch* dispatch)
    : memory_{memory}, return_address_{return_address}, dispatch_{dispatch} {
  Dynarmic::A32::UserConfig conf{this};
  // ARMv7-A, not the default. `UserConfig::arch_version` ships as `v8`, which would accept encodings no
  // armeabi-v7a binary may contain and is the same trap `rb::isa::Options()` already pins to v7 — the
  // two tiers have to agree on the *architecture*, or a differential disagreement would be an
  // configuration difference wearing its clothes.
  conf.arch_version = Dynarmic::A32::ArchVersion::v7;
  // The other two options `rb::isa::Options()` sets, and which this config was missing.
  //
  // `hook_hint_instructions` decides whether Dynarmic *tells the caller* about a hint instruction at all:
  // `arm_PLD_imm` and its neighbours emit nothing when it is clear (`frontend/A32/translate/impl/
  // hint.cpp:11-18`) and an `A32ExceptionRaised` when it is set. Tier I's front end sets it, Tier J's did
  // not, so for every hint -- `pld`, `yield`, `sev`, and `bkpt` -- the two tiers were handed different IR
  // rather than the same IR answered differently, which is the one divergence a differential cannot see
  // from its own output. It also meant `bkpt` decoded to a NOP on the shipping tier, where the front-end
  // comment's stated policy is that a trap has to reach RetroBug.
  //
  // `define_unpredictable_behaviour` is the same kind of mismatch in the other direction: with it clear,
  // an unpredictable encoding raises `UnpredictableInstruction` and stops the guest here, while Tier I ran
  // Dynarmic's defined behaviour for it. `docs/30` §Step 37 records what each of the four lanes did before
  // and after, because this pair of lines is the first time both tiers were asked the same question.
  conf.hook_hint_instructions = true;
  conf.define_unpredictable_behaviour = true;
  // Android's guest is little-endian and CPSR.E is not reachable from an app; forcing it removes a
  // whole axis of state that neither tier can test.
  conf.always_little_endian = true;
  // A fault cannot be returned from MemoryRead32. Halting after every data access is what makes
  // "record the fault, ask the core to stop, hand back zero" safe rather than a value the guest gets to
  // use; docs/10 §8.2 named this flag for exactly this reason.
  conf.check_halt_on_memory_access = true;
  // Cycle counting is the budget mechanism, not an accounting pretence: GetTicksForCode gives one tick
  // per guest encoding, so `executed_` is a real guest-instruction count and `max_instructions` means
  // the same thing here that it claims to.
  conf.enable_cycle_counting = true;
  conf.code_cache_size = kCodeCacheBytes;
  // A diagnostic, not a feature: the exclusive-load disagreement in `docs/30` §Step 30 has to be
  // attributed to either Dynarmic's IR optimizations or its AArch64 code generation before anything is
  // "fixed", and this is the only way to ask that question without editing the vendored tree.
  if (const char* noopt = std::getenv("RB_JIT_NOOPT"); noopt != nullptr && noopt[0] != '\0' && noopt[0] != '0') {
    conf.optimizations = static_cast<Dynarmic::OptimizationFlag>(0);
  }
  // Coprocessor 15 has to be present before the `Jit` is built, because the decision whether to compile a
  // callback or an assertion is made at translation time and the shared_ptr is baked into the code cache's
  // lifetime. Constructing it here rather than in the member list: it names `*this`, and a
  // self-reference must not be handed out before the object it points at is far enough along to answer.
  coproc15_ = std::make_shared<Arm15Coprocessor>(*this);
  conf.coprocessors[15] = coproc15_;
  // The exclusive monitor, because Dynarmic dereferences this pointer unconditionally in the trampoline
  // it bakes for every LL/SC access. `processor_id` indexes that monitor's per-core slot, and this object
  // is one core, so it is 0 in a 1-processor monitor. See the member's declaration for why correctness
  // does not depend on the monitor being shared.
  conf.global_monitor = &monitor_;
  conf.processor_id = 0;

  try {
    jit_ = std::make_unique<Dynarmic::A32::Jit>(conf);
  } catch (const std::exception& e) {
    err_ = std::string{"could not build the AArch64 back end: "} + e.what();
    return;
  }
  fallback_interp_ = std::make_unique<InterpExecutor>(memory_, return_address, dispatch);
}

JitExecutor::~JitExecutor() = default;

JitExecutor::CoprocSite* JitExecutor::CoprocSiteFor(bool tp_read, bool tp_write, std::string detail) {
  // One site per distinct access, because the pointer is compiled into every block that mentions the
  // register and a fresh site per *reference* would grow without bound — `libc.so` reads the thread pointer
  // from 98 places, which is 98 blocks and one site. A `std::deque` rather than a `std::vector` because
  // appending must not move what is already referenced; the linear scan is over a handful of entries.
  for (auto& s : coproc_sites_) {
    if (s.tp_read == tp_read && s.tp_write == tp_write && s.detail == detail) return &s;
  }
  coproc_sites_.push_back(CoprocSite{this, tp_read, tp_write, std::move(detail)});
  return &coproc_sites_.back();
}

void JitExecutor::RefuseCoprocessor(const CoprocSite& site) {
  std::string detail = "the coprocessor access ";
  detail += site.detail.empty() ? "p15 c13 (thread pointer)" : site.detail;
  // R15 is the address, and it is trustworthy *here* specifically because a coprocessor callback is a
  // call: `PrepareForCall` flushes the register allocators' live state into the jit state first, which is
  // also why the callbacks can read and write `live_` at all. `live_->pc` would not do — that field holds
  // the address the `Run` was entered at, and by now the guest has moved.
  if (jit_ != nullptr) {
    detail += " at 0x" + Hex(jit_->Regs()[15]) + " is not modelled";
  } else {
    detail += " is not modelled";
  }
  SetPending(Pending::Unimplemented, std::move(detail));
  if (jit_ != nullptr) {
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
}

void JitExecutor::SyncIn(const MachineState& state) {
  // A Run begins with no exclusive reservation, in both tiers — `InterpExecutor::Run` clears its
  // `ExclusiveState` the same way. This is a *defined starting condition* rather than an architectural
  // claim: the marker is not guest-visible, so what matters is that neither tier can carry one in from a
  // previous Run and have an STREX pass on the first block of the next one differently.
  jit_->ClearExclusiveState();
  monitor_.ClearProcessor(0);

  auto& regs = jit_->Regs();
  static_assert(sizeof(JitRegs) == 16 * sizeof(std::uint32_t), "the guest register file is 16 words");
  std::copy(std::begin(state.r), std::end(state.r), regs.begin());

  // CPSR bit 5 is the JIT's authority on ARM vs Thumb, while Tier I carries the mode on the block's
  // location descriptor. Both are written here so neither tier can be running the wrong ISA off a stale
  // bit — and SyncOut reads it back the same way, so a guest `bx` that flips the mode survives.
  std::uint32_t cpsr = state.cpsr;
  if (state.mode == isa::Mode::Thumb) {
    cpsr |= kCpsrTFlag;
  } else {
    cpsr &= ~kCpsrTFlag;
  }
  jit_->SetCpsr(cpsr);
  jit_->SetFpscr(state.fpscr);

  // ExtRegs is the VFP bank as 64 32-bit words: S<2n> is D<n>'s low half and S<2n+1> its high half,
  // which is the same little-endian pairing `interp.cpp` uses for Get/SetExtendedRegister32. Two tiers
  // with two different orderings here would show up as garbage doubles, not as an obvious failure.
  auto& ext = jit_->ExtRegs();
  static_assert(sizeof(JitExtRegs) == 64 * sizeof(std::uint32_t), "VFPv3-D32 is 64 words");
  for (int i = 0; i < 32; ++i) {
    ext[static_cast<std::size_t>(i) * 2] = static_cast<std::uint32_t>(state.d[i]);
    ext[static_cast<std::size_t>(i) * 2 + 1] = static_cast<std::uint32_t>(state.d[i] >> 32);
  }
  // PC last: setting it is what makes the core runnable, and nothing above depends on it.
  regs[15] = state.pc;
}

void JitExecutor::SyncOut(MachineState& state) {
  const auto& regs = jit_->Regs();
  std::copy(regs.begin(), regs.end(), std::begin(state.r));
  state.pc = regs[15];
  state.r[15] = regs[15];
  const std::uint32_t cpsr = jit_->Cpsr();
  state.cpsr = cpsr;
  state.mode = (cpsr & kCpsrTFlag) ? isa::Mode::Thumb : isa::Mode::Arm;
  // The shared derivation, not a local unpack: see ItStateFromCpsr. Tier I used to take this field from
  // the block's location descriptor and this tier from CPSR, and the two disagree at the moment
  // execution stops, which is a false finding with a real-looking number attached.
  state.itstate = ItStateFromCpsr(cpsr);
  state.fpscr = jit_->Fpscr();
  auto& ext = jit_->ExtRegs();
  for (int i = 0; i < 32; ++i) {
    state.d[i] = static_cast<std::uint64_t>(ext[static_cast<std::size_t>(i) * 2]) |
                 (static_cast<std::uint64_t>(ext[static_cast<std::size_t>(i) * 2 + 1]) << 32);
  }
}

RunResult JitExecutor::Run(MachineState& state, unsigned max_instructions) {
  RunResult out{};
  if (!ok()) {
    out.reason = StopReason::Internal;
    out.detail = err_;
    return out;
  }
  live_ = &state;
  budget_ = max_instructions;
  executed_ = 0;
  pending_ = Pending::None;
  pending_detail_.clear();
  pending_slot_ = 0;
  pending_svc_ = 0;
  pending_pc_ = 0;

  SyncIn(state);
  const Dynarmic::HaltReason hr = jit_->Run();
  SyncOut(state);
  if ((pending_ == Pending::Returned || pending_ == Pending::HostCall) && pending_pc_ != 0) {
    // Dynarmic advances PC across the fetch it could not read, which leaves the core pointing one
    // instruction *past* the address RetroBug stopped on. The stop address is the one that means
    // something — it is where the sentinel or the host slot lives, and where the call bridge looks to
    // resume — so the state is normalised back to it rather than to the core's arithmetic.
    state.pc = pending_pc_;
    state.r[15] = pending_pc_;
  }

  live_ = nullptr;
  return MapHalt(hr, executed_);
}

RunResult JitExecutor::MapHalt(Dynarmic::HaltReason hr, unsigned executed) {
  RunResult out{};
  out.instructions_executed = executed;
  switch (pending_) {
    case Pending::None:
      // No RetroBug-side reason was recorded, so the halt came from dynarmic's own vocabulary.
      if (HaltHas(hr, Dynarmic::HaltReason::MemoryAbort)) {
        out.reason = StopReason::Fault;
        out.detail = "guest memory access faulted inside compiled code";
      } else if (HaltHas(hr, Dynarmic::HaltReason::CacheInvalidation)) {
        out.reason = StopReason::Budget;  // self-modifying code: resumable, and worth a diagnostic
        out.detail = "code cache invalidated during execution";
      } else if (HaltHas(hr, Dynarmic::HaltReason::Step)) {
        out.reason = StopReason::Budget;
      } else {
        out.reason = StopReason::Suspended;
      }
      break;
    case Pending::Returned:
      out.reason = StopReason::Returned;
      break;
    case Pending::HostCall:
      out.reason = StopReason::HostCall;
      out.host_slot = pending_slot_;
      break;
    case Pending::Syscall:
      out.reason = StopReason::Syscall;
      out.svc_number = pending_svc_;
      out.detail = std::move(pending_detail_);
      break;
    case Pending::Fault:
      out.reason = StopReason::Fault;
      out.detail = std::move(pending_detail_);
      break;
    case Pending::Unimplemented:
      out.reason = StopReason::Unimplemented;
      out.detail = std::move(pending_detail_);
      break;
    case Pending::Suspended:
      out.reason = StopReason::Suspended;
      out.detail = std::move(pending_detail_);
      break;
  }
  return out;
}

bool JitExecutor::SurfaceNonCodeDestination(GuestAddr pc) {
  if (pc == return_address_) {
    SetPending(Pending::Returned);
    pending_pc_ = return_address_;
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    return true;
  }
  std::uint32_t slot = 0;
  if (dispatch_ != nullptr && dispatch_->IsHostEntry(pc, &slot)) {
    SetPending(Pending::HostCall);
    pending_slot_ = slot;
    pending_pc_ = pc;
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    return true;
  }
  return false;
}

/**
 * The front-end read. Returning `nullopt` is how a fault is signalled to the translator.
 *
 * This is *one* of the two places the call sentinel is recognised, and only having it here is a trap
 * that cost a cycle: the first guest return translates the block at the sentinel, but Dynarmic then
 * **caches the block it built for that address**, so every later return executes the cached
 * raise-`NoExecuteFault` block and never asks for the code again. The intercept then silently vanishes
 * after one call — observed as the first case ending `Returned` and every case after it ending `Fault`.
 * `ExceptionRaised` therefore performs the same check, and this one is for the addresses a guest can
 * only reach by fetching, such as an unmapped page.
 */
std::optional<std::uint32_t> JitExecutor::MemoryReadCode(GuestAddr vaddr) {
  if (SurfaceNonCodeDestination(vaddr)) return std::nullopt;
  std::uint32_t v = 0;
  if (!memory_.Read(vaddr, &v, 4)) return std::nullopt;
  return v;
}

std::uint8_t JitExecutor::MemoryRead8(GuestAddr vaddr) {
  std::uint8_t v = 0;
  if (!memory_.Read(vaddr, &v, 1)) {
    SetPending(Pending::Fault,
               "guest memory read at " + std::to_string(vaddr) + " is unmapped or not readable");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
  return v;
}

std::uint16_t JitExecutor::MemoryRead16(GuestAddr vaddr) {
  std::uint16_t v = 0;
  if (!memory_.Read(vaddr, &v, 2)) {
    SetPending(Pending::Fault,
               "guest memory read at " + std::to_string(vaddr) + " is unmapped or not readable");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
  return v;
}

std::uint32_t JitExecutor::MemoryRead32(GuestAddr vaddr) {
  std::uint32_t v = 0;
  if (!memory_.Read(vaddr, &v, 4)) {
    SetPending(Pending::Fault,
               "guest memory read at " + std::to_string(vaddr) + " is unmapped or not readable");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
  return v;
}

std::uint64_t JitExecutor::MemoryRead64(GuestAddr vaddr) {
  std::uint64_t v = 0;
  if (!memory_.Read(vaddr, &v, 8)) {
    SetPending(Pending::Fault,
               "guest memory read at " + std::to_string(vaddr) + " is unmapped or not readable");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
  return v;
}

void JitExecutor::MemoryWrite8(GuestAddr vaddr, std::uint8_t value) {
  if (!memory_.Write(vaddr, &value, 1)) {
    SetPending(Pending::Fault,
               "guest memory write at " + std::to_string(vaddr) +
                   " is unmapped, read-only, or an executing page");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
}

void JitExecutor::MemoryWrite16(GuestAddr vaddr, std::uint16_t value) {
  if (!memory_.Write(vaddr, &value, 2)) {
    SetPending(Pending::Fault,
               "guest memory write at " + std::to_string(vaddr) +
                   " is unmapped, read-only, or an executing page");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
}

void JitExecutor::MemoryWrite32(GuestAddr vaddr, std::uint32_t value) {
  if (!memory_.Write(vaddr, &value, 4)) {
    SetPending(Pending::Fault,
               "guest memory write at " + std::to_string(vaddr) +
                   " is unmapped, read-only, or an executing page");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
}

void JitExecutor::MemoryWrite64(GuestAddr vaddr, std::uint64_t value) {
  if (!memory_.Write(vaddr, &value, 8)) {
    SetPending(Pending::Fault,
               "guest memory write at " + std::to_string(vaddr) +
                   " is unmapped, read-only, or an executing page");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
}

bool JitExecutor::ExclusiveCompareAndStore(GuestAddr vaddr, std::uint64_t value, std::uint64_t expected,
                                           std::size_t bytes) {
  // Byte-wise, not a typed `Write(vaddr, &value, bytes)`: the four callers hand a 64-bit `value` for every
  // width, and the same assembly-by-shift rule Tier I uses keeps the two tiers from disagreeing over host
  // endianness instead of over the memory model.
  unsigned char raw[8] = {};
  if (!memory_.Read(vaddr, raw, bytes)) {
    SetPending(Pending::Fault,
               "guest exclusive access at " + std::to_string(vaddr) + " is unmapped or not readable");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    return false;
  }
  std::uint64_t current = 0;
  for (std::size_t i = 0; i < bytes; ++i) {
    current |= static_cast<std::uint64_t>(raw[i]) << (i * 8);
  }
  // Dynarmic has already dropped the marker and matched the address; the value is the half that makes the
  // pair atomic. A word that changed since the exclusive load means someone else got in, so the store
  // fails with `Rd = 1` and the guest retries — which is the behaviour `pthread_mutex_lock` depends on.
  if (current != expected) return false;

  for (std::size_t i = 0; i < bytes; ++i) {
    raw[i] = static_cast<unsigned char>(value >> (i * 8));
  }
  if (!memory_.Write(vaddr, raw, bytes)) {
    SetPending(Pending::Fault,
               "guest exclusive write at " + std::to_string(vaddr) +
                   " is unmapped, read-only, or an executing page");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    return false;
  }
  return true;
}

void JitExecutor::CallSVC(std::uint32_t swi) {
  pending_svc_ = swi;
  SetPending(Pending::Syscall, SvcDetail(swi));
  jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

void JitExecutor::ExceptionRaised(GuestAddr pc, Dynarmic::A32::Exception exception) {
  /**
   * A hint whose architectural effect is nothing executes as nothing, and the guest carries on -- the
   * same rule Tier I applies to the same IR op, from the same predicate. Returning without halting is
   * what "continue" means here: the raise is not the block's terminal, so the compiled block walks on to
   * the encoding after the hint exactly as hardware does.
   */
  if (isa::HintIsNoOp(exception)) return;
  const char* name = ExceptionName(exception);
  if (exception == Dynarmic::A32::Exception::NoExecuteFault) {
    // The other half of the sentinel/slot check — see MemoryReadCode for why both are needed.
    if (SurfaceNonCodeDestination(pc)) return;
    SetPending(Pending::Fault, "instruction fetch failed at " + std::to_string(pc) +
                                   " (no code could be read there)");
  } else if (exception == Dynarmic::A32::Exception::Breakpoint) {
    SetPending(Pending::Suspended, "guest breakpoint");
  } else {
    SetPending(Pending::Unimplemented,
               std::string{name} + " at " + std::to_string(pc));
  }
  jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

void JitExecutor::AddTicks(std::uint64_t ticks) {
  executed_ += static_cast<unsigned>(ticks);
  if (executed_ >= budget_) {
    // The budget is dynarmic's own stop condition rather than a RetroBug outcome, so `pending_` stays
    // None and MapHalt reads the reason off the HaltReason.
    jit_->HaltExecution(Dynarmic::HaltReason::Step);
  }
}

std::uint64_t JitExecutor::GetTicksRemaining() {
  return executed_ >= budget_ ? 0 : budget_ - executed_;
}

bool JitExecutor::PreCodeReadHook(bool is_thumb, GuestAddr pc, Dynarmic::A32::IREmitter& ir) {
  /**
   * The ARM64 back end cannot represent `Term::Interpret`, so the six declined encodings have to be
   * stopped before the translator reaches it — see `docs/30` §Step 20a for the SIGABRT this replaces.
   *
   * Three details are load-bearing, and the order among them is not interchangeable:
   *
   *   - `SetPending` before the halt, because it is first-write-wins and the halt provokes further
   *     callbacks (`ExceptionRaised` among them) that would otherwise get to name the reason.
   *   - `CheckHalt` *around* `ReturnToDispatch`, not the dispatch terminal alone: returning to dispatch
   *     on an unmodified pc re-translates the same instruction, which is precisely the hang Tier I had.
   *     The halt flag is what makes the emitted code leave instead of arriving back here.
   *   - A failed word read returns true rather than vetoing, so an unmapped or non-executable fetch keeps
   *     reporting itself through `MemoryReadCode`/`NoExecuteFault` instead of being relabelled here.
   *
   * The message keeps Tier I's wording and pc base so the differential can compare the two tiers'
   * accounts of the same instruction directly; the translator's own function name is added, because the
   * one thing a reader needs next is which of the six this was.
   */
  if (is_thumb) return true;  // all six are `arm_*` bodies
  std::uint32_t word = 0;
  if (!memory_.Read(pc, &word, 4)) return true;
  const DeclinedForm* match = nullptr;
  for (const auto& form : kDeclinedForms) {
    if ((word & form.mask) != form.value) continue;
    // Most ones in the mask wins, which is dynarmic's own ranking; ties keep table order, as its stable
    // sort does. Naming the wrong body would not change the refusal, but it would put a wrong function
    // name in a log someone is about to debug from.
    if (match == nullptr || form.ones > match->ones) match = &form;
  }
  if (match != nullptr) {
    SetPending(Pending::Unimplemented,
               std::string{"the A32 front end declined to translate the instruction at 0x"} +
                   Hex(pc) + " (banked-register or privileged load/store; " + match->body + ")");
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    ir.SetTerm(Dynarmic::IR::Term::CheckHalt{Dynarmic::IR::Term::ReturnToDispatch{}});
    return false;
  }
  return true;
}

void JitExecutor::InterpreterFallback(GuestAddr pc, std::size_t num_instructions) {
  /**
   * Dynarmic has no interpreter of its own, and on the host we ship on it has no hand-back either:
   * `Term::Interpret` reaches `UserCallbacks::InterpreterFallback` **only** in the x64 back end, while
   * the AArch64 one answers it with `ASSERT_FALSE` and terminates the process (`docs/30` §Step 20a). So
   * this function is dead code on ARM64 — kept because it is correct, because the same class of embedder
   * on an x64 host does reach it, and because `PreCodeReadHook` above is what makes it unnecessary here
   * rather than the reverse. It is *not* why Tier I exists: Tier I is the `execmem`-denied fallback and
   * the differential oracle, and stands on its own.
   *
   * Two hosts, one veto: `PreCodeReadHook` lives in `translate_arm.cpp`, which every back end shares, so
   * the same six lines also pre-empt the x86_64 emulator lane's version of this failure — there the
   * callback does fire, and it used to re-enter Tier I at the same pc with no progress. The arm64 host
   * aborts and the x64 host hangs; both are now a refusal that names the instruction.
   *
   * The contract is exact: "execute exactly num_instructions starting from PC". So each iteration
   * advances one real encoding's worth of PC — measured in bytes with the Thumb-32 test above, not by a
   * step budget — and any other stop reason (a host call, a fault, the sentinel) is propagated rather
   * than swallowed, because stopping inside the fallback has to reach the same place it would have
   * reached from compiled code.
   */
  if (live_ == nullptr) return;
  SyncOut(*live_);
  for (std::size_t i = 0; i < num_instructions; ++i) {
    const GuestAddr start = live_->pc;
    const GuestAddr target =
        start + EncodingSizeAt(memory_, start, live_->mode);
    while (live_->pc < target && pending_ == Pending::None) {
      const RunResult r = fallback_interp_->Run(*live_, 64);
      if (r.reason == StopReason::Returned || r.reason == StopReason::HostCall) {
        SetPending(r.reason == StopReason::Returned ? Pending::Returned : Pending::HostCall);
        pending_slot_ = r.host_slot;
        pending_pc_ = live_->pc;
        break;
      }
      if (r.reason == StopReason::Fault) {
        SetPending(Pending::Fault, r.detail);
        break;
      }
      if (r.reason == StopReason::Unimplemented) {
        SetPending(Pending::Unimplemented, r.detail);
        break;
      }
      if (r.instructions_executed == 0) break;  // no progress: never spin inside the JIT
    }
    executed_ += 1;
    if (executed_ >= budget_) break;
  }
  SyncIn(*live_);
  if (pending_ != Pending::None) {
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
}

void JitExecutor::Invalidate(GuestAddr from, GuestAddr to) {
  if (!ok() || to <= from) return;
  // Real work, unlike Tier I's no-op: this tier has compiled code that a guest write can invalidate, and
  // AD-7's self-modifying-code story depends on this call reaching the cache that holds it.
  jit_->InvalidateCacheRange(from, to - from);
}

}  // namespace rb::exec
