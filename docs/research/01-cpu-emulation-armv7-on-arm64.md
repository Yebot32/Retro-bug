# 01 — Executing ARMv7 (A32/T32) machine code on an AArch64 host

RetroBug research track: **CPU emulation strategy**. Scope: how to run legacy `armeabi-v7a` native code
(ARMv7 A32/T32 + VFP/NEON) inside a 64-bit-only process on modern ARM hardware, with Android
(`arm64-v8a`) as the first host and iOS/Apple Silicon as the second. This document evaluates every
viable strategy, its license fit against RetroBug's Apache-2.0 "no GPL linking" constraint, and its
correctness risk, then recommends an architecture.

> **Context reminder (from the brief).** Normal path is **in-process, no VM, no guest OS image**: we decode/JIT
> ARMv7 instructions and reproduce ARMv7 CPU+memory semantics, with an Android runtime/API layer above.
> Repo is **Apache-2.0**; treat **GPL-2.0 code (QEMU, Unicorn) as a linking boundary we cannot cross** in a
> shipped binary. Where a "GPL boundary" exists it is called out inline.

---

## TL;DR decision matrix

| Strategy | Perf (rel. native arm64) | Correctness risk | License fit vs Apache-2.0 | iOS-viable | Verdict |
|---|---|---|---|---|---|
| **A. Hardware AArch32-at-EL0** (kernel `CONFIG_COMPAT`, run A32 natively) | ~1.0× (native) | **Low** for the subset of silicon that has it | N/A (uses host HW) | **No** (all Apple Silicon is AArch64-only) | **Fair-weather path, not a foundation.** Present on the devices that don't need RetroBug; absent on 64-bit-only flagships (Cortex-A715/X4/A720/A520, all Apple). Use only as an opportunistic *probe*, never as the load-bearing mechanism. |
| **B. QEMU `linux-user` (arm guest / aarch64 host)** as the model | ~0.08–0.15× (TCG; "8–12× slower" class) | **Low-ish** (mature ARMv7 model incl. VFP/NEON/IT) | **GPL-2.0 → boundary.** Read/study OK; **cannot link.** | QEMU itself is GPL; TCG-on-Apple-Silicon possible in principle, GPL-blocked for us | **Best *reference model*, wrong *dependency*.** Study it hard; reimplement the same ideas permissively. |
| **C. QEMU full TCG / softmmu** (guest OS in a VM) | low, high overhead | low | GPL-2.0 → **boundary** | No (VM) | **Wrong shape** — RetroBug is explicitly *no VM, no guest OS image*. Discard. |
| **D. Unicorn Engine** (ARM backend) | ~0.1× (TCG) | **Medium** — ARM VFP/**NEON historically incomplete/off** | **GPL-2.0 → boundary** (QEMU fork) | No | **Not suitable as foundation.** GPL + missing NEON + per-call API granularity (no cross-call TB reuse) kill it. |
| **E. Dynarmic `arm_32` frontend** as a JIT core | ~0.2–0.6× (native-quality DBT) | **Medium** (models 3DS ARMv8, not the *Linux/Android* userland) | **0BSD** → **clean, linkable** | Yes (backend-agnostic; already JITs to AArch64 host) | **The single most reusable asset.** A real, permissively-licensed ARM32→AArch64 dynamic recompiler (Citra uses it). Mine its decode + regalloc ideas; verify ABI coverage. |
| **F. Original interpreter + selective DBT** (our own ARMv7 core) | interp ~0.02–0.1×; DBT hot paths ~0.3–0.7× | **High effort**, controllable w/ test suite | **Original → clean** | Yes | **Recommended core.** Interpreter first (correctness, bootstrapping), DBT for hot loops. |
| **G. Hybrid: HW-AArch32 fast path + (F) DBT fallback** (ARM64EC-spirit) | ~1.0× when HW present, else DBT | Medium (two code paths to validate) | Clean (F) + host feature | No on iOS | **Right posture *if* we ever want a speed tier on AArch32-capable Android.** Keep as optional accelerator behind a single semantic core, not as the primary model. |
| **H. Decoder-driven interpreter via **Capstone** / `llvm-mc` MCInstantiator** | interp (slow) | Medium | Capstone **BSD-3-clause** (clean); LLVM **Apache-2.0 + LLVM exception** (clean) | Yes | **Accelerators for (F):** Capstone to jump-start the *decoder*; MCInstantiator for the **emitter** side of the DBT. Not a CPU model by themselves. |

**Bottom line:** Build **(F) an original ARMv7 interpreter + selective DBT** as the load-bearing semantic
engine, informed by **(B) QEMU linux-user's ARM model** (study, do not link) and **(E) Dynarmic's `arm_32`**
(0BSD, linkable). Treat **(A/G) hardware AArch32** as an opportunistic accelerator behind the same core, never
the foundation, because it is absent on exactly the 64-bit-only devices (and all Apple silicon) RetroBug
exists to serve.

---

## 1. Hardware AArch32-at-EL0 path — is it exploitable, and is it fair-weather?

### 1.1 What the architecture guarantees (and no longer guarantees)

An AArch64 CPU *may* also implement the **AArch32 execution state at EL0**, which is what lets a 64-bit
kernel run 32-bit (`ELF EM_ARM`) user processes. Historically (Armv7→Armv8.0, ~2011–2022) this was
effectively always present; **it is now being removed, and Apple never shipped it on Apple Silicon.**

The single architectural bit that matters is **`ID_AA64PFR0_EL1.EL0` (bits [3:0])** — "support for the
AArch32 execution state at EL0" ([ARM ARM D24.2.93](https://developer.arm.com/documentation/ddi0487/maa/-Part-D-The-AArch64-System-Level-Architecture/-Chapter-D24-AArch64-System-Register-Descriptions/-D24-2-General-system-control-registers/-D24-2-93-ID-AA64PFR0-EL1--AArch64-Processor-Feature-Register-0)).
Two independent renderings of this field agree on the modern encoding (higher value = AArch32 present):

- Static register mirror: `0b0001` = "**EL0 can be executed only in AArch64 state**"; `0b0010` =
  "**EL0 can be executed in either AArch64 or AArch32 state**"; other values reserved
  ([df.lth.se ARM SysReg mirror](https://df.lth.se/~getz/ARM/SysReg/AArch64-id_aa64pfr0_el1.html)).
- The **Linux kernel's** machine-readable definition lists `0b0001 = IMP`, `0b0010 = AARCH32` and derives
  `id_aa64pfr0_32bit_el0()` from it
  ([arch/arm64/tools/sysreg via LXR](https://lxr.missinglinkelectronics.com/linux/arch/arm64/tools/sysreg),
  [patch "arm64: cpufeature: Check availability of AArch32"](https://lists.infradead.org/pipermail/linux-arm-kernel/2016-January/402830.html)).

> **Unverified / flag:** the exact numeric value for "AArch32 present" has shifted across spec revisions
> (legacy cores report `0b0000`, which older kernels treated as "supported"; the field was re-encoded when
> "AArch64-only EL0" was formalized). Treat the *semantics* as firm and the *exact bit pattern* as needing a
> re-read against the current DDI0487 before we hard-code it. See §Open questions.

**Which cores dropped it (CONFIRMED):**
- **Cortex-A715** (Armv9, announced June 2022) is the first Cortex-A core that is "**purely 64-bit, completely
  removing support for 32-bit code**" ([HWcooling architecture analysis](https://www.hwcooling.net/en/arm-introduces-new-cortex-a715-core-architecture-analysis/),
  [CNX-software](https://www.cnx-software.com/2022/06/29/arm-cortex-x3-cortex-a715-armv9-cores-improves-cortex-a510-efficiency/)).
- **Cortex-X4 / A720 / A520** (the 2023 **Armv9.2-A** lineup, e.g. Snapdragon 8 Gen 3, Dimensity 9300) are
  "**AArch64 only**" ([XDA](https://www.xda-developers.com/arm-cortex-x4-a720-a520-announced/);
  [Wikipedia Cortex-X4 = ARMv9.2-A](https://en.wikipedia.org/wiki/ARM_Cortex-X4);
  [Arm Cortex-A comparison table PDF](https://documentation-service.arm.com/static/6480a98316f0f201aa6b949d)).
- **All Apple Silicon (M-series / A-series on modern iOS)**: **no AArch32 EL0** ([741g/android-emulator-m1-preview#2](https://github.com/741g/android-emulator-m1-preview/issues/2),
  [HN 27277351](https://news.ycombinator.com/item?id=27277351)). This alone **disqualifies HW AArch32 as the
  foundation** because iOS/Apple Silicon is a first-class RetroBug target.

> **Version-where-optional:** secondary sources disagree on whether AArch32-at-EL0 became *architecturally
> optional* in **Armv8.6-A** ([Wikipedia ARM family](https://en.wikipedia.org/wiki/ARM_architecture_family))
> or **Armv8.8-A / Armv9.3-A**. The **empirically safe statement for RetroBug** is: *the first shipping
> consumer silicon without it is Cortex-A715 (2022), and every flagship Cortex since (X4/A720/A520) lacks it.*
> Pin the exact "optional from" clause from DDI0487 before citing it externally.

### 1.2 Detecting support from Android userspace, without root

There is **no single HWCAP that means "AArch32 EL0 available to *you*"**; that decision belongs to the kernel,
and HWCAPs are *produced by* a running binary's own personality. Practical options:

- **`getauxval(AT_HWCAP)` / `AT_HWCAP2`** and **`/proc/cpuinfo`** describe features *for the calling process's
  ISA* (AArch64 flags like AES/CRC/SVE on the 64-bit side)
  ([ARM runtime-detection blog](https://developer.arm.com/community/arm-community-blogs/b/operating-systems-blog/posts/runtime-detection-of-cpu-features-on-an-armv8-a-cpu),
  [getauxval(3)](https://man7.org/linux/man-pages/man3/getauxval.3.html),
  [kernel arm64 elf_hwcaps](https://www.kernel.org/doc/html/v6.5/arch/arm64/elf_hwcaps.html)). They do **not**
  directly tell a 64-bit process "this CPU can run 32-bit."
- **`MRS` of `ID_AA64PFR0_EL1` from EL0** is **trapped/emulated by the kernel** which returns a *sanitized*
  value; not all ID regs are safely readable and the useful "can I run A32" answer is masked
  ([kernel cpu-feature-registers](https://www.kernel.org/doc/html/v6.5/arch/arm64/cpu-feature-registers.html)).
- **Robust, root-free detection = functional probe**: `fork()+execve()` a tiny **static ARM (32-bit) ELF** we
  ship in the APK's `assets/` and see whether it starts. Success ⇒ kernel `CONFIG_COMPAT` + AArch32-EL0 both
  present. Failure (`ENOEXEC`) ⇒ not available. This sidesteps register/HWCAP ambiguity entirely.
- **Android system property** `ro.product.cpu.abilist32` reports what the *vendor build* advertises
  ([NDK ABIs](https://developer.android.com/ndk/guides/abis)) — a fast pre-filter, but it can be empty even on
  AArch32-capable silicon when the OEM disabled 32-bit (see 1.3).

### 1.3 Can an arm64 Android kernel even `exec` a 32-bit ELF today?

Only if built with **`CONFIG_COMPAT`** (arm64's 32-bit-EL0 support symbol; there is **no `CONFIG_ARM64_32BIT_VDSO`**,
and `CONFIG_ARM64_ILP32` is a *separate* "32-bit pointers in AArch64" ABI, not classic ARMv7 EABI)
([CONFIG_COMPAT](https://cateee.net/lkddb/web-lkddb/COMPAT.html);
[SO: 32-bit ARM on aarch64 despite CONFIG_COMPAT](https://stackoverflow.com/questions/59379848/running-32-bit-arm-binary-on-aarch64-not-working-despite-config-compat)).
Even with `CONFIG_COMPAT`, a full 32-bit userland needs the **32-bit vDSO** and the **kuser-helpers page** or
syscalls like `gettimeofday` fall back to traps (see §7); omitting the compat vDSO causes a documented perf
collapse ([sigma-star pitfall](https://sigma-star.at/blog/2024/02/aarch64-32-bit-compat-pitfall/)).

**Android reality:** Google Play already requires 64-bit libs, and **"64-bit-only" devices are shipping now** —
**Pixel 7/7 Pro launched 64-bit-only**; Android 12+ emulator images are 64-bit-only
([64-bit requirements](https://developer.android.com/google/play/requirements/64-bit),
[Pixel 7 32-bit disabled](https://www.reddit.com/r/Android/comments/y353es/)). Crucially, a device can be
**64-bit-only *by policy* even when its cores support AArch32**: e.g. Tensor/Snapdragon parts whose CPU IP is
Armv8/v9-AArch32-capable but whose *GKI vendor build ships `CONFIG_COMPAT=n` / empty `abilist32`*. Conversely a
device with A715-class cores is AArch32-incapable *in silicon* regardless of config.

### 1.4 Verdict + rough share

**Can we exploit it? Yes, narrowly, as an accelerator — no, as a foundation.** Reasons:
1. **Target mismatch (the "fair-weather" problem):** HW AArch32 is available on older/mid 64-bit Android that
   already runs `armeabi-v7a` apps natively — precisely the users who **don't need RetroBug**. It is **absent on
   64-bit-only flagships and on all Apple Silicon** — the users who **do** need it. A path that only works when
   you don't need it is not a load-bearing strategy.
2. **In-process model conflict:** RetroBug's normal path is "in-process, no guest OS image." Delegating to the
   kernel's AArch32-EL0 means a **separate 32-bit process** with its own bionic/linker — the opposite of the
   embedding model, and it can't be mixed per-library into our 64-bit process.
3. **Second target is disqualified** (Apple = zero AArch32).

**Rough share (directional, not a hard stat):** Among *active arm64 Android*, a **majority of 2019–2023 silicon
still implements AArch32 at the CPU-IP level**, but the **share of devices where a 64-bit process can actually
use it** is much smaller and **falling fast** (OEMs disabling `CONFIG_COMPAT`; new SoCs dropping it in silicon).
For RetroBug's **addressable set** (64-bit-only devices + iOS), the usable share of the HW fast path trends to
**near zero over the product's life**. Build for its absence; treat presence as a bonus.

---

## 2. QEMU TCG: ARM guest on AArch64 host — mechanics to learn from

QEMU's **TCG (Tiny Code Generator)** is the canonical design RetroBug should *study* (GPL boundary → not link):

- **Translation blocks (TBs).** Guest code is decoded into TBs at branch boundaries; each TB records the CPU
  state it consumes/produces and is compiled to host code. Direct branches are **chained** so "TBs can be called
  successively after execution, without getting back to the QEMU code and looking for the next TB" (the
  `goto_tb`/`exit_tb` epilogue trick) — this is the single most important throughput idea
  ([QEMU Translator Internals](https://www.qemu.org/docs/master/devel/tcg.html),
  [Airbus SecLab TCG deep-dive](https://airbus-seclab.github.io/qemu_blog/tcg_p1.html)).
- **Two-level IR.** Guest → **TCG ops (IR)** → host. `target/arm/tcg/translate*.c` emits TCG ops; `tcg/aarch64/`
  turns them into A64. Decoupling guest-decode from host-emit is why TCG is portable; RetroBug gets the same
  benefit (guest ARMv7 front-end reusable across host arm64 *and* x86 later)
  ([TCG IR docs](https://www.qemu.org/docs/master/devel/tcg-ops.html)).
- **CPU state lives in memory (`cpu_*` env), not pinned to host regs,** during IR; the back-end regallocs into
  host registers per-TB. Access via `cpu_ldst`/`cpu_ld*_mmu` helpers, which also carry the softmmu/user-mode
  address-space semantics ([Airbus deep-dive](https://airbus-seclab.github.io/qemu_blog/tcg_p1.html)).
- **ARM feature coverage (A-profile).** TCG models **Armv5/6/7/8/9 A-profile**, incl. **VFP and NEON
  (`vfp_translate`)**, and treats **M-profile (Cortex-M) as a separate front-end** — so ARMv7-A A32/T32 + VFP/NEON
  *is* fully modeled in QEMU ([QEMU A-profile emulation docs](https://www.qemu.org/docs/master/system/arm/emulation.html)).
  This matters for us: **NEON is not optional for a real Android `.so`** (bionic, libGLES, codecs use it), so any
  foundation must cover A32 **and** T32 **and** VFP **and** NEON — a bar QEMU clears, Unicorn does not (§4).
- **Thumb-2 decode complexity.** T32 is 16/32-bit mixed with the **IT block** (conditional-execution state carried
  across up to 4 following instructions). QEMU carries per-TB `condexec`/IT state and threads it through each
  translated instruction; this is the classic correctness hotspot (see §7).
- **32-bit guest on 64-bit host = the easy direction.** QEMU's hard limitation is the *reverse*: it **dropped
  support for 64-bit guests on 32-bit hosts** ("TCG oversized guest", 2016 RFC). RetroBug is a **32-bit guest on a
  64-bit host**, which is fully supported and requires no oversized-guest machinery
  ([qemu-devel RFC drop 64-bit-guest-on-32-bit-host](https://lists.endsoftwarepatents.org/archive/html/qemu-devel/2016-08/msg01965.html)).

**Memory ordering — ARMv7 (weak) → AArch64 (also weak).** The mapping is *favorable*: both are weakly ordered,
so unlike emulating x86-TSO, an AArch64 host does **not** pay a fence-emulation tax to preserve ARMv7 order.
QEMU's strategy:
- Guest barrier ops (`DMB/DSB/ISB`) → host barriers; TCG inserts barriers via `tcg_gen_mb()` **only in a parallel
  (MTTCG) context** and elides them for uniprocessor/single-thread correctness runs
  ([Multi-threaded TCG docs](https://www.qemu.org/docs/master/devel/multi-thread-tcg.html)).
- Guest `LDREX/STREX` (exclusive monitors) → **LL/SC on the host**: an AArch64 host has native `LDXR/STXR` so
  `TCG_TARGET_HAS_*` maps `tcg_gen_atomic_*`/`qemu_ld_excl`/`qemu_st_excl` onto them. When the host can't express
  a guest atomic, TCG takes **`EXCP_ATOMIC`: the instruction is emulated under a global serializing lock**
  ("ensures all emulation is serialised") — a correctness fallback, not a perf path
  ([MTTCG docs](https://www.qemu.org/docs/master/devel/multi-thread-tcg.html)). The ARM exclusive-monitor
  semantics (granule, size, the `STREX`-must-fail-on-context-switch rule) are the subtle part; see
  [Raymond Chen on LDREX/STREX + barriers](https://devblogs.microsoft.com/oldnewthing/20210614-00/?p=105307).

**Performance reality check.** QEMU's own analysis: full-system **TCG is "~8× slower" for x86-64 and "~12×
slower" for aarch64** vs native on a capable host (with host-side fix items like `pauth-impdef`, `CMPXCHG128`)
([Linaro: QEMU a tale of performance](https://www.linaro.org/blog/qemu-a-tale-of-performance-analysis/)). A 2025
cross-arch study found **aarch64 TCG throughput notably above e.g. i386** in a job-based benchmark ("aarch64
completes 144 jobs vs i386-pentium2 107 — 34% more") ([fluxcoil emulation perf study](https://blog.fluxcoil.net/posts/2025/02/emulation-performance-and-consumption/)).
Take-away: **TCG-class emulation ≈ 0.08–0.15× native**, and a *purpose-built* DBT that leans on the shared
weakly-ordered memory model (no fence tax) can beat generic TCG — but you inherit all the correctness surface.

> **GPL boundary:** everything above (TCG, `target/arm/tcg`, `linux-user`) is **GPL-2.0**. It is a *design
> reference*, not a dependency. Do not `#include`, copy, or link QEMU sources into the Apache-2.0 RetroBug
> binary.

---

## 3. QEMU `linux-user` as the model for RetroBug's normal path

`linux-user` is the closest existing system to RetroBug's "no kernel, no VM" normal path — **but only for the CPU
layer**, not the app layer. What it does:
- **Does not emulate a kernel.** It runs a foreign-ISA *binary* natively on the host kernel and **emulates Linux
  syscalls in user space** inside a per-CPU `cpu_loop` — it "translates syscalls," handling "endianness, 32/64 bit
  parameter size," and documents caveats (limited `clone` flags; cross-ISA memory-model/atomic differences)
  ([QEMU User-space emulator docs](https://www.qemu.org/docs/master/user/main.html)).
- **ARM EABI syscall entry.** For ARM, `cpu_loop` handles `EXCP_SWI`: it pulls the syscall number from **`r7`**
  (EABI/Thumb) and dispatches `do_syscall`; ARM-only "magic" calls above `ARM_NR_BASE` (e.g. `ARM_NR_breakpoint`,
  `ARM_NR_cacheflush`) are special-cased, unknown ones return `-ENOSYS`
  ([linux-user/arm/cpu_loop.c](https://raw.githubusercontent.com/qemu/qemu/master/linux-user/arm/cpu_loop.c)).
- **Signals / sigcontext.** Host signals are marshalled into a *guest* `ucontext`/`sigcontext` so guest
  signal handlers see an ARMv7 register frame; this is exactly where a 32-bit-on-64-bit system must fabricate a
  fake A32 `sigcontext` (there is no host A32 ABI to reuse).
- **It still sets up the kuser page** at `0xffff0000` even in user mode — a hint that ARMv7 code depends on it
  ([qemu#447 "Unable to reserve 0xffff0000"](https://gitlab.com/qemu-project/qemu/-/issues/447)); see §7.

**Known limits for a full Android app** (why `linux-user` alone is not enough for RetroBug):
- It launches **one ELF with one personality** and relies on the **host's** dynamic linker/`libc`. Android needs
  **its own 32-bit `bionic` + `linker` (`/system/bin/linker`)** loaded *as guest data*, dlopen of app `.so`s,
  `libc.so`/`libandroid_runtime` interposition, JNI, `AT_SECURE`, auxv for ARM — none of which `linux-user`
  provides.
- Android runtime syscalls/services — **`binder`/`ashmem`/`ion` ioctls, SELinux, ` property_*`, `logcat`,
  `membarrier`, `futex` group semantics, threads via `clone`+TLS**, and the whole `Zygote`/ART interaction — are
  outside `linux-user`'s scope. RetroBug's **Android/API layer above the CPU** is where this is solved (see
  recommended architecture §8); `linux-user` shows the *syscall-marshal + fake-signal-frame* pattern to copy,
  not the app model.
> **GPL boundary:** `linux-user/*` is **GPL-2.0**. Model the `cpu_loop`/`sigcontext`-fabrication design; write our
> own syscall table & signal frame in original code.

---

## 4. Unicorn Engine — is it a suitable foundation? **No.**

Unicorn is a QEMU-derived CPU-emulation toolkit aimed at *running snippets*, not whole OS userlands:
- **License = GPL-2.0** (QEMU fork) → **GPL boundary**, cannot be linked into an Apache-2.0 app. (QEMU is
  GPL-2.0; Unicorn inherits it.) This alone disqualifies it as a dependency for RetroBug.
- **ARM VFP/NEON is incomplete / off by default.** Long-standing issues: *"ARM NEON/VFP support seems to exist but
  is disabled by default"* ([unicorn#571](https://github.com/unicorn-engine/unicorn/issues/571)) and *"arm vfp
  support seems not well"* ([unicorn#1080](https://github.com/unicorn-engine/unicorn/issues/1080)). For real
  Android `.so`s (which use NEON heavily), a missing/optional FP/SIMD front-end is a **correctness dealbreaker**.
- **API granularity / perf.** Unicorn's public API is per-`uc_emu_start(begin, end, …)`: it stops on branches
  out of range / on every instruction-count hook, so it does **not** give you QEMU-style cross-call TB chaining
  "for free" from an app-embedding perspective — you'd be re-entering the engine at syscall/`svc` boundaries and
  paying helper-call overhead. It is designed for basic-block/shellcode analysis, not long-running process
  emulation. (There is **no `linux-user`** in Unicorn — you hand-build ELF loading, syscalls, signals, and
  memory yourself, i.e. you'd reimplement §3 on top of a GPL core with missing NEON.)
- **Maintenance:** active but slow; the ARM back-end lags mainline QEMU's ARM coverage.
**Verdict:** unsuitable as foundation (GPL + NEON gap + wrong API shape). Useful only as a *read-only* reference.

---

## 5. Other ARM→ARM64 / 32-on-64 DBTs worth learning from

### 5.1 **Dynarmic (0BSD)** — the most important finding for RetroBug
Dynarmic is a **dynamic recompiler (DBT)** with multiple guest/front-end backends. Its own README confirms:
**hosts = x86-64 and AArch64**, and it recompiles **"ARM v3 through v8 (32/64-bit)"**, under a
**"0BSD license"** ([lioncash/dynarmic](https://github.com/lioncash/dynarmic); license text
[azahar-emu/dynarmic LICENSE.txt](https://github.com/azahar-emu/dynarmic/blob/master/LICENSE.txt) = 0BSD:
"Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby
granted"). Concretely:
- **Citra uses Dynarmic's ARM32 front-end (`arm_32`) to JIT the 3DS's ARMv8 CPU** to the host — i.e. a real,
  shipped, **permissively-licensed ARM32 → (x64 **and** AArch64)** dynamic recompiler exists, including VFP
  (the 3DS uses VFP/NEON) ([Citra CPU wiki](https://citra.azahar-emu.org/wiki/cpu/) — "dynamic recompiler …
  capable of recompiling the … instruction set"; Dynarmic README for host list).
- **Why this matters:** RetroBug's "is there *any* permissively-licensed ARM32 JIT we could build on?" — **yes:
  Dynarmic (0BSD is compatible with Apache-2.0 and permits linking/static incorporation).** Two caveats to verify:
  (a) Dynarmic models a *bare CPU/SoC* (3DS or Switch), **not** the *Linux/Android user ABI* — no bionic, no
  kuser page, no `svc`→Linux-syscall table; we'd supply §3/§7 around it. (b) Dynarmic's `arm_32` front-end is
  tuned to the 3DS's ARMv8 subset (VFPv3, NEON present) — confirm ARMv7-A VFPv3-D16/`d0–d31`/FPSCR and Thumb-2
  IT-block coverage against our corpus before betting on it.
- **`arm_32` vs `arm_64`:** the `arm_64` front-end (used by yuzu/Switch) is **not** what we need; `arm_32` is the
  ARMv7/Thumb-2 relevant one.

### 5.2 Microsoft **ARM64EC / ARM64X** — the hybrid architecture idea (important to RetroBug)
Microsoft's approach for Windows-on-ARM is a **native + emulated hybrid in one process**:
- **ARM64EC ("Emulation Compatible")** is an **ABI** for ARM64 code that **interoperates with emulated x64 in the
  same process**: "the Arm64EC code runs with native performance, while any x64 code runs by using emulation …
  built-in," letting you **incrementally** move hot modules from emulated to native, and OS syscalls are
  Arm64EC so the *transitioning* code is native even while the app thinks it's x64
  ([Arm64EC — Microsoft Learn](https://learn.microsoft.com/en-us/windows/arm/arm64ec)).
- **ARM64X** is the **dual-code PE** (`machine = AA64 (ARM64) (ARM64X)`) carrying both native and emulated-target
  code chunks so one binary works under either
  ([FFRI ProjectChameleon ARM64X reloc](https://ffri.github.io/ProjectChameleon/new_reloc_chpev2/)).
- **The architectural lesson for RetroBug** (not the exact mechanism, since MS's *emulated* arch is x64 not
  ARMv7): build a **seam** where a call can resolve to either **native arm64** or **our ARMv7 emulator**, and make
  the emulator **in-process, crossable, and incrementally replaceable** (thunk ABI at the boundary, per-module
  "is this emulated?" bit, and the ability to hand a native arm64 fast path where a library ships arm64). This is
  the shape of §8's recommended architecture. (Microsoft's "mix native AArch32 + emulated fallback for
  ARMv7-only instructions" is best treated as an *analogue*; I did not find primary evidence Microsoft emulates
  individual ARMv7-only *instructions* on AArch64 — WoA ran 32-bit ARM **natively** via AArch32-EL0 and emulated
  **x86** via Prism. See Open questions.)

### 5.3 Apple's AArch32 story on Apple Silicon — **none**
All Apple Silicon (M1 Firestorm/Icestorm onward) is **AArch64-only at EL0**; there is **no hardware AArch32**, so
iOS/macOS can't run 32-bit ARM even in principle
([741g m1 issue#2](https://github.com/741g/android-emulator-m1-preview/issues/2),
[HN 27277351](https://news.ycombinator.com/item?id=27277351)). **Consequence:** the second RetroBug target has
**zero** hardware AArch32 — pure emulation is the *only* option there, which is the strongest argument for making
the DBT/interpreter (not the HW path) the load-bearing core.

### 5.4 Android-specific 32-on-64 translation: **Native Bridge** (the mechanism to plug into)
Android ships a first-class hook for exactly RetroBug's problem: **`libnativebridge`** ("Native Bridge") lets the
runtime **delegate loading and executing of a 'translated architecture'** (a different ISA/ABI) to a pluggable
implementation — it "enables apps with native components to run on systems with different ISA or ABI"
([platform/art/libnativebridge](https://android.googlesource.com/platform/art/+/refs/heads/main/libnativebridge/)).
Real-world instances are **Intel Houdini** (x86→ARM; `libhoudini`/`libndk_translation`) and Android-x86/redroid's
`libnb` (**arm native bridge**) ([redroid#175](https://github.com/remote-android/redroid-doc/issues/175),
[androidx86 native-bridge discussions](https://groups.google.com/g/android-x86/c/SEMbCDXzTz4)).
**Design takeaway:** RetroBug's CPU layer should **present itself as an Android *native-bridge* provider for
`armeabi-v7a`** so Zygote/ART/app-process plumbing (`ro.dalvik.vm.native.bridge`,
`ro.product.cpu.abi`, the bridge's `NativeBridgeItf` callbacks) loads our engine **without patching the runtime**.
(Houdini is proprietary → reference for the *integration interface*, not the code.) **ExaGear ARM** (ARM binary
translation, e.g. ARM↔x86 for Android/Linux) is prior art in the same space but commercial/encumbered; note it,
don't depend on it.

---

## 6. Reusable vs original engineering (decoder, JIT, licensing)

**What can be permissibly reused:**
- **Instruction decoder:**
  - **Capstone** — multi-arch disassembler incl. **ARM A32/T32** with detailed operands, under a
    **BSD-style license** ([capstone-engine.org](https://www.capstone-engine.org/); upstream
    [capstone repo](https://github.com/aquynh/capstone) = BSD-3-Clause). **Verdict: usable as a *decode
    oracle* / development accelerator and (with care) as a fallback decoder** — but it is a *disassembler*: it
    gives mnemonic+operands, **not** the semantic effect; you still write the exec/flag/IT-block semantics. Relying
    on it on the hot path also couples you to its JIT-hostile allocation/API shape. Best used to **generate and
    cross-check our own decoder tables**, and for a slow-path safety net.
  - **Machine-readable ARM ISA → generated decoder.** Arm publishes the architecture in **XML + ASL (Architecture
    Specification Language)** and states the semantics are "written in ARM's ASL … **all executable**," enabling
    transpiling to C for **simulators, decoders, emulators**
    ([Arm ASL](https://developer.arm.com/architectures/architecture%20specification%20language),
    [Arm MRA release note](https://alastairreid.github.io/ARM-v8a-xml-release/),
    [UQ-PAC/aslp partial evaluator](https://github.com/UQ-PAC/aslp),
    [ASL reference DDI0626](https://www.scs.stanford.edu/~zyedidia/docs/arm/asl.pdf)). **Verdict: strategically the
    right long-term move** — auto-generate the decode tables (and ideally a reference executable-semantics oracle
    for fuzzing) from Arm's machine-readable spec. **Caveat:** the ASL/MRA **data** carries an Arm license that
    must be checked for *redistribution/commercial* use (research-friendly ≠ product-friendly); the *generator +
    generated tables* we author are original. This gives a **differential-testing oracle**: ASL-interpreter output
    vs our JIT.
- **Code emitter for the DBT:**
  - **`llvm-mc` / `MCInstantiator`** (LLVM, **Apache-2.0 + LLVM exception**) lets you **assemble machine code at
    runtime** from MCInst objects (`llvm::MCInstantiator`, added ~LLVM 19 for JIT-time instantiation); we can emit
    AArch64 host code by building `MCInst`s instead of hand-rolling `tcg_out_*` byte patching
    ([llvm-mc](https://llvm.org/docs/CommandGuide/llvm-mc.html),
    [MaskRay: integrated assembler / MCInstantiator](https://maskray.me/blog/integrated-assembler-improvements-in-llvm-19),
    [Embecosm how-to](https://www.embecosm.com/appnotes/ean10/ean10-howto-llvmas-1.0.html)). **License-clean** for
    us (Apache-2.0). **Caveat:** linking LLVM is heavy for a mobile `.so`; consider a *thin* custom A64 emitter
    (or MCInstantiator subset) instead of all of LLVM.
- **A whole ARM32 JIT we can build on:** **Dynarmic (`arm_32`), 0BSD** — the *only* permissive, shipping ARM32 DBT
  found (see §5.1). This is the answer to "is there *any* permissively-licensed ARM32 JIT" → **yes, one.**

**What must be original (or at least non-GPL) engineering:**
- The **guest *semantic* model**: A32/T32 executors with **CPSR/APSR** + **Q-flag/saturation**, **VFP/NEON** with
  **FPSCR** + denormal/rounding, **IT-block** conditional execution (§7), **CP15 `mrc`/`mcr`** traps, and the
  **LDREX/STREX exclusive-monitor** semantics. QEMU/Unicorn model all of this but are GPL. **Dynarmic models the
  *instruction* semantics but not the *OS/ABI* surface** (kuser page, auxv, signal frames, syscall table).
- The **Android integration layer**: 32-bit bionic/linker loading, `svc`→syscall table for ARM Linux,
  fake A32 `sigcontext`, **native-bridge provider** shim (§5.4).
- **Reimplementation** of the TB design/chaining and barrier/atomic mapping (§2) in original code — the *ideas*
  are not copyrightable, the *source* is GPL, so build our own.

**License summary of candidates named:**
| Component | Role | License | Usable under Apache-2.0? |
|---|---|---|---|
| **QEMU** (`target/arm`, `linux-user`) | reference / (would-be) engine | GPL-2.0 | **No link.** Study only |
| **Unicorn** | snippet CPU emu | GPL-2.0 | **No link**; also no NEON |
| **Dynarmic (`arm_32`)** | ARM32→A64 DBT core | **0BSD** | **Yes** |
| **Capstone** | decode helper | **BSD-3-Clause** | **Yes** (as decoder/helper) |
| **LLVM `llvm-mc`/MCInstantiator** | host-code emitter | **Apache-2.0 + LLVM exception** | **Yes** |
| **Arm MRA / ASL data** | generate decoder + oracle | **Arm proprietary license (research)** | **Check redistribution** before shipping generated tables |
| **Intel Houdini / ExaGear ARM** | prior art | proprietary | **No** (interface inspiration only) |

---

## 7. A32/T32 ABI + state surface we MUST model (correctness hot-spots)

These are the things that make an ARMv7 userland *work*; each is a concrete implementation requirement with
evidence:

1. **CPSR/APSR full flag semantics + Thumb bit (T).** Condition codes **N,Z,C,V**, the **Q** (saturation) bit,
   **GE** bits (SIMD), and **CPSR.T** (Thumb state) which is *architecturally visible* in the branch LSB and in
   `BX`. `MOVS`/`S`-suffix instructions restore flags; `CPSR` save/restore via `MRS/MSR`. Model all of it — this is
   the difference between "runs" and "runs correctly."
2. **IT-block state (`ITSTATE`) — the classic hard part.** T32's `IT`/`ITx` prefix encodes up to **4 following
   instructions' condition codes** in state carried *between* instructions (a `condexec`/`condmask` pair). Inside a
   DBT you must (a) keep IT state per-TB, (b) apply per-instruction predicates to *conditional* memory ops, and
   (c) **split TBs correctly on IT boundaries / on ops that architecturally force an IT break** (e.g. `BMI`,
   `CPS`, `SVC`, and loads/stores that can't be conditional). Getting IT-block TB-splitting wrong causes
   mis-predication bugs that are extremely hard to find. (QEMU threads this as `condbit/condmask` per translated
   insn; §2.)
3. **Banked registers / privileged modes — and does userspace need them?** ARM has banked `SPSR`/`LR_svc`, etc.
   for SVC/IRQ/ABT/UND modes. **Userspace (AArch32 EL0) normally only sees the EL0 (USR) bank + `svc`.** Evidence
   that `svc` *is* relied on: **ARM Linux syscalls are `svc 0`** from bionic (QEMU `EXCP_SWI`/`r7` path, §3), and
   the legacy **kuser helpers** were reached via `svc`/`ldr` (§6 below). So we must model the **`SVC`→syscall
   exception** faithfully, and the **abort mode** at least enough to deliver `SIGSEGV`/`SIGBUS` with a correct
   guest `sigcontext`. We do **not** need full IRQ/FIQ delivery (no guest kernel).
4. **VFP: `d0–d31`/`q0–q15` + `FPSCR`.** Full 32-double / 16-quad register set, **`FPSCR`** (NZCV + `QC`,
   enable/rounding, exception traps), **ASID** (VFP vs AArch64 FP), lazy-enable semantics, and the `MRC/MCR` and
   `VSTM/VLDM` encodings. 3DS-era Dynarmic has VFP; **Unicorn's VFP/NEON is the documented weak point (§4)**, so
   don't inherit a half-VFP engine.
5. **NEON (Advanced SIMD).** Non-negotiable for real Android `.so`s (bionic string/mem, codecs, graphics). TCG/`vfp_translate`
   covers it (§2); Dynarmic `arm_32` covers NEON for the 3DS — **verify coverage vs the ARMv7-A NEON subset** bionic uses.
6. **CP15 `mrc`/`mcr` reads — and the kuser/`0xffff0000` helper page (the real gotcha).**
   - Userspace reads CP15 for `MIDR`/cache-type (`CTR`, `CCSIDR`, …). **bionic** uses CPU-id/hints (e.g. `strlen`/`memcpy`
     tuning) and `__get_tls()`; historically TLS on ARM used **`__kuser_get_tls` at `0xffff000e`** on older
     kernels, modernized to the **TPIDRURO (`MRC p15,0,Rt,c13,c0,3`)** coprocessor read
     ([SO: TLS sequences on ARM](https://stackoverflow.com/questions/29818269/code-sequences-for-tls-on-arm),
     [bionic elf-tls](https://github.com/aosp-mirror/platform_bionic/blob/main/docs/elf-tls.md)). So we must
     implement the **TPIDRURO/TPIDRURW** CP15 registers and any `MIDR`/cache reads apps issue (return sane values).
   - **The `0xffff0000` vector/kuser page (`CONFIG_KUSER_HELPERS`) provides `__kuser_cmpxchg`,
     `__kuser_cmpxchg64`, `__kuser_get_tls`, `__kuser_memory_barrier`, and the signal trampolines** at fixed
     offsets, and **ARMv6/v7 userspace can still depend on it**
     ([kernel_user_helpers docs](https://docs.kernel.org/arch/arm/kernel_user_helpers.html),
     [CONFIG_KUSER_HELPERS](https://cateee.net/lkddb/web-lkddb/KUSER_HELPERS.html),
     [arm64 compat adds CONFIG_KUSER_HELPERS](https://lists.infradead.org/pipermail/linux-arm-kernel/2017-September/530934.html)).
     **This is a genuine 32-on-64 trap:** the fixed address `0xffff0000` sits in the *low* range that a 64-bit host
     normally reserves for the kernel / can't map for a 64-bit process — exactly the collision that bites a 32-bit
     guest on a 64-bit host, and QEMU explicitly hit it ([qemu#447 "Unable to reserve 0xffff0000 bytes"](https://gitlab.com/qemu-project/qemu/-/issues/447)).
     **RetroBug MUST synthesize this page** (map a guest page and place our own A32 helper stubs at the documented
     offsets, and/or trap accesses to it) — a naive 64-bit host has no AArch32 kernel to provide it.
7. **Exclusive monitors / `LDREX`/`STREX` (and the byte/halfword variants).** ARMv7's `LDREXB/STREXB`,
   `LDREXD/STREXD`, `LDREXH/STREXH` (post-v6/v7) plus the **exclusive-monitor granularity/address-match rules**
   and "STREX must fail if the monitor was cleared by any store since the LDREX" contract; and these ops can appear
   **inside an IT block** (predicate + side-effect interaction)
   ([Raymond Chen Thumb-2 atomics/barriers](https://devblogs.microsoft.com/oldnewthing/20210614-00/?p=105307)).
   On an AArch64 host map to `LDXR/STXR` (native LL/SC) — the *same weakly-ordered model* — but the
   **IT-predication + monitor-context** edge cases are where emulators are quietly wrong; differential-test them.
8. **`CLREX`, `DMB/DSB/ISB`, `yield`, `wfe/wfi`** — encode as host barriers / no-ops per §2's mapping; `ISB` also
   forces a **TB boundary** (self-modifying-code/prefetch semantics) which your TB end/flush logic must honor.
9. **`ATOMIC`/unaligned and sub-page protection.** ARMv7 permits some unaligned accesses; userspace relies on
   `mprotect`/`mincore` page-granular behavior. On a 64-bit host with 4K vs 16K pages (Android 15+ 16 KB pages!),
   guest 4-K page semantics (guard pages, `mmap` granularity, `MRS CTR` line size) must be presented consistently.

> The **convergence of §7.2 (IT-block), §7.6 (kuser page + CP15 TLS), and §7.7 (LDREX/STREX + monitor)** are, in my
> judgment, the highest-risk correctness areas for a 32-bit-on-64-bit ARMv7 engine, because each is *both* subtle
> *and* load-bearing for bionic to run at all.

---

## 8. Recommended architecture for RetroBug

**Core posture: original ARMv7 *interpreter* first, *selective DBT* second, hardware AArch32 as an optional
accelerator behind a single semantic model — integrated as an Android *native-bridge* for `armeabi-v7a`.**

Layered plan (top = the API layer already specified elsewhere):

1. **Single semantic source of truth.** One guest state object (r0–r15, CPSR/APSR incl. `T`/`Q`/GE, `d0–d31`/`q0–q15`,
   `FPSCR`, TPIDR*, IT state, exclusive-monitor tag). Both the interpreter and the DBT read/write *this same state*
   so they can't diverge.
2. **Tier 0 — correctness-critical interpreter (ship first).** A32+T32 decode → direct exec over the guest state.
   Use it to boot bionic + linker, run JNI, and as the **reference model / fallback** the JIT never trusts blindly.
   Get VFP/NEON, IT-block, `SVC`, kuser page, CP15, `LDREX/STREX` (§7) exactly right here first.
3. **Tier 1 — selective DBT for hot code.** Trace/inline-cache hot loops into TBs (chain via the §2 epilogue trick).
   **32-bit guest on 64-bit host is the easy direction (§2), and the shared weak memory model means minimal barrier
   tax.** Map `LDREX/STREX`→`LDXR/STXR`; fall back to a serializing lock only for constructs you can't lower.
   Candidate building blocks, license-clean:
   - **Front-end (decode+semantics):** **fork/extend Dynarmic `arm_32` (0BSD)** for the instruction executors and,
     if it lowers to AArch64 host, its `backend_abi`/regalloc — *after* verifying ARMv7-A VFPv3-D32/NEON + IT-block
     coverage (§5.1, §6). If coverage is insufficient, use it as scaffolding and replace the ARMv7-A specifics.
   - **Emitter:** thin custom A64 emitter, optionally backed by **`llvm-mc`/MCInstantiator (Apache-2.0)** (§6).
   - **Decoder tables:** auto-generate from **Arm ASL/MRA** and cross-check with **Capstone (BSD)** (§6).
4. **Tier 2 (optional) — hardware AArch32 accelerator.** On Android devices where our §1.2 functional probe
   (`execve` a 32-bit static ELF) succeeds *and* the workload is a whole 32-bit sub-process (not an
   in-process library call), you *may* bounce to native AArch32. Gate it behind the same semantics + a capability
   bit. **Never the foundation** (§1.4) and **unavailable on iOS** (§5.3). This is the ARM64EC-style "native
   fast-path, emulated fallback" seam (§5.2), applied opportunistically.
5. **Android integration seam = Native Bridge provider (§5.4).** Expose RetroBug as an `armeabi-v7a`
   native-bridge so ART/Zygote loads guest `.so`s through the standard interface. Provide: guest **auxv (ARM)**,
   32-bit **`linker`+bionic** as guest data, `svc`→**ARM Linux syscall table** (§3), fabricated **A32 `sigcontext`**
   for signals, and the **kuser/`0xffff0000` page** (§7.6).
6. **Validation as a first-class feature.** Build a **differential fuzzer**: random A32/T32 → (interpreter) vs
   (JIT) vs (**ASL-interpreter oracle**) ; golden tests against `qemu-arm-linux-user` output (GPL-clean: black-box,
   compare observable state, no linking) and, on AArch32-capable hosts, against **hardware** as ground truth for
   §7 edge cases (IT+LDREX/STREX, FPSCR, flags). This is how you *de-risk the correctness surface* (§7).

**Why not "just build on QEMU linux-user"?** It's the right *shape* (§3) but the wrong *license* (§2/§3 GPL
boundary) and the wrong *app model* (§3). We reimplement its design permissively, reusing Dynarmic where it helps.

---

## 9. Open questions / unverified (be explicit)

1. **Exact `ID_AA64PFR0_EL1.EL0` bit pattern for "AArch32 present."** Two sources agree `0b0001`=AArch64-only,
   `0b0010`=both, but the *legacy* `0b0000` handling and the kernel's `IMP/AARCH32` enum names conflict across
   renderings. **Action:** read current DDI0487 §D24.2.93 + `arch/arm64/tools/sysreg` before hard-coding; prefer the
   §1.2 functional probe over register parsing anyway.
2. **Version where AArch32-at-EL0 became *architecturally optional*.** Sources split between **Armv8.6-A** and
   **Armv8.8-A/Armv9.3-A**. Firm, safe fact: **first shipping silicon without it = Cortex-A715 (Armv9, 2022).** Not
   yet pinned to an ARM-version clause.
3. **Dynarmic `arm_32` real ARMv7-A coverage.** I confirmed hosts (x64+AArch64), guest scope ("ARM v3–v8"), and
   0BSD license, and that Citra JITs ARM32 — but did **not** confirm: full **VFPv3-D32/`d0–d31`**, the
   **ARMv7-A NEON subset** bionic uses, **IT-block + `LDREX/STREX` correctness**, and **Linux/Android `svc`** handling
   (Dynarmic targets console SoCs). **Action:** source-level audit of `src/arm_32/*` + build a coverage harness.
4. **Does Microsoft WoA *actually* emulate individual ARMv7-only instructions** (as the brief hypothesized)? I
   found ARM64EC = **x64**-on-ARM64 hybrid and WoA running 32-bit ARM **natively via AArch32-EL0** + x86 via
   **Prism**; I did **not** find a primary writeup of per-instruction ARMv7 emulation on AArch64. The *hybrid
   idea* (§5.2) is solid; the specific "ARMv7-only-instruction emulation" claim is **unconfirmed**.
5. **Android GKI `CONFIG_COMPAT` status (per OEM/build).** Whether a given modern arm64 Android kernel still
   enables 32-bit EL0 is a *vendor build* choice; not a single global answer. **Action:** sample real devices at
   runtime with the §1.2 probe; check `zcat /proc/config.gz`/`/proc/1/…` on a fleet (no-root limits this).
6. **Quantified share of "modern arm64 Android that could use HW AArch32."** I gave a *directional* estimate
   (majority of CPU-IP still capable, but the *usable/needed* intersection trends to ~0 and is undefined on iOS).
   A hard percentage needs device-fleet data (e.g. Play device stats) not gathered here.
7. **QEMU linux-user ARM signal/`sigcontext` + `clone`/TLS corner cases** and **CP15 `MIDR`/cache reads bionic
   actually issues** on a given Android API level: modeled conceptually from docs/source (§3,§7), not exhaustively
   enumerated from the Android bionic corpus here.
8. **Arm MRA/ASL redistribution license** for shipping *generated* decoder tables in a commercial/product build
   (research license ≠ product license) — must be checked before depending on generated-from-spec tables in a
   distributed RetroBug binary.
9. **Concrete perf target.** "TCG ≈ 8–12× slower (§2)" is for *full-system*; a *user-mode-only, weak-ordered
   same-family* DBT should do better, but I have **no measured ARMv7-user-on-AArch64 DBT number** — needs a
   prototype benchmark against real app `.so`s.
10. **`linux-user` GPL-clean reimplementation scope.** Confirm which ARM `linux-user` behaviors (fake sigframe,
   `ARM_NR_*` magic calls, `cacheflush`/`set_tls`) are *required* by the specific Android API levels we target —
   the syscall-table deltas were described from `cpu_loop.c`, not fully diffed against a target Android NDK's
   `unistd`/bionic syscall usage.
