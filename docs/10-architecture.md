# RetroBug Architecture — Phase 1

Status: **decision document**. Every `AD-n` below is a decision with a stated rationale and, where it exists, a
named rejected alternative. Every `C-n` from [`research/90-cross-check.md`](research/90-cross-check.md) is
resolved in [§16](#16-contradiction-resolution-register) and implemented by one of the `AD-n`. Where something
rests on assumption rather than evidence, it says so and names the experiment.

Evidence convention used throughout: **[1L]** = I read the primary source myself in this session
(line-verified); **[1A]** = a sub-agent read a primary source and I did not re-verify; **[INFER]** =
reasoned from documented behaviour, no direct source; **[UNVERIFIED]** = load-bearing and unchecked.

---

## 1. Terminology, defined once

The product brief forbids describing this project as API-call binary translation or a compatibility shim, and
Apple's review vocabulary (guideline 4.7) speaks of *emulator apps*, not *compatibility layers*. `README.md:7,21`
currently calls RetroBug a "compatibility layer"; that is wrong and will be corrected (see `docs/00` §3.1). Three
terms, used consistently from here:

| Term | Definition in RetroBug | Where it applies |
|---|---|---|
| **CPU emulation** | Executing guest ARMv7 machine code by decoding it and running it as translated instructions on the host CPU, reproducing ARMv7 register, coprocessor, memory and ordering semantics. | Every byte of `lib/armeabi-v7a/*.so`. Always emulation. Never API translation. |
| **Runtime reproduction** | RetroBug supplying, in its own process, the Android runtime surface a 32-bit app expects: ELF loading, bionic, syscall ABI, linker namespaces, `Context`/resources, `Settings`, permissions, graphics/audio/input entry points. | The service layer above the emulated CPU, plus everything needed because the guest is not an installed package (§7). |
| **API forwarding** | Where the host already implements an API, a 32-bit guest shim accepts ARM32 calls and calls the host's 64-bit implementation. | Guest `libGLESv2.so` → host GLES; guest `libOpenSLES.so` → host audio. This is *not* translation of guest code, and is not a substitute for CPU emulation anywhere guest machine code runs. |

**Non-negotiables inherited from the brief**, restated because architecture decisions are checked against them:
(a) CPU + runtime emulation, not API translation; (b) normal path in-process, not a VM, not a legacy OS image;
(c) the isolated environment is a distinct subsystem reusing the same CPU core, per-app-scoped FS/network/config
isolation, network off by default, never default, never a silent fallback; (d) Android first but the CPU core
shared with iOS from day one; (e) general-purpose apps, not games-only; (f) zero configuration for the common
case; (g) no hardcoded per-app logic outside the data-driven App Revival mechanism; (h) isolation claims actively
verified.

---

## 2. Decision index

| # | Decision | Rationale | Resolves |
|---|---|---|---|
| AD-1 | Guest machine code runs on a **forked Dynarmic** (0BSD) A32 front end with two executors over one IR | Verified ARMv7-A coverage + existing, CI-differentially-tested AArch64 backend, at a license that fits Apache-2.0 | C2, C3 |
| AD-2 | **Tier I (IR interpreter) is a first-class target from commit one**, not an iOS add-on | A shippable iOS app cannot JIT; some OEM Android builds may lack `execmem`; Tier I is the universal reference | C3, C8, C10 |
| AD-3 | Managed code runs on **host ART**; only native code is emulated | The only performant shape; matches the Houdini precedent; keeps the emulated surface to machine code | C9 |
| AD-4 | The guest is **never an installed package** — RetroBug synthesises `ApplicationInfo`, resources, data dirs, permissions | An armeabi-v7a-only APK cannot install on a 64-bit-only device; this is *why* runtime reproduction is unavoidable rather than optional | C14 |
| AD-5 | Native binding via **one pre-shipped agent `.so` + host-side `RegisterNatives`**; zero runtime-generated libraries, ever | Exec-from-app-data is `neverallow`-blocked and ART's fd-load path is unreachable | C1 |
| AD-6 | Two build-time guest **address models** selected by startup probe: flat/identity on Android, biased `GUEST_BASE` on iOS | Fixed low-VA reservations fail intermittently on Darwin | C16 |
| AD-7 | Guest page granule fixed at **4 KiB**, host granule queried; no `userfaultfd` | ARMv7 guests and their ELFs are 4 KiB; SMC detection must work where `userfaultfd` is absent | C6, C10 |
| AD-8 | Three isolation **tiers T0/T1/T2** with enforced naming discipline; "isolated" reserved for T1 and gated on a runtime self-test | Brief (c)+(h); `:guestN` provides zero isolation and must not be sold as such | C5, C13 |
| AD-9 | Graphics/audio exposed as **`RbGfxChannel` with `InProcessChannel` and `RingChannel`**, built from day one | T1 has no `gpu_device`; without the seam the sandbox tier forks the renderer | C5 |
| AD-10 | Guest system libraries **built from AOSP/NDK source**, never extracted from a device | Extracted binaries are not redistributable; AOSP bionic is BSD-3 | C12 |
| AD-11 | **`qemu-arm` linux-user as an out-of-process semantic oracle** | Shared front end makes I-vs-J differential testing prove nothing about semantics | C8 |
| AD-12 | One profile **store and schema**, keyed `(guestAppId, instanceId)`, defaults from capability probes, no inheritance across tiers | Zero-config (f) and the no-leak rule (c) are both satisfied by one mechanism, correctly keyed | C14 |
| AD-13 | App Revival is **three data tiers** (TOML overlays → hash-gated in-memory patches → shipped profiles); no package-name `if` in C++ | Brief (g), and the developer's proven pattern in `docs/research/08` | C14 |
| AD-14 | Vendored Dynarmic kept **byte-identical to upstream**, RetroBug changes only in `patches/` + wrapper | `docs/research/08`: upstream drift cost that team weeks; also makes provenance auditable | C12 |
| AD-15 | Host **GLES forwarding on Android**, ANGLE-on-Metal on iOS, **SwANGLE as universal reference renderer**; Vulkan is not the host abstraction | Android 16 CDD still mandates GLES1/2/3 [UNVERIFIED U-4]; one host API per platform beats one abstraction everywhere | C4 |
| AD-16 | **Vocabulary + license hygiene in-repo**: `NOTICE.md`, `LICENSES/`, `REUSE.toml`, clean-room protocol doc, `external/` provenance | Apache-2.0 §4 is a distribution obligation that starts on day one, not at release | C12 |
| AD-17 | **T1 entry is a long-press on the app's library entry**, opening a contextual "Enter isolated environment"; single tap still means T0 | A deliberate, discoverable gesture is what makes "never a silent fallback" true in practice, and it fixes the list's data model before Phase 2 wires it | — |
| AD-18 | **App Revival patches are orthogonal to the tier** — they apply on T0 and never require T1; the two are separate optional tools on one profile row | Coupling them would push apps into the slower tier to fix a problem a patch already solves | — |

---

## 3. System shape

```
┌────────────────────────────────── RetroBug APK ──────────────────────────────────┐
│                                                                                   │
│  UI layer (Kotlin): library · import · launch · advanced settings · tier badge    │
│                                  │                                                │
│                    ProfileStore (AD-12)  ·  App Revival engine (AD-13)            │
│                                  │                                                │
│  ┌──────────── T0: normal path, one process ─────────────┐                        │
│  │ host ART (64-bit)                                     │                        │
│  │  ├─ guest DEX, loaded by RetroBug (AD-3/AD-4)         │                        │
│  │  ├─ natives bound by RegisterNatives (AD-5)           │                        │
│  │  └─ RetroBug agent .so  ← pre-shipped, arm64          │                        │
│  │                │ trampoline (AAPCS ⇄ AArch64)         │                        │
│  │  CPU core (AD-1): A32/T32 decode → IR → {Tier J, I}   │                        │
│  │  guest memory: fastmem 4 KiB pages (AD-6/AD-7)        │                        │
│  │  guest ELF loader + linker · syscall layer · signals   │                        │
│  │  32-bit guest libs: bionic + generated proxies         │                        │
│  │  RbGfxChannel=InProcess ──► host GLES/audio/input      │                        │
│  └───────────────────────────────────────────────────────┘                        │
│                                  ▲ selects executor/renderer per instance         │
│  ┌──────────── T1: isolated environment (AD-8/AD-9) ─────┐                        │
│  │ separate isolatedProcess service, fresh UID, isolated │                        │
│  │ _app SELinux domain: no file naming, no non-AF_UNIX   │                        │
│  │ sockets, no gpu_device, 3 findable services           │                        │
│  │  SAME CPU core (Tier J still legal: execmem allowed)  │                        │
│  │  RbGfxChannel=Ring ──► host renders (or SwANGLE in-sandbox)                      │
│  │  instance root = only writable FS; profile lives here │                        │
│  └───────────────────────────────────────────────────────┘                        │
└───────────────────────────────────────────────────────────────────────────────────┘
```

Cross-platform: everything from "CPU core" down through "32-bit guest libs" is platform-agnostic C/C++ (§13).
The iOS build replaces the left-hand ART block with the same shape but Tier I, and swaps the channel/graphics
backends.

---

## 4. AD-1 — CPU emulation strategy: fork Dynarmic

### 4.1 What was decided and why

**Decision: build on a vendored fork of Dynarmic's A32 front end, adding a new interpreter backend to its IR.**

Tradeoff analysis, as required by the brief:

| Axis | Build our own | **Fork Dynarmic (chosen)** | Adapt QEMU/Unicorn |
|---|---|---|---|
| Performance | Unknown; years to reach parity | A32→AArch64 backend exists today, with fastmem and `LDREX/STREX` lowering | Unicorn: no TB chaining across calls; QEMU TCG: strong but GPL |
| Correctness risk | Decoder is the long tail (Thumb-2, IT-block, ASIMD); Arm's MRA is **banned** (proprietary, no-redistribution, anti-patent-analysis clause **[1A]**), so the tables must be hand-derived from the ARM ARM regardless | Same ISA work already done once under a permissive license | Proven, but cannot be linked |
| Maintenance | Ours entirely | Ours, but upstream tree was already maintained 2017–2024; forks still active | QEMU = perpetual rebase against a GPL project |
| Licensing | Clean | **0BSD, verified verbatim [1L]**: "Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted", © 2017 merryhime → fully Apache-2.0 compatible | **GPL-2.0: disqualifying** for an Apache-2.0 + Play/App-Store product |

**The ARMv6-vs-ARMv7-A question, resolved.** Dynarmic's A32 front end is publicly associated with Citra, whose
guest is an ARM11 (ARMv6). RetroBug needs ARMv7-A: NEON, `sdiv`/`udiv`, full Thumb-2, ARMv7 barriers, CP15 TLS.
I enumerated the source tree myself **[1L]** (`api.github.com/git/trees`, `truncated: false`) and the
`frontend/A32/translate/impl/` directory contains, among others:

- `divide.cpp` → `SDIV`/`UDIV` (ARMv7-only; absent from ARMv6)
- `barrier.cpp` → `DMB`/`DSB`/`ISB` (ARMv7 memory model)
- `asimd_three_regs.cpp`, `asimd_two_regs_misc.cpp`, `asimd_one_reg_modified_immediate.cpp`,
  `asimd_misc.cpp`, `asimd_load_store_structures.cpp`, `packing.cpp`, `parallel.cpp`, `reversal.cpp`,
  `saturated.cpp` → the NEON/ASIMD group, plus `decoder/asimd.{h,inc}`
- `thumb32_*.cpp` (13 files by encoding group) + `decoder/thumb32.{h,inc}` → Thumb-2
- `synchronization.cpp` → `LDREX`/`STREX` family; `status_register_access.cpp` → `MRC`/`MCR`
- `coprocessor.cpp`, `backend/arm64/emit_arm64_a32_coprocessor.cpp` → CP15 on the AArch64 host
- `vfp.cpp` + `decoder/vfp.{h,inc}` → VFP; `frontend/A32/ITState.h` → IT-block state
- Host backends for A32: `backend/arm64/` **and** `backend/x64/` **and** `backend/riscv64/`, each with
  `a32_address_space`, `emit_arm64_a32_memory`, `emit_arm64_a32_coprocessor`
- `frontend/A32/disassembler/` → its own disassembler, reusable for our tooling

So the front end is **not** ARMv6-scoped. Two honest limits on that conclusion: (i) a file listing proves the
encodings are *dispatched*, not that their *semantics* are complete or that FP status is exact — Dynarmic's own
README says FPSR state is approximate **[1A]**; (ii) the coverage of the ARMv7 subsets real apps use is
**[UNVERIFIED → U-8]**. U-8's experiment is cheap and host-side: disassemble every `.so` in the corpus (§Phase 2)
with `llvm-objdump` and diff the mnemonic set against the decoder's coverage. Until that runs, treat ARMv7-A
completeness as **strongly indicated, unproven**.

**Update after M0 step 3:** that measurement is now done — see
[`32-isa-coverage.md`](32-isa-coverage.md), which quantifies what real ARMv7 bionic actually demands and
formally downgrades the file-listing argument above to "dispatched, not proven". §17's U-8 row carries the
result: demand half closed, semantic correctness still open.

**Consequences of accepting this dependency**, stated plainly:
- Upstream is unmaintained: `merryhime/dynarmic` 404 **[1L]**, `lioncash/dynarmic` archived 2024-03-05 **[1A]**.
  We take a live fork (`azahar-emu/dynarmic`) as the import source and treat it as ours to maintain. AD-14 keeps
  that survivable.
- There is **no interpreter and no `DYNARMIC_USE_JIT` option** **[1A]**, and `MemoryProcessor` no longer exists
  **[1A]** (doc 01 assessed a stale API — see the corrections list). Tier I is genuinely new work, not a build flag.
  `docs/research/01` must be amended on both points.
- Dynarmic's IR is designed for host codegen. Interpreting it is viable but unmeasured → **U-9**.

### 4.2 One front end, two executors

```
A32/T32 fetch+decode → translate → IR blocks ─┬→ Tier J: AArch64 host code  (Android, macOS)
                                              ├→ Tier I: IR interpreter     (iOS; fallback everywhere)
                                              └→ Tier X: x64 host code      (dev/CI only, oracle cross-check)
```
Semantics live only in the front end. Flag evaluation (ARM's four status bits vs host NZCV) and the IT-block are
resolved *before* IR, so host-flag optimisation is a Tier J patch and cannot change guest-visible semantics — this
is what makes C3 a false conflict (the IR *is* Dynarmic's IR; there is no second executor to disagree with).
Berberis' literal `TODO: Flags in host flags` in its ARM path **[1A]** is the cautionary precedent: it pays
`LDR/MSR` per block. We do not copy that; lazy flag materialisation is a Tier J pass with a per-block "flags
needed" liveness analysis, validated against Tier I.

---

## 5. AD-2 — iOS JIT constraint, designed in from day one

**Verdict: JIT is not obtainable by a shippable iOS app, so the core is IR-first with the interpreter tier as a
required, always-maintained path.** Supporting evidence, in order of strength:
- Apple's own porting guide: **"`pthread_jit_write_protect_np()` isn't available on iOS"** and `MAP_JIT` requires
  `com.apple.security.cs.allow-jit`, which is macOS-only **[1A]**.
- iOS treats an RWX `mmap` request as "writable, but not executable" **[1A]**; xnu gates `MAP_JIT` behind the code
  signing monitor (EPERM) **[1A]**.
- The developer's first-party probes on this machine: `allow-jit` "does NOTHING on iOS — an active red herring";
  uncatchable SIGKILL on unprepared execute in iOS 26/27 betas **[1A, from docs/research/08]**.
- Policy precedent: iSH's DMA Article 6(7) request for a JIT entitlement was **refused** — "no JIT for
  non-browser apps on iOS" **[1A]**. Sideload JIT requires dev-signing plus a tether/pairing (AltStore, StikJIT).

This is the **hybrid** answer, and the tradeoff is explicit in §14: iOS ships Tier I, and per secondary-source
comparisons (libriscv ≈ 1/3 of a JIT, iSH gadgets 3–5× over `switch` dispatch) Tier I costs roughly **3×** versus
Tier J — **[UNVERIFIED → U-9]**, measured by instrumenting `TranslateBlock` on a real guest `.so` and running
CoreMark-ARMv7 under both tiers before any iOS performance claim is made anywhere.

Two consequences that are easy to miss and are therefore decided here:
1. **"Apple Silicon" is two products.** A Mac-native/Catalyst build *can* JIT; an iOS-on-iPad build cannot. The
   `RbCodeCache` seam (§8.3) has three backends and a platform selection table, so this is a configuration, not a
   fork.
2. **Tier I is also the Android safety net.** OEM sepolicy overwrites are possible (**[UNVERIFIED → U-13]**), so
   a device that denies `execmem` degrades to Tier I instead of failing. That is the strongest argument for
   building Tier I *early* rather than "for iOS, later".

---

## 6. AD-3 / AD-4 — where the emulation cut falls, and what RetroBug must fake

**AD-3: guest Java/Kotlin runs natively on host ART; only ARMv7 machine code is emulated.** DEX bytecode is
architecture-neutral and the host's ART is fast and correct; the expensive, interesting problem is bionic, the
linker, and guest native libraries. This is the Houdini shape **[1A]**, and it is why "CPU emulation" and
"general-purpose app support" can coexist with a performance budget.

**AD-4: the guest APK is never an installed package.** On a 64-bit-only device the platform package manager
rejects an armeabi-v7a-only APK, so RetroBug runs it from a file. That single fact generates the whole runtime
reproduction surface, and it is worth listing because it is where the project's real risk concentrates:

| The guest expects | Because it is not installed, RetroBug must supply | Confidence |
|---|---|---|
| `ApplicationInfo`, `PackageInfo`, version codes | synthesised from the parsed manifest | HIGH |
| Resources + `AssetManager` over the guest APK | `addAssetPath` is a hidden API; alternative is a RetroBug-owned resource loader | **MEDIUM → U-11** |
| `Context` with the guest's package name | hidden `Activity.attach` / `ContextImpl` construction, or the VirtualApp-style stub lineage | **LOW-MEDIUM → U-11** |
| Its own data dir | per-instance root under RetroBug's `files/` (normal path), instance root (T1) | HIGH |
| Runtime permissions | mapped onto RetroBug's own grants, per-app, surfaced in UI | HIGH (policy work, not technical) |
| An app UID | there isn't one on T0; in T1 the *process* has a fresh UID, but it is RetroBug's sandbox, not the guest's | HIGH, and must be disclosed (§10.6) |
| `Settings.Secure/System` rows | virtualised per `(guestAppId, instanceId)` (the table is keyed by package) | HIGH |

**Degraded hosting modes are a designed fallback, not an absence of one**: v1a "guest as library" (no guest UI,
guest services/activities not hosted — useful for the native-only bring-up milestone) and v1b a `SurfaceView`
window hosting the guest's own rendering (the `NativeActivity` path, which needs the fewest hidden hooks because
the guest never instantiates an Android UI framework `Activity` subclass beyond `NativeActivity` itself). The
product listing must not promise "any APK on Android 16" until U-11 is green.

---

## 7. AD-5 — native binding without generating code files

The previous plan (doc 02) required a runtime-generated arm64 "shadow `.so`" written into app storage and
`dlopen`'d. That is **prohibited**, by two independent layers, both verified:

1. SELinux: `neverallow { all_untrusted_apps -untrusted_app_25 -untrusted_app_27 -runas_app } { app_data_file
   privapp_data_file }:file execute_no_trans;` — read verbatim from `private/app_neverallows.te:69-73` **[1L]**,
   with a stronger sibling rule covering `isolated_app_all` on all `data_file_type` **[1A]**.
2. ART/linker: `dlopen` from an fd needs `ANDROID_DLEXT_USE_LIBRARY_FD`, but ART sets only
   `ANDROID_DLEXT_USE_NAMESPACE` (`native_loader_namespace.cpp:168-171`), so the path check cannot be bypassed
   **[1A]**.

Meanwhile `apk_data_file` *is* executable: `allow appdomain apk_data_file:file { getattr open read ioctl lock map
x_file_perms }` with `x_file_perms = { getattr execute execute_no_trans map }` (`private/app.te:442-443`) **[1L]**,
and `apk_data_file` is not an `app_data_file_type`, so the neverallow does not reach it **[1A]** — including inside
an isolated process.

**Decision (AD-5).** Ship **one** pre-built arm64 agent library inside RetroBug's own APK (`jniLibs/`), whose
exported symbols are irrelevant. Binding proceeds entirely at runtime without touching the filesystem:

1. **At import**: parse the guest DEX for every `native` method (class descriptor, name, shorty — the DEX is
   authoritative and already carries the shorty, which is what ART's bridge trampoline path would have needed and
   cannot be reached: `java_vm_ext.cc:335` passes `shorty` only when `NeedsNativeBridge()` **[1A]**). Parse
   `lib/armeabi-v7a/*.so` symbol tables for `Java_*` exports and `JNI_OnLoad`.
2. **At load**: `findLibrary(name)` returns the pre-shipped agent path, so ART's `dlopen` succeeds and
   `UnsatisfiedLinkError` never fires.
3. **At bind**: RetroBug calls host-side `RegisterNatives` (a fully supported public path **[1A]**) mapping each
   native method to a host trampoline that marshals per AAPCS32 and enters the emulator.
4. **Guest `JNI_OnLoad`** runs *on the emulator*, against a synthesised 32-bit `JNIEnv*` proxy, invoked by
   RetroBug rather than by ART — ART's own `JNI_OnLoad` call happens under a class-loader override
   (`java_vm_ext.cc:1125-1142` **[1A]**), which the proxy must reproduce exactly.
5. **`System.load(absolutePath)`** bypasses `findLibrary` entirely (found in cross-check, not in doc 02). The
   agent installs a `Runtime` hook so an absolute path resolving inside the guest APK maps to the same agent +
   same import-time index. Any path outside the APK fails as it would on a real device.

Rejected: (i) per-app pre-generated shadow libs shipped in the APK — corpus-bounded, violates (f) and (g);
(ii) memfd + `/proc/self/fd/N` dlopen — dead per blocker 2; (iii) using ART's trampoline contract — bridge-gated,
unreachable to an app **[1A]**. **The same mechanism works identically on iOS and in T1, which is why AD-5 is
architecturally preferred over anything needing a generated file.**

---

## 8. CPU/OS-level emulation vs service-layer reproduction

The brief asks for this boundary explicitly.

| Concern | Layer | How |
|---|---|---|
| ARMv7 instruction semantics, CPSR/APSR, IT-block, VFP/NEON, CP15 | **emulated** | Dynarmic A32 front end → IR → tier (§4) |
| Guest virtual memory, page tables, protection | **emulated** | RetroBug's own 4 KiB page table over fastmem (§8.2) |
| ARM Linux syscall ABI (`r7` numbering, `__ARM_NR_cacheflush`, `sigreturn`/`ucontext`, EAGAIN/EINTR translation) | **emulated** | Monitor executes syscalls from emulated code; nothing reaches the kernel directly |
| Guest ELF loading, relocation (`R_ARM_ABS32`, `R_ARM_THM_CALL`, `R_ARM_JUMP_SLOT`), linker namespaces | **emulated** | RetroBug's own AArch32 loader (running guest code under emulation where it reuses AOSP `linker`) |
| Guest `libc` (bionic), `libm`, `libdl`, `libstdc++`/`libc++`, TLS (`tp` register) | **emulated** | AArch32 bionic built from AOSP source (BSD-3), executed by the CPU core (AD-10) |
| `libEGL`/`libGLESv1_CM`/`libGLESv2`/`libOpenSLES`/`libandroid` | **reproduced as a service layer**: 32-bit shim + **API forwarding** to host | Generated proxies from the Berberis `native_bridge_support/android_api` machinery (Apache-2.0, genuinely AArch32 **[1A]**), NDK 32-bit linker stubs as the ABI symbol authority |
| Java framework, `Context`, resources, `Settings`, permissions | **reproduced** on host ART | §6/AD-4 |
| Surface/BufferQueue, audio output, input events | **reproduced** | `RbGfxChannel`/audio channel; host `Surface`/`AudioTrack`/`MotionEvent` mirrors (§9) |
| `dlopen`/`JNI` bridging between host ART and guest code | **reproduced** | AD-5 |

### 8.2 AD-6 / AD-7 — address and page model

- `RB_GUEST_PAGE_SHIFT = 12` fixed. Host granule is read once at init via `getauxval(AT_PAGESZ)`; Android 15+
  ships 16 KiB-page devices, so a guest 4 KiB mapping that straddles a host granule **leaves fastmem** (null PTE +
  fault-based slow path) rather than growing a soft TLB. Mixed-granule frequency is **[UNVERIFIED]** but bounded
  by the design's correctness either way.
- Two address models, chosen at build time and *confirmed by probe at startup*: Android → flat/identity low 4 GiB
  window, reservation verified and reported, never crashed on; iOS → biased `GUEST_BASE` with an OS-chosen base.
  `docs/research/08` item 7 measured that fixed-window reservations fail intermittently on iOS, which is the
  reason this is not "primary + fallback" but two real models (**C16**). **Every** pointer marshalling helper must
  take `guest_base` into account from the first commit — a `guest_base == 0` shortcut anywhere would be a latent
  iOS-only bug.
- Pointer crossing is the hard part of a 64-bit host sharing its address space with a 32-bit guest: host→guest
  arguments (an ART heap pointer, a `jobject`) are **not truncatable**, so crossing uses a 32-bit handle table
  plus low-arena bounce buffers; guest→host pointers are validated against the page table. fastmem faults are
  attributed (dynarmic-style `check_halt_on_memory_access` / `recompile_on_fastmem_failure`) so a real guest
  `SIGSEGV` is delivered as a guest signal instead of killing RetroBug.
- Self-modifying code is detected by write-tracking on translated blocks (`block_range_information`) plus PTE
  nulling — **not** `userfaultfd`, which one doc relied on and another showed is unreliable to deny from an app
  (**C10**), and which iOS lacks. This is also the ARMv7 `__ARM_NR_cacheflush` path: a guest `cacheflush` on an
  executable region must invalidate, not `memcpy`.
- The legacy kuser-helper page at `0xffff0000` is **synthesised as guest code** (bionic's `__kuser_memory_barriers`
  and helper vectors), because no 32-bit kernel provides it here.

### 8.3 Code cache seam

`RbCodeCache` interface, implementations `AnonRwx` (default; permitted by
`allow appdomain self:process execmem;` at `private/app.te:189` **[1L]**), `MemfdDualAlias` (W on the memfd, X on
a second mapping — how ART's own JIT works, and permitted by `app_domain()`'s
`appdomain_tmpfs:file { execute … }` **[1A]**), and `Interpreted` (no executable memory at all). Selection is
probed, never assumed, and the probe result is recorded in the diagnostics report. In T1 the cache is
architecturally forced to be a sandbox-created memfd, because the sandbox provably cannot execute any path it can
name (**[1A]**, `private/app.te:731-746`) — which happens to give AD-8's instance tagging for free.

**Update at vendoring time (was open risk U-6, now closed):** both non-interpretive backends already have
MIT-licensed upstream implementations in the vendored tree. `externals/oaknut/include/oaknut/code_block.hpp`
performs the anon `mmap(PROT_READ|PROT_EXEC)` and RWX forms (plus a Darwin `MAP_JIT` variant that iOS cannot use,
and a Linux `PROT_MPROTECT` variant), and `dual_code_block.hpp` implements the `memfd_create` + dual `MAP_SHARED`
RW/RX aliasing exactly as specified above. Only the `Interpreted` backend — the iOS path and the OEM-`execmem`
fallback — is new work. See `external/PROVENANCE.md`.

---

## 9. AD-9 / AD-15 — graphics, audio, input

**Decision:** forward, don't translate, on Android; ANGLE-on-Metal on iOS; **SwANGLE (ANGLE→SwiftShader,
Apache-2.0) is the universal reference renderer** and doubles as the sandbox renderer and the CI oracle — one
component serving three purposes. Vulkan is **not** the host abstraction (rejected in `docs/research/03`; the
brief's iOS second target is served by ANGLE's metal backend plus MoltenVK for *guest* Vulkan, which is
Apache-2.0 **[1A]**).

Two GLES 1.x host executors remain selectable per instance (data-driven, never by package name) because U-4
("Android 16 CDD still mandates GLES 1.1") is **[UNVERIFIED]**: native host GLES1 forwarding, and ANGLE's GLES1
layer. GLES1 fixed-function fidelity — ANGLE gates generated shaders on a 13-bit state mask, up to 8192 variants,
and dEQP-GLES1 is not in ANGLE's own suite **[1A]** — is the largest single *correctness* tail in the project, and
it is deliberately deferred behind AD-16's reference-renderer oracle rather than hidden.

**The structural decision that keeps T1 honest (C5):** the guest graphics surface is `RbGfxChannel` with
**two implementations built from day one** — `InProcessChannel` (T0, direct calls) and `RingChannel` (T1,
serialised command ring in the instance root, host-side replay, buffer objects kept host-side and named by 32-bit
guest handles). Naive pixel shipping is dead on arrival (~220 MB/s at 720p60 plus a Binder round-trip per frame,
against a ~1–2 MB transaction cap), so T1's *default* is the in-sandbox software renderer and the hardware ring is
an opt-in, deferrable mode. The runtime layer is therefore **split-host** by design, and its proxy libs must be
writable against either transport from the start. `MotionEvent` is a real 32-bit mirror, not a proxy stub: it
crosses as a host C++ object (`android_view_MotionEvent_getNativePtr`) **[1A]**.

---

## 10. The isolated per-app environment (T1) — a distinct subsystem

### 10.1 Why `isolatedProcess` and not anything else

`docs/research/05` evaluated every Android primitive; two are worth restating because they foreclose the
alternatives: user namespaces and mount namespaces are closed to apps (SELinux defines `user_namespace` and grants
it to nobody; `neverallow appdomain fs_type:filesystem ~getattr;` plus a seccomp blocklist entry for
`chroot`/`mount`/`umount2` **[1A]**), and `android:process=":guestN"` slots share one UID, one SELinux type and
each other's files — i.e. **`:guestN` provides no isolation at all** and may only be used for crash/lifecycle
containment (**C13**, enforced by naming discipline). By contrast a bound isolated service gets a **fresh UID from
the 99000–99999 band per instance** with its own `isolated_app` SELinux domain, and its restrictions are
`neverallow`-enforced, meaning no OEM can silently widen them without failing build-time checks **[1A]**.

### 10.2 Enforcement matrix — what actually enforces each claim

| Claim | Enforced by | Mechanism |
|---|---|---|
| Cannot name a file outside its instance | **kernel/SELinux** | `neverallow isolated_app_all app_data_file_type:file open;`, no `dir` perms at all **[1A]** |
| Cannot execute anything it can name | **kernel/SELinux** | `no_x_file_perms` neverallow on all `data_file_type` except `apk_data_file`/`dalvikcache`/`system_data_file`/`apex_art` **[1A]** |
| Network off by default | **kernel/SELinux** | `neverallow isolated_app_all { … } : { tcp_socket udp_socket … } create;` — AF_UNIX only **[1A]** |
| JIT still works there | **kernel/SELinux** | `isolated_app ⊂ appdomain` → `execmem` **[1L/1A]** |
| Cannot reach Android system services | **kernel/SELinux** | service discovery capped at `activity`/`display`/`webviewupdate` **[1A]** |
| Cannot see another instance's files/paths | **RetroBug** | VFS resolver + fd capability table (`docs/research/05` §1.5): component-at-a-time `openat(O_PATH|O_NOFOLLOW)` + `fstat`, no host path strings ever, `..` clamped at the instance root, `/proc` served synthetically, RetroBug's own fds kept below `FD_BASE` and absent from the table |
| Config bound to *that* app's profile | **RetroBug** | AD-12's namespaced store, physically inside the instance root |
| Instance token unforgeable | **kernel** | `Binder.getCallingUid()` is stamped by the kernel |

**`openat2(RESOLVE_IN_ROOT)` is unavailable** (`openat2` is not in bionic's `SYSCALLS.TXT`, so it is outside the
app seccomp allowlist — and filters only ever tighten **[1A]**). So the kernel's own containment primitive cannot
be the enforcement point and RetroBug's resolver must implement `RESOLVE_IN_ROOT` semantics itself. The same
absence is what removes `io_uring`, `landlock` and `keyctl` from the guest's escape inventory — a rare piece of
good news, and the reason `seccomp` is a *backstop* (RetroBug installs its own TSYNC'd filter in the child,
denying `execve`/`ptrace`/`process_vm_*`/`unshare`/`setns`/`perf_event_open`/`userfaultfd`/`bpf`/`socket` outside
AF_UNIX) rather than the primary mechanism: a filter cannot distinguish RetroBug's own `write()` from the guest's,
so it may only apply to a process containing no trusted RetroBug code — which is exactly T1.

### 10.3 Lifecycle

`create` (allocate instance root, write profile row, snapshot probe results) → `enter` (explicit user action;
bind the isolated service; **run the self-test battery before declaring the environment isolated**) → `run` →
`exit` (drain capability table, assert empty, seal state) → `teardown` (archive or erase). Plus a real
**`REAPED`** state: the system kills an isolated process the moment it stops being needed
(`SUBREASON_ISOLATED_NOT_NEEDED`) **[1A]**, so an isolated environment is *not* a long-lived daemon; state must be
durable on disk, never process-resident. A failed bind **fails visibly** — a degraded, clearly-labelled message,
never a silent in-process fallback (a structural guarantee: the T1 guest loader cannot be instantiated in-process;
test **T-37** asserts this).

### 10.4 Verification is a release gate, not a test suite (brief (h))

The leak-probe fixture from `docs/research/05` runs **at entry**, before the badge may say "isolated":
`Binder.getCallingUid() ∈ [99000,99999]`; `open()` of its own `files/` path returns `EACCES`;
`socket(AF_INET)` fails; a marker file in another instance's root is invisible; `AF_UNIX`-to-`dnsproxyd` cannot
produce a name resolution; `ptrace`/`/proc/PID/mem` of a sibling fails; no cross-instance code-cache aliasing.
Shipped claims name their enforcer (kernel vs RetroBug) in the UI string itself.

### 10.5 Tier table and required disclosure wording

| Tier | Subject | Enforcement | May be called | Network |
|---|---|---|---|---|
| **T0** | guest in RetroBug's own process, per-app profile dir, monitor-mediated FS/settings | advisory — complete mediation, but bypassable by a RetroBug bug | "separate profile" — **never** "isolated", **never** "sandbox" | the host app's network; not a per-guest claim |
| **T1** | one fresh `isolatedProcess` service per instance | OS-enforced (UID + SELinux + `neverallow`) plus RetroBug's seccomp backstop and VFS | "isolated environment" | **off by default**; the sandbox cannot grant itself access, and the opt-in lives only in that app's profile |
| **T2** (later) | app-zygote / `:guestN` pre-warm | none per-instance | must never be presented as isolation | — |

T0 must be entered **only** by explicit user choice with the difference in one sentence, and never after a T1
failure. T1's disclosure includes performance: no `gpu_device` means materially worse fps and cold start, so T1 is
a different performance contract, and (f)'s "zero configuration" applies to T0 only.

### 10.6 iOS: what cannot be achieved, documented rather than assumed

One installed app = one container, one Seatbelt profile, all apps share the `mobile` UID; there is no
`isolated_app` analogue, no seccomp equivalent, no `fork`/`posix_spawn`, and app extensions cannot be launched,
fed, or kept alive at will by the containing app. Network Extensions are capability-gated (`com.apple.developer.
networking.networkextension` needs an approved use case), device-global and consent-gated, and cannot express
per-instance policy. **On iOS, cross-instance isolation is 100 % RetroBug-enforced and resists the guest and other
apps, not a RetroBug defect** — the `docs/research/05` §3.5 disclosure list D-1…D-10 ships as user-visible
wording, with crypto-erase (Keychain-wrapped key + `FileProtectionType.complete`) substituting for guaranteed
teardown.

### 10.7 AD-17 — entering T1 is a long-press on the app's list entry

Decided now, not deferred to polish, because the gesture determines the library screen's data model (an app entry
must know its own tier state and patch state before Phase 2 wires the list) and because a discoverable, deliberate
entry gesture is what keeps brief (c)'s "never a silent fallback" true in practice.

**The interaction.** The library is RetroBug's normal app list. **A single tap on an entry launches it on T0** —
that is the zero-config path (f) and it never changes. **A long-press (long-hold) on an entry opens that app's
contextual action surface**, whose primary item is:

> **Enter isolated environment** — for this app only

Long-press is the *defined* gesture. Because long-press alone is inaccessible (motor impairment, switch-control and
screen-reader users, and some stylus users), the same contextual surface must also be reachable from a visible
overflow control on the entry and from the app's detail screen; the gesture and the overflow open **one** menu, not
two implementations. That is an accessibility requirement on the design, not a later addition.

**Contextual items on the same surface** (all per-app, all per-tier-explicit): compatibility profile summary,
active App Revival patches (§12), force-check/repair the isolated environment's self-test, reset this app's
profile, and remove this app.

**States the entry must render**, which is why this constrains the data model:

| State | Rendering |
|---|---|
| never launched | plain entry |
| last/next launch on T0 | plain entry; tier is implicit (T0 is the default and needs no badge cluttering the list) |
| flagged by App Revival heuristics as wanting T1 | quiet indicator **plus** a one-line reason; tapping still launches T0 — the indicator only points at the long-press menu |
| T1 created for this app | labelled "Isolated environment created" so the user knows a second, separate thing exists |
| currently running in T1 | in-progress indicator, and the running surface carries the badge below |

**What must never happen:** no automatic transition to T1, no "we couldn't start it normally so we switched you"
path, and no T1 option that is disabled-but-visible without an explanation. Concretely: the T1 bind failure case
(§10.3) renders a failure message naming the reason and **must not** offer "try normally instead" as a follow-up,
because that would reintroduce the silent fallback through the back door.

**Entering T1 shows a first-run screen** stating, in the user's language, the reduced contract — no network unless
they turn it on for this app, software rendering unless they opt into the experimental hardware channel, slower
cold start and lower fps, and that a bug in RetroBug cannot cross this line because Android's own process sandbox
enforces it. On iOS the same screen carries the §10.6 disclosures, since T1 there is monitor-enforced only.

**While running, the tier is always visible.** T1 sessions render a persistent badge and a distinct frame/colour so
"running normally" and "running in an isolated environment" can never be confused at a glance, and the exit
affordance is explicit rather than only the system back gesture. T0 renders no badge. This is the visual
counterpart to the naming discipline in AD-8: the badge is where the tier name is *earned* by the runtime
self-test.

---


## 11. AD-12 — per-app profile model

One store, one schema, keyed `(guestAppId, instanceId)`. Rationale: brief (f) requires zero configuration and
brief (c) requires that isolated config never merge with normal config; a single keyed store satisfies both, while
two parallel config systems guarantee drift.

- **No per-package entry is required to launch.** Defaults come from first-launch capability probes (GPU feature
  probe, `execmem`/code-cache probe, 4 GiB reservation probe, GLES1-artefact probe) and are recorded as
  `derived.toml`. A profile may only *override* a probe result; a missing profile never means failure, and no
  package name is ever compared in C++ (brief (g)).
- **Closed, namespaced lookups.** `ProfileStore` accepts only a typed key carrying the instance id; there is no
  API that takes an unnamespaced key, so "fall back to global" is not expressible. Defaults are a versioned
  compiled-in table keyed `(guestAppId, configName)`, never an ambient read of `SharedPreferences`,
  `Settings.*`, or system properties.
- **Tier and patches are separate fields, neither implying the other** (AD-18). A profile row carries
  `tier ∈ {T0, T1}` and `patches: []` independently; the schema must make a cross-reference between them
  unrepresentable, so "patched ⇒ isolated" and "isolated ⇒ patched" are both non-starters in the data model, not
  merely UI conventions.
- **No cross-tier inheritance, stated explicitly.** A T1 row is created from probe results at creation time. It
  is **not** copied from, does not reference, and cannot be read by the T0 row for the same app — and vice versa:
  the T0 loader is handed a `NormalKey` type that an isolated instance cannot construct, and the T1 store lives
  physically inside the instance root, so the FS guarantee *is* the config guarantee. Two different apps' T1
  instances never share a row, a directory, a code-cache key, or a store handle, even when created for the same
  compatibility reason.
- Zero-config for the common case therefore means: import → probes → launch on T0, with nothing asked. Advanced
  settings exist to *override* probes and to enter T1, and both are visibly labelled.

---

## 12. AD-13 — App Revival extensibility

Data-driven, three mechanisms, pattern taken from the developer's proven per-game system in `docs/research/08`
(TOML overlays, hash-gated in-memory patching, built-in profiles with user overrides winning), which is itself the
same shape as Xenia's per-game patch model:

1. **Compatibility profiles** — declarative TOML per `(guestAppId, instanceId)`: executor/renderer/flag choices,
   API-level behaviour divergences, virtualised `Settings`, permission grants, and named behaviour overrides such
   as a synthesised connectivity state.
2. **In-memory code patches** — applied to the loaded guest image inside the emulator's memory, keyed by
   `(library, sha256(build-id), byte pattern, arch)`. **The APK is never modified** (brief (g) and the distribution
   posture in §13.3), so the user's copy stays byte-identical to what they imported and a build-id mismatch makes
   the patch inert rather than harmful.
3. **Diagnostics backbone** — the `>>> INITIALIZATION FAILED AT: <stage> <<<` probe-report pattern proven in
   `docs/research/08`: every appendable stage echoes "requested vs active", because silent-ignore interfaces were
   what cost that team debug sessions. For patches that means a *declared* patch that failed to match must be
   reported as inactive, never quietly skipped.

### 12.1 AD-18 — the patch system is independent of T1, not a feature inside it

**Patching belongs to the runtime reproduction layer, which both tiers use. It is not part of the sandbox and the
sandbox is not a prerequisite for it.** Concretely:

- **A patch applies on the normal T0 path.** An app whose startup connectivity check is broken but which needs no
  actual internet is fixed by the patch and **stays on T0** — no isolation, no software renderer, no cold-start
  penalty. This is the common case and it is the reason the two must not be coupled: coupling would move apps into
  the slower tier to solve a problem the patch already solved.
- **Two orthogonal axes on one profile row.** `tier ∈ {T0, T1}` and `patches: []` are separate fields with no
  validation, UI state, or code path from either implying the other. Entering T1 must never require a patch entry
  to exist; applying a patch must never change the tier. Phase 2 should assert this in the profile schema (no field
  may reference the other) rather than rely on discipline.
- **Usable independently or together.** Apps needing both get both, and the diagnostics report records the pair,
  because a patch that behaves differently in T1 (different renderer, no `gpu_device`, proxied services) must be
  visible as such rather than debugged blind.
- **One honest interaction to surface in the UI.** T1's network is off by default (§10.5), while a connectivity
  spoof reports "connected". Both can be true at once — the app believes it is online and no traffic flows — and
  that combination is often exactly what the app wants. But a user reading "network available" may infer "this app
  can reach the internet", so **every patch that touches connectivity states its effect on connectivity rather
  than its mechanism**, and the T1 network opt-in toggle is presented separately from it.
- **Scoping.** Patches are per `(guestAppId, instanceId)`: a patch created for one app is never offered to, or
  inherited by, another, mirroring the config rule in §11.

### 12.2 Worked example — spoofing "network available" (the reference shape for Phase 2)

Target: a 2012-era app, `armeabi-v7a`-only, `targetSdk 10`, that shows *"No connection — check your network"* and
exits at launch although its actual function needs no network. Three ways such a guard can be written, and the
escalation ladder RetroBug uses to reach each. **All three run on T0.**

| # | How the guard is written | RetroBug mechanism | Layer | Needs T1? |
|---|---|---|---|---|
| 1 | Java calls `ConnectivityManager.getActiveNetworkInfo()` / `isConnected()` | declarative override: the virtualised connectivity service returns a synthesised connected `NetworkInfo` **for that app only** | service layer (host ART side) | no |
| 2 | Guest `.so` calls `getifaddrs()`, `ioctl(SIOCGIFFLAGS)`, or reads `/sys/class/net/wlan0/operstate` / `/proc/net/dev` | virtualise the interface-enumeration surface for that instance: one up, running, non-loopback `wlan0` with plausible MTU and `IFF_UP\|IFF_RUNNING\|IFF_BROADCAST\|IFF_MULTICAST` | syscall/VFS layer (AD-7's resolver already serves a synthetic `/proc`, so this is the same seam) | no |
| 3 | Guard is inlined and uninterceptable by a behaviour flag — a cached comparison, a `strcmp` against a specific interface name, a check whose result is latched before any API is reached | hash-gated in-memory code patch on the loaded guest image | emulator memory, after load | no |

Mechanisms 1 and 2 are declarative profile fields; only 3 needs a byte pattern. **Phase 2 should implement 1 and 2
first**, because most legacy guards are written against the connectivity service, and the escalation to 3 must be
possible to *detect* (the app still fails, the diagnostics show no connectivity API was ever called) rather than
reached by guessing.

Concrete shape of a tier-3 patch, as data (byte sequences illustrative, not from a real APK):

```toml
# revival/<guestAppId>/network-guard.toml
[patch]
id     = "appx-network-guard-1"
title  = 'Skip startup "no connection" guard'
effect = "App is told a network is present. Grants no internet access."   # §12.1 wording rule
when   = "after_load"        # after the guest image is mapped, before JNI_OnLoad runs

[[target]]
library         = "libmain.so"
build_id_sha256 = "9f2c…e17"    # mismatch ⇒ inert + a logged reason, never a best-effort apply
arch            = "armeabi-v7a"
expect          = 1             # exactly one match site; 0 or >1 ⇒ refuse and report

[[target.op]]
kind   = "redirect"            # nop | bytes | redirect
match  = "2D E9 F0 47 06 46 0F F4 80 6F"   # Thumb-2 guard prologue, matched not offset-pinned
to     = "@rb_spoof_connected"             # RetroBug-provided helper, emitted as *guest* code
```

Why `redirect` rather than just `nop`: the fix usually has to make the check *return true*, not remove the branch.
So the patch system needs a way to **emit guest code** — a small assembler for the handful of ARMv7/Thumb-2
sequences patch authors need, placed in a reserved region of that instance's code cache (hence instance-tagged,
per §8.3 and C5) and targeted by a `B`/`BX` from the patched site. Matching a *prologue pattern* rather than
pinning a file offset is what lets one patch survive a re-link of the same app build, and `expect = 1` is the
safety rule: ambiguous matches are refused, not resolved by picking one.

**Verification obligation** (brief (h) applied to patches, not just isolation): every shipped patch file carries
an assertion — the observable post-conditions RetroBug must see if it worked (the app reached `Activity.onResume`,
the connectivity API was called and answered, no `SIGSEGV` inside the patched block) — and CI runs patch tests
against the corpus so a RetroBug change that silently breaks a patch fails a build rather than reappearing as a
user report.

**Flagging heuristics for T1** are a rule set over import analysis plus first-run diagnostics, and they *suggest* —
they never enter: repeated guest crashes with a signature matching an environment-conflict class, `EACCES`/denied
network the app evidently requires, path-collision between two guests' assumptions about shared storage, or an
API-level behaviour divergence RetroBug's shared layer cannot satisfy simultaneously for all apps. Entering T1
remains a deliberate user transition reached by the AD-17 long-press (brief (c)) — and note the ordering: the
heuristic list is triaged **against patches first**, so a patchable startup guard never costs an app a trip into
the slower tier.

---


## 13. AD-16 — cross-platform core split, and repo/licensing hygiene

### 13.1 What is shared vs what is glue

| Platform-agnostic C/C++ core | Platform glue |
|---|---|
| ARMv7 decode, IR, **Tier I interpreter**, flag/IT-block semantics | **Tier J host codegen backend** (AArch64) and its `__builtin___clear_cache` / `dc civac`+`ic iallu`+`isb` discipline |
| Guest memory model, 4 KiB page table, fastmem + `GUEST_BASE` bias, SMC write-tracking | `RbCodeCache` backend selection (`AnonRwx` / `MemfdDualAlias` / `Interpreted`) |
| Guest AArch32 ELF loader, relocation, namespace semantics | Executable-memory syscalls; memfd; `AT_PAGESZ` acquisition |
| Guest syscall table, errno/signal/`ucontext` marshalling | Host signal delivery (`sigchain` interposition), `SIGSEGV` attribution |
| VFS resolver, fd capability table, mount trie, `/proc` synthesis | Sandbox primitive: **Android `isolatedProcess` vs iOS in-process monitor — designed separately, not ported** (the brief calls this out and `docs/research/05` confirms the primitives differ to the point of no equivalence) |
| Profile store, App Revival patch engine, diagnostics recorder | Packaging, entitlements, provisioning, background/suspension contract (iOS jetsam, `applicationDidEnterBackground`), `VpnService` consent UX |
| `RbGfxChannel` interface, guest-side proxy libraries, GLES state machine | Host GL/EGL/audio/input adapters: Android native GLES, iOS ANGLE-metal/MoltenVK; `AVAudioEngine` vs `OpenSL ES`; `CAMetalLayer` vs `Surface` |

The one place the split is *not* clean is JNI (§7): AD-5's `RegisterNatives` + pre-shipped agent design is what
makes that mechanism identical on T0, T1 and iOS instead of a per-platform divergence.

### 13.2 Repo structure

No source layout exists (`docs/00` §1), so the one below is proposed, honouring the `.gitignore`'s already-encoded
Gradle + NDK expectation. `docs/research/08`'s dual-entry CMake layout (`XenDroid/CMakeLists.txt` +
`cmake/ios.toolchain.cmake`, where the Gradle and Xcode builds can never touch each other) is adopted as the shape.

```
app/                 Android host UI + agent .so packaging (Kotlin/Gradle)
core/                platform-agnostic C/C++ (CPU, memory, loader, syscall, VFS, profiles, revival)
external/dynarmic/   vendored, byte-identical; patches/ holds our diffs (AD-14)
guest-libs/          AArch32 bionic + generated proxy libs (AD-10)
platform/android/    Tiers, RbGfxChannel impls, sandbox, code-cache backend
platform/ios/        ANGLE-metal, Tier I default, in-process monitor
tests/isa/           ARM ARM vectors, oracle harness, Tier I≡Tier J equivalence
docs/research/       the 11 Phase 0 documents
```

### 13.3 Licensing posture

Keep Apache-2.0. Add `NOTICE.md` (Apache §4 attribution, starts accruing day one), `LICENSES/` + `REUSE.toml`, and
a written clean-room protocol: QEMU/Unicorn/binutils are **study-only**, no code pasted, citations to specific
upstream files welcome in docs, implementation independently written, provenance recorded per file. Two named
dependencies are **banned**: Arm MRA data (no-redistribution + anti-patent-analysis clause) and any GPL-2.0 linker
target; `gl4es` turns out to be **MIT** and is therefore allowed if ever needed (**C12**). Run `qemu-arm` only as
an out-of-process oracle — the process boundary is the license clean-room, and it is also the correctness oracle
(**AD-11/C8**). No third-party `.so` is ever extracted from a device image (**AD-10**). RetroBug ships zero
APKs and no in-app downloader; users import their own, which is the UTM/iSH/RetroArch distribution precedent
(Apple guideline 4.7 blesses emulator apps offering downloads) and the same posture that keeps Play's
dynamic-code clauses out of scope — though **[UNVERIFIED → U-12]**, and app-virtualisation takedowns are a live
commercial risk with grounds nobody pinned down.

---

## 14. Performance expectations, stated honestly

No measured ARMv7-user-on-AArch64 throughput exists in any research track (**[UNVERIFIED]**); every number below is
an estimate anchored to secondary sources, and Phase 2 must replace each with a measurement before any public
claim:

| Path | Estimate | Anchor |
|---|---|---|
| Tier J, Android | 2–5× native arm64 on compute-bound code | QEMU TCG-class DBT reports; not measured for our shape |
| Tier I, iOS/OEM-fallback | ≈ 3× Tier J (i.e. 6–15× native) | libriscv ≈ 1/3 of a JIT; iSH gadget dispatch 3–5× over `switch` — **U-9** |
| Guest↔host API forward | per-call IPC-class overhead, unmeasured | **U-7/`docs/research/03`** could not quantify proxy-call overhead |
| T1 with software renderer | 5–20 fps for a 2010-era GLES2 app at 720p | `docs/research/05` C4 estimate |
| Pure-DEX apps on host ART | ~native | these need no emulation at all — but the share of 32-bit-only pure-DEX apps is **unmeasured** (`docs/research/02` §8: the 72.5% figure is a biased sample) |

---

## 15. What this document deliberately does not decide

Component interface *signatures*, milestone slicing, the corpus list, CI shape, and the on-device test harness
belong to Phase 2. Two UI matters are **already** decided here rather than deferred: the T1 entry gesture and its
state rendering (AD-17, §10.7), and the rule that patches are tier-independent (AD-18, §12.1). Phase 2's UI work
builds toward those directly.

---

## 16. Contradiction resolution register

| # | Contradiction | Resolution | Implemented by |
|---|---|---|---|
| C1 | doc 02's runtime-generated shadow `.so` vs the `execute_no_trans` neverallow + unreachable fd-load | No generated code files, ever; pre-shipped agent + host `RegisterNatives` + guest `JNI_OnLoad` on the emulator | AD-5 |
| C2 | "build our own" vs Dynarmic's 0BSD A32→AArch64 | Fork Dynarmic; add our own Tier I; treat its ARMv7 completeness as indicated-not-proven (U-8) | AD-1 |
| C3 | data-form IR core vs host-code DBT | False conflict: one front end, one IR, two executors; flags/IT resolved before IR | AD-1, AD-2 |
| C4 | forward-to-GLES vs MoltenVK-first | Forward on Android; ANGLE-metal on iOS; SwANGLE as reference/sandbox/CI renderer; Vulkan not the abstraction; five selectable executors | AD-15, AD-9 |
| C5 | T1 has no `gpu_device` | `RbGfxChannel` with two impls from day one; software renderer as T1 default; hardware ring opt-in and deferrable | AD-9 |
| C6 | 4 KiB guest vs 16 KiB host pages | `RB_GUEST_PAGE_SHIFT=12`; host granule queried; mixed granules leave fastmem; no soft TLB in v1 | AD-7 |
| C7 | nothing is verifiable without hardware | Locally verifiable: ISA decode/semantics vs external oracle, corpus build + ABI verification, IR-tier instrumentation, host-side unit tests. Needs device: end-to-end launch, `execmem` matrix, reservation probe, U-10, U-11 | AD-11, §14, Phase 2 sequencing |
| C8 | interpreter-vs-JIT was supposed to be *the* correctness strategy, but both tiers share a front end | Relabelled as a **back-end equivalence** test; the semantic oracle is external `qemu-arm` out-of-process; corpus mnemonic diffing for coverage | AD-11 |
| C9 | doc 02's DEX loading cannot run inside doc 05's sandbox | T1 uses `InMemoryDexClassLoader` over a host-brokered memfd + AD-5 natives; if U-10 fails, T1 covers native code only and that is disclosed | AD-3, AD-5, **U-10** |
| C10 | one doc relies on `userfaultfd`, another shows it cannot be denied | `userfaultfd` not used at all; write-tracking + PTE nulling, which also works on iOS | AD-7 |
| C11 | doc 01's AArch32 capability probe is itself sepolicy-blocked | No AArch32 code path in v1; hardware AArch32 stays a research note | AD-1 scope |
| C12 | reuse recommendations vs license facts | MRA and GPL linking banned; gl4es actually MIT; Berberis reused only for its Apache-2.0 AArch32 ABI shims; vendoring + NOTICE + REUSE + clean-room protocol | AD-10, AD-14, AD-16 |
| C13 | `:guestN` sold as isolation by one doc, disqualified by another | Tiers named; `:guestN` = lifecycle/crash containment only; "isolated" gated on the T1 self-test | AD-8 |
| C14 | "zero config" vs per-app data | One store/schema, probe-derived defaults, profile overrides only, no package entries required to launch, no cross-tier inheritance | AD-12, AD-13 |
| C15 | terminology drift ("compatibility layer", "API translation") | §1 defines the three terms; README wording corrected; listing uses "emulator" | §1, AD-16 |
| C16 | identity-mapped arena vs measured iOS reservation failures | Two build-time address models behind a startup probe; iOS biased-only; `guest_base` in every marshalling helper from commit one | AD-6 |

---

## 17. Open risks — the honest list

Ordered by how much rework each would cause. **U-2** and **U-10** are the two that could change the design.

| # | Risk | Status | Experiment | If it fails |
|---|---|---|---|---|
| U-2 | A contiguous 4 GiB low reservation succeeds in a zygote-forked Android app process | **[UNVERIFIED]** — derived from `mmap` semantics, not run on a device | one `mmap` probe in `JNI_OnLoad` on emulator + 2–3 devices, logged in the device report | Biased `GUEST_BASE` everywhere: JIT perf survives (dynarmic supports a nonzero `fastmem_pointer`), pointer aliasing becomes slower and every host→guest crossing needs the handle table (§8.2) — a real cost, not a flag |
| U-10 | `InMemoryDexClassLoader` over a host-brokered memfd works in an `isolated_app`, and `RegisterNatives` binds into it | **[UNVERIFIED]** | test APK: host memfd-writes a tiny dex, passes the fd over Binder, sandbox loads and calls a registered native | T1 covers **native code only**; DEX stays in T0, and the per-app FS isolation story weakens for managed code and must be disclosed |
| U-8 | Dynarmic's A32 coverage of the ARMv7-A/VFPv3-D32/NEON subset real apps use, incl. misaligned access and FPSR accuracy | **HALF-CLOSED** — demand measured ([`32-isa-coverage.md`](32-isa-coverage.md)): 338,034 instructions / 855 mnemonics from real bionic/libm/libc++/zlib ARMv7 code; 8,387 IT-block instructions and hundreds of `f64` VFP ops, so both are mandatory. **Semantic correctness is still unverified** — the file-listing argument in §4.1 was explicitly downgraded to "dispatched, not proven" | Enable `DYNARMIC_TESTS=ON` and run the `arm`/`thumb` generator lanes on the x86_64 emulator; add a mnemonic→handler mapping tool for the presence half | Patch the ops (0BSD allows it) or route unimplemented encodings to `ExceptionRaised` → a per-instruction host helper; **not** to Capstone in the shipped binary |
| U-11 | Hidden-API `Activity.attach` / `AssetManager.addAssetPath` / `HiddenApiBypass` still work at targetSdk 36 / Android 16 | **[UNVERIFIED]**, and it is the *runtime* layer's soft spot | one build on emulator + one OEM device | Designed fallback exists (v1a/v1b, §6); the listing must not promise "any APK" until green |
| U-13 | `execmem` + memfd-exec survive on Samsung/Xiaomi/OPPO/vivo/Huawei | **[UNVERIFIED]** — HIGH only for AOSP `main` **[1L]**, never for OEM overlays | 6-device probe matrix capturing `errno` (apps cannot read `dmesg`) | Fall to Tier I in that process — which is why Tier I is built early (AD-2) |
| U-4 | Android 16 CDD still mandates GLES 1.1, i.e. host GLES1 forwarding exists | **[UNVERIFIED]** | grep the CDD; `dlopen("libGLESv1_CM.so")` + `glGetString(GL_VERSION)` on 5 devices | SwANGLE everywhere for GLES1, losing fps on old fixed-function apps |
| U-5 | RetroBug can select ANGLE per app the way the platform does | **[UNVERIFIED]** | `dlopen("libGLESv2_angle.so")` from an app | Vendor ANGLE/SwANGLE in RetroBug (already needed for T1) |
| U-7 | The AArch32 arm32 variants of `native_bridge_support/android_api` still build on `main` | **[UNVERIFIED]**, and three docs recommend reusing it | build one module in CI, or reimplement the generator against the NDK's 32-bit stub libs | Build guest libs from an `android15-release` tag, or generate the ~200 needed stubs ourselves from NDK stubs + `.map` (Apache-2.0) |
| U-9 | Tier I costs ~3× Tier J rather than 10–20× | **[UNVERIFIED]**, anchored on secondary sources only | IR-ops-per-instrument-on-a-real-`.so`, CoreMark-ARMv7 both tiers | Add an IR→IR fusion/superinstruction pass; lower iOS marketing claims and say so |
| U-12 | Play's dynamic-code clauses do not bar executing user-supplied ARMv7 code | **[UNVERIFIED]**; RetroBug ships zero downloaded code | read current Device-and-Network-Abuse text; compliance question | Sideload/own-site distribution, or a ROM-integrated tier as a separate channel |
| U-1 | (residual) that no better `dlopen` path exists | C1's rejection came from reading ART, not from a device | 20-line test APK, ~1 h | Nothing depends on it; AD-5 already avoids the whole class |
| U-14 | Free `ubuntu-24.04-arm` CI runners expose AArch32 EL0 | **[UNVERIFIED]** | one CI job, static ARM32 hello-world | `qemu-arm` as oracle only; RetroBug's own arm64 tests don't need AArch32 |
| — | **No armeabi-v7a emulator exists on this machine** (x86_64 host, single 16 KiB playstore image with empty `abilist32`) **[1L]** | Hard constraint, not a risk | — | Milestone 1's end-to-end step requires a physical 64-bit-only arm64 phone. Phase 2 must sequence everything else to be independently green before that device is needed |
| — | Dynarmic upstream is archived | **[1A]**, and it is a maintenance commitment we are accepting knowingly | — | AD-14's byte-identical vendoring + `patches/` keeps a future re-import cheap |

### 17.1 What I verified first-hand, so the reader can weight it

Read myself, this session: `private/app.te:189` (`allow appdomain self:process execmem;`),
`private/app_neverallows.te:69-73` (the `execute_no_trans` neverallow), `apk_data_file` exec grants at
`private/app.te:442-443`, Dynarmic's `LICENSE.txt` (0BSD, verbatim), Dynarmic's full source tree via the GitHub API
(the ARMv7-A file evidence in §4.1), Berberis `README.md` first line (riscv64→x86_64, i.e. **not** an ARM32
translator), `lioncash`/`merryhime` repo availability, and this machine's SDK/NDK/CMake/JDK inventory.

**Not** re-verified by me, relied on above and marked **[1A]**: everything from `system/sepolicy`'s isolated-app
domain beyond the three rules quoted, the Apple `pthread_jit_write_protect_np` quote and xnu gating, the ART
`java_vm_ext.cc`/`native_loader_namespace.cpp` line references, the MRA licence text, and `docs/research/08`'s
first-party iOS probes. Each is load-bearing somewhere in this document and each is listed in §17 with its
experiment.

---

## 18. Approval record

Approved by the product owner on 2026-09-19, on the architecture summary rather than the full document:

- **AD-3 approved** — guest managed code executes on the host's native 64-bit ART and only ARMv7 machine code is
  emulated. The owner's wording: this "matches my intent", closing the question of whether running DEX natively
  conflicts with the brief's "not a compatibility shim" constraint. It does not, as the owner reads it.
- **The T0/T1 split approved** as the execution model (§3, §6, §10).

Two additions the owner required before Phase 2, both now in this document rather than deferred:

1. **§10.7 / AD-17** — T1 is entered by a **long-press on the app's entry in the normal library list**, opening a
   contextual per-app option to enter that app's isolated environment. Defined as a required interaction now, with
   the list state model and accessibility alternative, so Phase 2's UI work builds toward the gesture directly.
2. **§12.1–§12.2 / AD-18** — the **App Revival patch system is architecturally independent of T1**, belonging to
   the runtime reproduction layer that both tiers share. A patch (worked example: spoofing "network available" for
   an app with a broken connectivity check that needs no real internet) applies on the **normal T0 path** without
   moving the app into the isolated environment; patches and the isolated environment are separate optional tools
   on an app's profile, usable independently or together; and the mechanism stays data-driven with no APK
   modification, modelled on the owner's own per-game patch system in `docs/research/08` and on Xenia's per-game
   patch shape.

Still pending owner action: approval to **push** commits `a1a2d7b` and `c5e324c` (currently local-only), and the
§17 items that need a physical 64-bit-only arm64 device — most consequentially **U-2** (4 GiB reservation) and
**U-10** (whether an isolated process can load the guest DEX at all), since a U-10 failure restricts T1 to native
code only and must then be disclosed in the UI per §10.5.
