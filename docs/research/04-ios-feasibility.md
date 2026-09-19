# 04 — iOS / Apple Silicon feasibility, and the CPU-core constraints it imposes

RetroBug research track: **iOS target viability + JIT-less core strategy**. Companion documents:
`01-cpu-emulation-armv7-on-arm64.md` (CPU strategy), `02-android-runtime-abi-surface.md` (runtime cut point),
`08-prior-work-local-audit.md` (local prior art: XenDroid iOS port, MoltenVK, Xenia/xemu forks).

Research date: 2026-09-19. Method: primary sources preferred (Apple developer docs, Apple's own BSD/Darwin
man pages, `apple-oss-distributions/xnu` source, project READMEs/issues of UTM, iSH, PPSSPP, box64,
libriscv, MoltenVK, RetroArch). Every claim below is either (a) quoted/paraphrased from a linked source, or
(b) explicitly marked **inference** / **unverified**. Quoted text is in "quotes".

---

## TL;DR

* **iOS will not let RetroBug JIT.** Not "hard" — *not obtainable*: `MAP_JIT` needs an entitlement Apple
  grants only to system processes (historically) and, post-DMA, to **browser engines** in the EU. iSH asked
  Apple under DMA Art. 6(7) for exactly this and was refused with "Apple … does not offer JIT compilation
  for non-browser apps on iOS."
* **Decisive technical detail:** Apple's own porting guide states **"The `pthread_jit_write_protect_np()`
  function isn't available on iOS."** Even the escape hatch for a Darwin JIT (per-thread W^X toggling) is
  macOS-only. So the iOS story is not "JIT with friction", it is "no executable code cache at all".
* **Therefore: the CPU core must be a translator that emits *data*, not machine code**, executed by a
  fast threaded/gadget dispatcher — exactly the design proven by **iSH** (gadgets = array of function
  pointers, "~3-5x compared to … switch dispatch"), **UTM SE** (QEMU's **TCTI** "pseudo-JIT" backend), and
  **PPSSPP** (IR interpreter core switchable with its JIT at runtime). A host-codegen backend stays as an
  *optional* second tier for Android and macOS.
* **App Store policy is survivable** for a "runs user-supplied binaries" app: guideline **4.7** explicitly
  permits "retro game console and PC emulator apps" that "can offer to download games" (April 5, 2024
  update), RetroArch/iSH/UTM SE/PPSSPP all ship, and **2.5.2** targets code that changes *the app's*
  functionality. But 4.7.1–4.7.5 impose content/reporting/index/age duties **if RetroBug offers downloads** —
  the cheap answer is: import user APKs via Files/share-sheet, ship none, say "use your own apps" in the
  listing (PPSSPP's exact wording).
* **The real iOS blockers after the CPU are**: no background CPU (suspension kills the emulated machine),
  jetsam memory ceilings vs a guest heap (mitigated by
  `com.apple.developer.kernel.increased-memory-limit`), 16 KB host pages vs 4 KB guest pages, and no
  `fork`/`execve`/runtime-code-loading.

---

## 1. W^X and code signing on iOS / Darwin

### 1.1 The rule: executable pages must carry a valid code signature

Apple's own framing (macOS-oriented, but the kernel mechanism is shared):

> "The Hardened Runtime capability prohibits the execution of code in a memory page, unless that code is
> accompanied by a valid code signature. Because a JIT compiler doesn't sign the code it generates, it
> technically violates the rules of the Hardened Runtime." —
> [Porting just-in-time compilers to Apple silicon](https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon)

On iOS the same per-page validation is performed by the kernel/code-signing monitor for *every* process —
there is no "opt out of hardened runtime" on iOS. Consequences for RetroBug's memory model:

* Every executed *host* page must be backed by a signature the kernel can validate (a cdhash page table
  inside the Mach-O `LC_CODE_SIGNATURE`), **or** the process must hold a JIT entitlement.
* Anonymous memory produced at runtime is, by construction, unsigned → executable only in a
  kernel-blessed "JIT region".

### 1.2 RW→RX transitions: what ordinary apps can and cannot do

Primary evidence that a normal iOS app cannot get executable anonymous memory, in escalating detail:

1. **Apple's `mmap(2)` man page** ([mirror of the Xcode man page](https://keith.github.io/xcode-man-pages/mmap.2.html)) — the
   single most useful citation for this document, because it describes the *iOS* behaviour explicitly:
   > "**MAP_JIT** — Allow mapping pages both PROT_WRITE and PROT_EXEC when the hardened runtime is enabled.
   > Without this flag an attempt to create a mapping with both PROT_WRITE and PROT_EXEC set will fail with
   > MAP_FAILED on macOS. **A writable, but not executable mapping is returned on iOS, watchOS and tvOS.**
   > Usage of this flag requires the caller to have the `com.apple.security.cs.allow-jit` entitlement on macOS."
   Note the iOS wording: asking for RWX on iOS does not even error — the kernel **silently downgrades you to
   RW**, so a naive JIT "works" until the first instruction fetch traps. RetroBug's Android path must not
   accidentally rely on this shape of bug either.
2. **XNU source** confirms the gate is an entitlement check by the code-signing monitor, not a policy knob:
   [`bsd/kern/kern_mman.c`](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_mman.c) —
   ```
   if ((flags & MAP_JIT) && (vm_map_csm_allow_jit(user_map) != KERN_SUCCESS)) {
           printf("[%d] code signing monitor denies JIT mapping\n", proc_pid(p));
           return EPERM;
   }
   ```
   The same block shows `MAP_JIT` must be `MAP_ANON | MAP_PRIVATE` and may not combine with `MAP_FIXED`,
   `MAP_SHARED`, `MAP_RESILIENT_CODESIGN`, `MAP_RESILIENT_MEDIA`, `MAP_TPRO` (`EINVAL` otherwise).
3. **`mprotect(2)`** on Apple platforms fails with `[EACCES] "The requested protection conflicts with the
   access permissions of the process on the specified address range"`
   ([man page mirror](https://keith.github.io/xcode-man-pages/mprotect.2.html)); there is no documented path from RW to RX
   for anonymous memory in an app without the JIT entitlement. **Inference (high confidence):** any
   "flip the page to X" trick inside RetroBug's cache is dead on iOS; the *only* sanctioned RW→RX story is
   the per-thread mode of a `MAP_JIT` region (1.4).
4. **The historical jailbroken-era loophole is closed.** Saagar Jha's writeup of the "W^X JIT via
   `ptrace(PT_TRACE_ME)`" trick (double-mapping one physical page as RW *and* RX while `CS_DEBUGGED` disables
   signature enforcement) is the best available description of the internals — and of its death:
   [Jailed Just-in-Time Compilation on iOS](https://saagarjha.com/blog/2020/02/23/jailed-just-in-time-compilation-on-ios/)
   (2020-02-23):
   > "Just-in-time compilation on iOS normally requires applications to possess the **dynamic-codesigning**
   > entitlement, a privilege that Apple uniquely awards to system processes that require the high-performance
   > tiers of JavaScriptCore. … While these applications cannot use mmap's MAP_JIT without this entitlement …
   > **Update, 6/24/20 — Preliminary testing on iOS 14 seems to indicate that Apple has changed the kernel so
   > that this trick no longer works.**"
   He also records two constraints that matter for an emulator's address space: non-`dynamic-codesigning`
   apps are denied "jumbo VA spaces" (iOS caps a process's VM by a formula based on physical RAM), and the
   trick required `get-task-allow`, so it was "unfit for the App Store" *by construction*.

### 1.3 `MAP_JIT`: what it is, when it appeared, and for whom

* Introduced by Apple for **JavaScriptCore's JIT on the macOS that shipped with the T2/SSA-era Macs** —
  macOS 10.14 Mojave (2018), same release as the Hardened Runtime; the Apple man pages carrying it are dated
  "May 1, 2020 / Mac OS X 12" ([`pthread_jit_write_protect_np(3)`](https://keith.github.io/xcode-man-pages/pthread_jit_write_protect_np.3.html)),
  and the XNU flag is defined in
  [`bsd/sys/mman.h`](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/mman.h#L127):
  `#define MAP_JIT 0x0800 /* Allocate a region that will be used for JIT purposes */`.
* Semantics (from the two Apple man pages + porting guide):
  * It designates **a JIT arena**: on macOS with the entitlement you may map `PROT_READ|PROT_WRITE|PROT_EXEC`.
  * **Only one** such region per process when the Hardened Runtime + `com.apple.security.cs.allow-jit` are in
    play: "it can only create one memory region with the `MAP_JIT` flag set."
  * On Apple silicon it is **never** simultaneously writable and executable (see 1.4).
  * On **iOS** the flag exists in the headers but is gated by an entitlement third parties cannot get, and the
    per-thread API that makes the region usable is **absent** (Apple's own words, 1.4).

### 1.4 The `pthread_jit_write_protect_np()` programming model — precise contract

This is the part that changes how a JIT's *emitter* must be written, so quote-level accuracy matters.
From [`pthread_jit_write_protect_np(3)`](https://keith.github.io/xcode-man-pages/pthread_jit_write_protect_np.3.html):

> "`pthread_jit_write_protect_supported_np()` … returns whether per-thread write protection on the MAP_JIT
> region is supported on this platform. On platforms where `pthread_jit_write_protect_supported_np()` is true,
> **MAP_JIT regions are never writeable and executable simultaneously**. When write protection is **enabled**
> for the thread, writes by the thread to the MAP_JIT region are denied and the MAP_JIT region **is
> executable**. When write protection is **disabled** for the thread, writes by the thread … are allowed and
> the MAP_JIT region **is not executable**."

and:

> "On platforms where `pthread_jit_write_protect_supported_np()` is false, MAP_JIT regions are simultaneously
> writeable and executable. Calls to `pthread_jit_write_protect_np()` are no-ops on unsupported platforms."

Reading (a) with (b) is the crux: **the same pointer to the code cache means different things depending on
which thread you are on and which mode that thread is in.** Practical rules for a Darwin JIT backend:

1. Every thread that enters the translator must be in *execute* mode (`protect = enabled`); every thread
   that emits must flip to *write* mode, emit, flip back. The mode is **per-thread**, so a codegen worker
   thread that forgets to re-enable protection will silently be unable to *execute* the blocks it just
   built, and a worker that leaves protection off can be blamed for another thread's "impossible" write fault.
2. You cannot hold a pointer into the arena across a mode switch and expect both operations to work; the
   emitter must be a strictly scoped region (`{ write; }` → `{ execute; }`), i.e. an RAII guard, and no
   guest code may run "inside" the emit scope.
3. Apple's preferred hardening API is not the toggle but the **callback**: `pthread_jit_write_with_callback_np(cb, ctx)`
   "disables write protection, calls the supplied callback … and then re-enables write protection", and it
   requires the callback to be enumerated in a per-image static list via `PTHREAD_JIT_WRITE_ALLOW_CALLBACKS_NP`,
   plus `com.apple.security.cs.jit-write-allowlist` (which then **disables** `pthread_jit_write_protect_np()`),
   and `…-freeze-late` if callbacks live in `dlopen()`-ed images. Apple: "Applications and frameworks should
   prefer to use `pthread_jit_write_with_callback_np()` as a defense-in-depth hardening."
   → For RetroBug: if a Darwin JIT tier is ever built, structure emission as "compile into a plain malloc'd
   buffer, then one validated memcpy into the arena inside a write callback." That shape is also what makes
   the Android tier trivial (no callback, just `mmap` + `__builtin___clear_cache`).
4. **I-cache is not coherent**: "Always call `sys_icache_invalidate(_:_:)` before you execute the machine
   instructions on a recently updated memory page. On Apple silicon, the instruction caches aren't coherent
   with data caches." (porting guide). This is also required on Android arm64 (`__builtin___clear_cache`),
   so it belongs behind the same abstraction.
5. **iOS has no toggle at all.** The porting guide's blunt statement — "The `pthread_jit_write_protect_np()`
   function isn't available on iOS" — means RetroBug must not compile a codegen tier into the iOS binary
   (dead code that cannot be entered legitimately), and must not stub it in a way that *silently* degrades
   (see the RW-not-RWX downgrade in 1.2).

---

## 2. Can a normal App Store iOS app get JIT? Distribution-channel matrix

| Channel | JIT (executable code cache)? | Evidence / notes |
|---|---|---|
| **(a) Public App Store (iOS)** | **No.** No obtainable entitlement. | Apple's `com.apple.security.cs.allow-jit` [entitlement page](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.allow-jit) lists availability **`macOS: 10.7.0 -` only** — it is a Hardened-Runtime (Mac) key, not an iOS one. `dynamic-codesigning` is "a privilege that Apple uniquely awards to system processes" ([Saagar Jha](https://saagarjha.com/blog/2020/02/23/jailed-just-in-time-compilation-on-ios/)). iSH/Apple: "Apple … does not offer JIT compilation for **non-browser apps** on iOS" ([ish.app](https://ish.app/blog/ish-jit-and-eu)). |
| **(b) Special-case entitlements** | Effectively **browsers only**, and (post-2023) only for *alternative browser engines*. | iSH: Safari/WebKit is "the first and to date only software on iPhone that is permitted to use JIT compilation technology"; after the DMA changes, "third-party iOS apps will be able to use JIT compilation **if Apple deems them in compliance with their requirements for browsers**", and iSH found even *developing* against the API "requires permission from Apple". The developer-facing keys are the browser-engine family: `com.apple.developer.embedded-web-browser-engine`, `com.apple.developer.web-browser-engine.{host,webcontent,rendering,networking}` ([entitlements index](https://developer.apple.com/documentation/bundleresources/entitlements)). No public criteria document enumerates a "you may have a JIT because you emulate" category. |
| **(c) Developer-signed / sideload (AltStore, SideStore, 3uTools, TrollStore-lite)** | **Yes — with a tether or a pairing file**, and only because the app is dev-signed. | UTM's own support table: `UTM.ipa` (sideload) JIT = "Yes(2)" with footnote "Enabling JIT may require a separate JIT enabler such as StikDebug"; iOS 14.0/14.1/14.4+ requires being "**semi-tethered** … to a Mac/PC (e.g. AltJIT), to another iOS device with Wifi sharing, or to itself (e.g. StikDebug)"; `UTM.SE.ipa` (App Store/enterprise) JIT = **No** ([docs.getutm.app/installation/ios](https://docs.getutm.app/installation/ios/)). AltStore's flow today: create a `.mobiledevicepairing` file with `jitterbugpair` on a Mac/PC, import into StikDebug/StikJIT, then long-press → "Enable JIT" ([AltStore FAQ](https://faq.altstore.io/altstore-classic/enabling-jit/)). Mechanism = the app runs under a debug attach (`get-task-allow` + `CS_DEBUGGED`-style permission), which is precisely why it cannot ship. |
| **(d) Enterprise / ADRP signing** | **No.** | UTM's table lumps "enterprise signing" into the *no-JIT* row for `UTM.SE.ipa`; UTM (JIT) crashes or black-screens when signed with an enterprise certificate: "If the certificate used for signing UTM is listed under … Enterprise App, then it is the wrong certificate." **Inference:** the JIT capability is bound to development signing, not to "not-App-Store". |
| **(e) EU: DMA marketplaces + notarized web distribution** | **No relaxation for emulators.** Browser-engine JIT only; interop request refused. | iSH filed an Article 6(7) interoperability request for the JIT entitlement and got: "After review, we have determined that it does not fall in scope of article 6(7) DMA. Apple does not itself offer emulation functionalities on iOS and it does not offer JIT compilation for non-browser apps on iOS." ([ish.app, 2024-10-16](https://ish.app/blog/ish-jit-and-eu)). Community summary in the UTM-approval thread: "JIT will only be allowed for alternate web browsers in the EU" ([HN 40958465](https://news.ycombinator.com/item?id=40958465)). **Verdict for RetroBug: design the iOS build as if JIT never exists, in every region.** |
| **(f) Jailbreak** | Yes (and hypervisor on M-series iPads). | UTM docs: `UTM.deb` (jailbroken) JIT = Yes, Hypervisor = "Yes(1)" where "(1) Hypervisor on iOS requires an M1 iPad or newer"; `UTM.HV.ipa` (TrollStore) JIT Yes / HV Yes. Not a distribution strategy. |
| **(g) "My Mac (Designed for iPhone/iPad)"** | **Not documented as more permissive; treat as no.** Locally re-signed iOS apps on macOS (PlayCover-style) can carry Mac entitlements — **unverified**. | There is no Apple statement granting iOS binaries `allow-jit` when run through `iosmsc`/iOSSupport. Apple's JIT docs speak only of macOS apps + Hardened Runtime. Because the iOS dyld shared cache lacks `pthread_jit_write_protect_np`, an iOS binary on macOS would also lack the sanctioned way to write its arena. → See Open questions. |
| **(h) Mac-native / Mac Catalyst build of the same core** | **Yes** — this is the *real* Apple-Silicon story. | `com.apple.security.cs.allow-jit` is a normal, self-declared Hardened-Runtime entitlement for Mac apps ("under Runtime Exceptions, select **Allow Execution of JIT-compiled Code**"), i.e. Mac App Store and direct-distribution builds may ship a JIT (that is how CrossOver/UTM/Parallels work on the Mac). Requirement: single `MAP_JIT` arena, per-thread write toggle or write-callback allowlist, `sys_icache_invalidate`. |

### 2.1 The "JIT via JavaScriptCore trick" — status

The trick people cite: obtain executable pages *indirectly* by making Apple's own blessed JIT (JavaScriptCore
inside WebKit/WKWebView, or an `ArrayBuffer`/`WeakMap` allocation inside a JIT tier) hand you memory you then
overwrite — i.e. borrow a WebContent process's blessing or a JSC-allocated arena.

* **Baseline (still true, and the reason the trick existed):** in-process JavaScriptCore in a third-party app
  has its JIT disabled — "JavaScriptCore runs in-process and consequentially **the JIT is disabled**, because
  Apple doesn't allow non-Apple processes to write to executable code memory. … WKWebView … runs in another
  sandboxed process (Safari, blessed by Apple)" ([HN 30696492](https://news.ycombinator.com/item?id=30696492));
  corroborated by iSH: WebKit is the only iOS software permitted to JIT.
* **Current status: not usable and not shippable.** I found **no** evidence of a working JSC-executable-memory
  trick on iOS 17/18/26 and the emulation-community position is that sideloaded-JIT-with-a-computer is *the*
  method now (UTM/AltStore docs above, 2024–2026). The kernel path that made "borrowed" memory possible for
  jailed apps was the `CS_DEBUGGED`/ptrace hole, and that was closed in iOS 14 (Saagar Jha update).
  **Inference + policy note:** even where a variant worked, it (i) relies on private behaviour (guideline
  **2.5.1** "Apps may only use public APIs"), (ii) writes into pages owned by another engine (Apple would treat
  as a security bypass), and (iii) is exactly the kind of thing Apple patches. **Do not design RetroBug around
  it; do not ship it.** Treat any such technique as a jailbreak/sideload-only debug feature at most.
  Marked **unverified** below because I could not find an authoritative "as of iOS X it is dead" statement.

### 2.2 `dlopen` / library loading, and what it means for RetroBug's system libraries

* iOS apps **may** embed dynamic frameworks *inside the bundle* (signed, `Frameworks/`), and may `dlopen`
  those. They **may not** load code from outside the bundle, and **may not** download code at runtime —
  2.5.2's "self-contained in their bundles … may not … download, install, or execute code" (quoted in §4) plus
  the signature-validation rule of §1.1 make it fail on both policy and mechanism. Apple's Darwin `dyld`
  behaviour is the enforcement point: an unsigned/foreign-team Mach-O fails with "code signature invalid"
  ([representative report](https://stackoverflow.com/questions/60384835/dyld-library-not-loaded-code-signature-invalid));
  and Apple's JIT docs note by default "JIT write callbacks are not allowed in frameworks/dylibs dynamically
  loaded at runtime via `dlopen(3)`" — dynamic loading is a second-class citizen even on macOS.
* **Consequences for RetroBug (decisive):**
  1. RetroBug's core, its decoder, its IR VM and every host backend must be **statically linked into the one
     app binary** (`.a`/`.o` in the executable). No "plugin core" `.dylib`/`.framework` fetched at runtime.
  2. The *guest* system libraries (`libc.so`, `libGLESv2_cm.so`, `libandroid.so`, `libOpenSLES.so`, guest
     `lib/armeabi-v7a/*.so`, `classes.dex`, `resources.arsc`, `boot.oat` …) are **data files in the container**,
     parsed by RetroBug's own guest ELF loader — never handed to `dlopen`. (Doc 02's "shadow `.so` generated per
     guest library" plan is Android-only: on iOS a *host* generated `.so` is unloadable code, so the JNI entry
     must be a symbol inside the main binary reached through a table, not a dylib.)
  3. Anything RetroBug generates at runtime — translated blocks, IR, per-app profiles, patch scripts — must
     live in **non-executable** storage or in the single blessed arena. Never write a Mach-O and load it.
  4. ART/DEX: on iOS there is no host ART (doc 02's cut point is Android-specific), so iOS requires RetroBug's
     own managed-code tier (or a documented "Android-runtime layer" that interprets DEX). That is a *policy
     non-issue* (a bytecode interpreter is what iSH/RetroArch/Delta/PPSSPP all ship) but it is a scope issue —
     see Open questions.

---

## 3. JIT-less: what actually works, and how fast

### 3.1 UTM SE — QEMU on iOS without a JIT

* **What it is.** [UTM README](https://github.com/utmapp/UTM): "UTM/QEMU requires dynamic code generation (JIT)
  for maximum performance. JIT on iOS devices require either a jailbroken device, or one of the various
  workarounds … UTM SE ('slow edition') **uses a threaded interpreter** which performs better than a
  traditional interpreter but still slower than JIT. **This technique is similar to what iSH does** for
  dynamic execution." Included guest arches: "ARM, PPC, RISC-V, and x8" (32- and 64-bit) — i.e. an
  **ARMv7-class guest already runs on iOS with no JIT**.
* **The exact mechanism (the brief's `asm-threads` question).** It is *not* `-accel mttcg` (that is a TCG
  threading mode, orthogonal), and it is not the 2020 `qemu-ios` "asm-threads" line either. UTM SE carries a
  TCG backend from the **`tctiSH/qemu`** fork — description: "A fork of QEMU that plays host to the
  **TCTI pseudo-JIT**"; default branch `with_tcti_vectors`, containing `tcg/aarch64-tcti/` (with a
  `tcti-gadget-gen.py` gadget generator) alongside upstream `tcg/tci`. Its
  [README](https://github.com/tctiSH/qemu/blob/with_tcti_vectors/tcg/aarch64-tcti/README.md) is the cleanest
  published description of the technique:
  > "A TCG backend that **chains together JOP/ROP-ish gadgets to massively reduce interpreter overhead vs
  > TCI**. … usable when JIT isn't available; e.g. **on platforms that lack WX mappings**. The general idea
  > squish the addresses of a gadget sequence into a 'queue' and then write each gadget so it ends in a
  > 'dequeue-jump'. Execution occurs by jumping into the first gadget, and letting it just play back some
  > linear-overhead native code sequences for a while."
  plus a hard register convention (x1–x15 = guest registers, x28 = thread-stream pointer) and a per-gadget
  epilogue that loads the next gadget address and branches to it. **Read that as: the translated "block" is
  pure data (an array of code-address words), the gadgets are pre-built, signed, read-only host code.**
* **Performance, as reported.** Apple-approved and shipped 2024-07 ([HN](https://news.ycombinator.com/item?id=40958465),
  [UTM SE listing](https://apps.apple.com/us/app/utm-se-retro-pc-emulator/id1564628856) — v4.7.5, iOS 14.0+).
  UTM's own docs: "UTM SE is a stripped down version of UTM which does not support JIT and uses a **slower
  interpreter** … can be used to run older operating systems such as DOS or stripped down operating systems
  such as Alpine Linux"; and "UTM SE does not support JIT (**will run much slower**) but is compatible with
  all signing methods." Community verdict in the approval thread: "It's the SE 'Slow Edition' … so this is all
  interpreted and barely usable" — i.e. *OS-booting* class workloads are feasible in minutes-to-tens-of-minutes;
  interactive x86 Windows is not. Treat "barely usable" as the reputation you must not earn.
* **Maintenance cost of the fork.** UTM's interpreter is a *fork of a fork* (QEMU is GPL-2.0 — for RetroBug
  that is a study-only boundary, per doc 01). The cost shows up in [UTM discussion #7694 "High performance
  interpreter for UTM SE"](https://github.com/utmapp/UTM/discussions/7694) (2026-04-28), where a user proposes
  swapping TCI for a libriscv-class interpreter and maintainer `osy` replies: "I've had an idea to use the new
  webasm backend for QEMU but **like any change it'll take a lot of time and effort to develop**. Feel free to
  try it yourself and send a PR if it works." Re-basing a custom TCG backend against upstream TCG churn is the
  recurring tax; note the fork is still alive (pushed 2026-09-15) and its branch is literally
  `with_tcti_vectors`, i.e. vector support is an ongoing patch series.

### 3.2 iSH — the closest architectural cousin, and its honest numbers

[iSH README](https://github.com/ish-app/ish) (line 65) is the single best first-party description of a
production **data-form** translator on iOS:

> "Possibly the most interesting thing I wrote as part of iSH is the interpreter. **It's not quite a JIT since
> it doesn't target machine code. Instead it generates an array of pointers to functions called gadgets, and
> each gadget ends with a tailcall to the next function; like the threaded code technique used by some Forth
> interpreters. The result is a speedup of roughly 3-5x compared to emulation using a simpler switch
> dispatch.**"

And the maintenance lesson (line 67): "Unfortunately, I made the decision to write nearly all of the gadgets
in assembly language. This was probably a good decision with regards to performance … but a horrible decision
with regards to readability, maintainability, and my sanity … there are almost no comments." → **RetroBug
rule: generate the gadget table and per-gadget code from a machine-readable spec, keep gadgets small and
compiler-generated, never hand-write hundreds of asm snippets.**

* iSH ships on the App Store ([listing](https://apps.apple.com/us/app/ish-shell/id1436902243), v1.3.2) — proof
  Apple accepts an app whose entire purpose is running user-supplied x86 Linux binaries.
* Performance: iSH is a 32-bit x86 Linux userland on an interpreter; users describe it as "ok, not great", and
  iSH's own JIT post says a real JIT "would significantly improve the performance of iSH — **in some cases, by
  an order of magnitude**". That is the best available first-party **JIT-vs-threaded-interpreter delta for a
  32-bit ISA on Apple silicon: ~10×**.
* **Self-modifying / mprotect semantics.** iSH cannot use host page protections at guest-page granularity, and
  iOS cannot give a JIT arena; it therefore tracks guest permissions **in software** (a page table of guest
  PTE flags) and re-validates translated blocks on writes. **Inference (high confidence, verify in code):**
  the same must be true for RetroBug — see §6. **Also relevant**: iOS/Darwin signal handling is Mach-exception
  based and a SIGSEGV from inside an emulated fault path is hostile to resume cleanly (the reason the ptrace
  JIT trick needed `PT_SIGEXC` + an `exc_server` Mach handler, per Saagar Jha); Apple Community/DTS threads
  confirm EXC_BAD_ACCESS interception in-process is not supported behaviour for app code
  ([example](https://discussions.apple.com/thread/2748961)). Design consequence: **never make a host memory
  fault the primary mechanism of guest semantics** (see §6 requirement about guard pages).

### 3.3 Console emulators that *do* ship on the App Store (all JIT-less)

* **PPSSPP (PSP) — on the App Store, officially.** [Listing](https://apps.apple.com/us/app/ppsspp-psp-emulator/id6496972903):
  "Play PSP games on your iOS device, in high definition … It can run nearly all PSP games with **great
  performance on iOS**. **No games are included with this download. Use your own real PSP games** and turn them
  into .ISO or .CSO files". Mechanism: PPSSPP's CPU is already two-tier — upstream docs describe the normal
  path as "translates PSP CPU instructions directly into optimized x86, x64, ARM, ARM64 or RISC-V machine
  code, using JIT recompilers (dynarecs)" ([ppsspp.org](http://dev.ppsspp.org/)), but the project also has an
  **IR interpreter core** (announced 2016: "Latest PPSSPP build adds **IR Interpreter CPU core**"
  ([thread](https://www.reddit.com/r/jailbreak/comments/4kcphk/))), i.e. AltiVec-free *translated PSP code in
  PPSSPP's own IR*, interpreted — and the two are switchable at runtime, as evidenced by
  [issue #20502 "Switching from IR Interpreter to JIT in-game crashes"](https://github.com/hrydgard/ppsspp/issues/20502).
  Older iOS evidence of the JIT side: "[WIP] iOS AltKit support for JIT", "iOS: Should fail gracefully when JIT
  is not available", "[iOS] Wipeout Pure lag" (slow without JIT) — GitHub search
  `repo:hrydgard/ppsspp ios jit`. **This is the precedent to copy: one front-end, an IR that is *data*, and a
  runtime-selectable IR-interpreter backend that is the *only* backend on iOS.**
* **RetroArch** — on the App Store ([listing](https://apps.apple.com/us/app/retroarch/id6499539433), seller
  "Libretro", v1.22.2, iOS 12.0+): cores are **statically compiled in** (no core downloads at runtime, which is
  the `dlopen`/2.5.2 problem of §2.2) and the cores that matter on iOS are the **interpreter/hand-written**
  ones; its iOS build and settings surface ship with the dynarec disabled where a core has one. **Inference**
  (from the store build's existence + §2.1/§4 rules) — I did not find a first-party RetroArch statement
  quantifying iOS core speed; see Open questions.
* **Delta / GBARunner-class handheld cores**: the App Store does carry handheld emulators (e.g. GBA/DS cores)
  whose CPU models are **interpreters**; full-speed GBA/DS on A-series silicon is routinely achieved because
  those guests are ~16 MHz-class. RetroBug's guest (32-bit Android on a 1.5 GHz A32 CPU with real threads,
  OpenGL ES calls, and a Dalvik/ART VM under it) is **orders of magnitude more work** than a GBA core —
  calibrate expectations accordingly. (I could not verify a first-party statement from Delta's author on the
  store build; an App Store "Delta – Game Emulator" entry appears under seller "Yvette Testut", which looks
  like a copycat. Treat as **unverified**.)
* **DuckStation / Flycast / DolphiN?** (DolphiniOS, OatmealDome's Dolphin fork): distributed **off** the App
  Store via AltStore/SideStore, and their performance depends on **JIT enabled by tether/pairing** — the same
  model as UTM (see §2 row (c)); DolphiniOS ships "no-JIT" experimental builds that reviewers/threads describe
  as far too slow for GameCube. **Inference from ecosystem positioning, not from a benchmark** — I found only
  video/forum-level evidence ([example](https://www.youtube.com/watch?v=NwSJ_P48q7A)); recorded as unverified.

### 3.4 What the measured data says about "interpreter vs JIT" for a 32-bit RISC-ish ISA

| Data point | Ratio | Source |
|---|---|---|
| iSH threaded gadgets vs plain `switch` dispatch | **3–5× faster** (same no-JIT constraint) | [iSH README](https://github.com/ish-app/ish) |
| Modern libriscv-style interpreter vs QEMU TCI | **~10× faster**; and "about **1/3 the performance of QEMU's JIT**" | [UTM discussion #7694](https://github.com/utmapp/UTM/discussions/7694); [libriscv README](https://github.com/fwsGonzo/libriscv) ("interpreter mode … substantially faster than other interpreters", CoreMark/STREAM charts, plus an optional binary-translation mode) |
| iSH with a hypothetical real JIT vs shipping iSH | **"order of magnitude"** | [ish.app/blog/ish-jit-and-eu](https://ish.app/blog/ish-jit-and-eu) |
| A hand-written *direct* DBT PoC vs QEMU's IR-based TCG (JIT!) | **up to 35×** — IR layering itself is expensive | A. I. Parker, ["Boosting Cross-Architectural Emulation Performance by Foregoing the Intermediate Representation Model"](https://arxiv.org/abs/2501.03427) (arXiv:2501.03427, 2025-01-06) |
| box64: `BOX64_DYNAREC=0` (interpreter) vs `=1` | "Dynarec" is the whole selling point; interpreter mode exists as a correctness/fallback tier | [box64 USAGE.md](https://github.com/ptitSeb/box64/blob/main/docs/USAGE.md) |

**Reading for RetroBug (the crux).** Best-case threaded/gadget interpreter ≈ **1/3 of a good JIT**, i.e.
**~3× slower than a JIT and maybe ~5–10× slower than native arm64** for an A32 guest, *if* the interpreter is
written to libriscv/TCTI standards. Two non-obvious implications:

1. **The 35× result is a warning about IR design, not about interpreters.** QEMU's TCG is slow partly because
   its IR is a generic, many-operand, host-agnostic op stream that then gets another lowering pass. A
   guest-specific "macro-op" stream that (a) keeps A32 semantics (writeback, condition codes, IT blocks,
   shifts-with-carry) folded into the op, (b) keeps operand fields at fixed byte offsets, and (c) dispatches by
   direct-threaded jump table, avoids the layering tax. iSH and TCTI both do (a)+(c); libriscv argues for
   "a 3-register-machine architecture, which makes it very easy to reach good interpreter performance without
   needing a register allocator".
2. **Two-tier, data-first, code-later is a real, shipped pattern**, not a theory: PPSSPP (IR interpreter ↔
   JIT switchable), TCTI (QEMU TCG frontend → data instead of host code), iSH (gadgets), FEX/box64
   (interpreter + JIT backends over an IR; **UCF/dynarec_lightly specifics unverified in this pass**),
   Dynarmic (A32 frontend with pluggable backends, 0BSD — API shape to be confirmed). RetroBug's "translator
   emits an IR; tiers = {gadget-threaded, host-codegen}" is exactly this family, and the iOS constraint picks
   which tier is mandatory.

### 3.5 Apple's own precedents (context only)

Rosetta 2 is Apple's x86-64→AArch64 DBT: it translates at install time into on-disk artifacts that the kernel
validates, and additionally JITs — available **only** because Apple ships it and it is exempt from the rules in
§1. The AArch32 story is the mirror image: Apple silicon has **no AArch32 EL0**, so nothing about iOS runs
32-bit ARM natively. Both facts reinforce the same architectural lesson RetroBug should take: **translation is
fine; *unblessed* executable memory is the thing that is not allowed.** (Also see doc 01 §1 on hardware
AArch32 being absent on all Apple silicon and on newest Cortex cores.)

---

## 4. App Store policy: emulators, interpreters, and user-supplied APKs

Current text ([App Review Guidelines](https://developer.apple.com/app-store/review/guidelines/), retrieved
2026-09 via archive):

* **2.5.1** — "Apps may only use public APIs and must run on the currently shipping OS. … phase out any
  deprecated features, frameworks or technologies that will no longer be supported." → kills `ptrace`,
  private `vm_map`/`host_*` calls, and any "JSC trick"; also relevant to OpenGL ES (see §5.1).
* **2.5.2** — "Apps should be **self-contained in their bundles**, and may not read or write data outside the
  designated container area, nor may they **download, install, or execute code which introduces or changes
  features or functionality of the app**, including other apps. Educational apps designed to teach, develop, or
  allow students to test executable code may, in limited circumstances, download code provided that such code
  is not used for other purposes. Such apps must make the source code … completely viewable and editable."
* **2.5.4** — "Multitasking apps may only use background services for their intended purposes: **VoIP, audio
  playback, location, task completion, local notifications**, etc." (→ §5.4: don't fake background audio).
* **4.2 Minimum Functionality** — "Your app should include features, content, and UI that elevate it beyond a
  repackaged website. If your app is not particularly useful, unique, or 'app-like,' it doesn't belong on the
  App Store. If your App doesn't provide some sort of lasting entertainment value or adequate utility…"
  Also 4.2 (power): "Design your app to use power efficiently and be used in a way that does not risk damage
  to the device. Apps should not rapidly drain battery, generate excessive heat, or put unnecessary strain on
  device resources." **A CPU pegged at 100 % on an iPhone is a review surface.**
* **4.7 (the emulator clause, as of the 2024-04-05 update)** — title: "**Mini apps, mini games, streaming
  games, chatbots, plug-ins, and game emulators**": "Apps may offer certain software that is not embedded in
  the binary, specifically HTML5 and JavaScript mini apps and mini games, streaming games, chatbots, and
  plug-ins. **Additionally, retro game console and PC emulator apps can offer to download games.** You are
  responsible for all such software offered in your app, including ensuring that such software complies with
  these Guidelines and all applicable laws."
  Apple's news post: "**4.7: Added games from retro game console emulator apps to the list of permitted
  software**, and clarifies that mini apps and mini games must be HTML5"
  ([developer.apple.com/news/?id=0kjli9o1](https://developer.apple.com/news/?id=0kjli9o1), 2024-04-05).
  Conditions 4.7.1–4.7.5 (quoted): offered software must follow privacy guidelines incl. 5.1; "include a method
  for **filtering objectionable material, a mechanism to report content and timely responses** … and the ability
  to **block abusive users**"; **4.7.2** "Your app **may not extend or expose native platform APIs** or
  technologies to the software without prior permission from Apple"; **4.7.3** no data/permission sharing
  without per-item consent; **4.7.4** "You must provide an **index of software** and metadata available in your
  app … including **universal links**"; **4.7.5** age-rating/age-restriction mechanism.
* **2.5.7** is currently "**Intentionally omitted**" (it is the old "no interpreted code" ancestor; the
  surviving rule is 2.5.2), and **2.5.3** forbids transmitting code that harms the device/OS.
* **5.1.1 / privacy**: standard data-collection disclosure + App Privacy nutrition labels; an APK importer that
  reads nothing but user-chosen files and phones home nothing labels cleanly, but any telemetry about installed
  APK lists is user-content collection and must be declared.

### 4.1 Is "an app that ships a bytecode interpreter" allowed? — Yes, demonstrably

The interpretation of 2.5.2 that matters: it targets **app self-updating**, not user content. Saagar Jha's
analysis (written from inside iSH's review fights) is the best public account:
["Fixing Section 2.5.2"](https://saagarjha.com/blog/2020/11/08/fixing-section-2-5-2/) —
"the backend is usually some sort of Turing-complete interpreter … these apps do not actually have the issue
that the guideline was meant to solve: the app itself—neither the frontend nor the backend—changes, and scripts
are user- and not developer-generated"; "Our own application, iSH, is a scripting application: **it interprets
x86 scripts**." But also the risk in practice: iSH "was once rejected with the rationale that '**During review,
your app installed or launched executable code, which is not permitted on the App Store**'", and reviewers
repeatedly "installed code and then complained that the app did what they told it to do" (they used `wget` to
restore a package manager). **Lesson for RetroBug: the danger is not the letter of 2.5.2, it is a reviewer
hitting an in-app downloader and screenshotting it.** App Review also still removed-and-restored iSH over this
([ish.app/blog/app-store-removal](https://ish.app/blog/app-store-removal)).

### 4.2 Does "runs user-supplied APKs" violate anything? — Position

* **Mechanically, no:** on iOS RetroBug never *installs* an APK; it maps a user-chosen file into its own
  sandbox and interprets its ARM code. No unsigned page is ever executed (§1 protects you: you *can't*), and no
  code changes RetroBug's functionality. Same shape as iSH (user binaries), UTM SE (user disk images),
  RetroArch (user ROMs), Delta, PPSSPP.
* **Policy-wise, the safe configuration (recommended):**
  * Import only through **Share Sheet / Files / document picker**; **ship zero APKs**; no in-app catalog, no
    URL-fetch-and-install, no "core"/"compat pack" downloads that contain code (translations/profiles may be
    data-only and should be versioned inside the binary or reviewed as content).
  * Listing text mirroring PPSSPP: "No apps are included with this download. Use your own apps."
  * If RetroBug *does* offer downloads (game APKs), it is squarely inside **4.7**: it must implement 4.7.1
    filtering/reporting/blocking, 4.7.2 (no extending native APIs to that software without permission — note
    this is *about the download channel*, not about RetroBug's own JNI bridge), 4.7.4 index with universal
    links, 4.7.5 age gating. **Cost/benefit: don't offer downloads on iOS v1.**
  * 4.2 framing: sell it as a *player/compatibility layer* with real UI (per-app profiles, patches, save
    management, controller support), not as "a loader".
* **Precedent for "Pro outside the store":** UTM (JIT) lives on AltStore/jailbreak while UTM SE lives on the
  App Store; Delta/PPSSPP ship "free + gold/paid" variants; RetroArch ships "Pro" outside Google Play on
  Android. **A "RetroBug Pro (AltStore)" with the Darwin JIT tier and an App Store "RetroBug" with the IR tier
  is an established, honest pattern** — but be aware 2.5.2/3.1.1 sensitivity about steering users to other
  purchase/distribution channels; consult current rules for wording.

### 4.3 The Android-app-inside-iOS-app legal question

* **Copyright/DRM.** An APK is a copyrighted work; modern Play apps are additionally protected by Play
  Install Guard/Play Protect and by LVL/`SafetyNet`-style attestation. RetroBug itself is a *tool*
  (Sony v. Connectix / Apple v. Oracle fair-use lineage for interoperable emulators/reverse engineering —
  **legal analysis is out of scope here and needs counsel**), but two concrete engineering-facing facts:
  1. **Do not ship APKs.** All shipped emulator precedents put the burden on the user ("No games are
     included… use your own real PSP games", PPSSPP listing; UTM SE's guest images are user-supplied or
     freely-redistributable). RetroBug must refuse to bundle *any* third-party APK, including "abandonware".
  2. **Do not circumvent.** Doc 02 flags that Google's 32-bit-drop is an *environmental* fact, not a DRM
     measure; RetroBug loading a legacy 32-bit APK the user already owns is not a Technological Protection
     Measure bypass. The line is crossed the moment RetroBug bypasses Play licensing, re-signs/patches for
     activation, or unpacks install-guarded binaries. "App Revival Patches" (README feature list) must be
     compatibility patches, never license checks removed.
* **Trademarks.** "Runs your old Android apps" is fine; using app names/logos in a catalog is not.
* **Google Play Protect** is irrelevant on iOS, but a *RetroBug-branded APK sideloaded on Android* would trip
  Play Protect warnings for unknown sources — a UX/documentation problem for the Android host, not a legal one.
* **Sideloadly/AltStore analogue:** the AltStore distribution of a JIT RetroBug relies on the user's own
  developer certificate (Apple's own signing path), which is the accepted community mechanism (see §2 (c)) —
  the same posture as UTM/DolphiniOS/PPSSPP-iOS.

---

## 5. Platform glue required on iOS

### 5.1 Graphics

* **Host APIs.** Metal is the only forward-supported path. **MoltenVK** is "a layered implementation of
  **Vulkan 1.4** … built on Apple's Metal … on macOS, iOS, tvOS, and visionOS", explicitly "does not use any
  private or undocumented API calls or features, so your app will be compatible with all standard
  distributions" ([MoltenVK README](https://github.com/KhronosGroup/MoltenVK/blob/master/README.md)) →
  App-Store-safe; the XenDroid tree already vendors a MoltenVK copy
  (`xaniteOG/XenDroid/emulator-core/.../third_party/MoltenVK/`, per doc 08).
* **Guest GLES mapping on iOS.** Apple documents OpenGL ES as *present on iOS*: "The OpenGL ES framework
  (`OpenGLES.framework`) in iOS provides implementations of **versions 1.1, 2.0, and 3.0** of the OpenGL ES
  specification" (EAGL contexts), while the framework's doc availability range is **iOS 2.0 – 12.0**, i.e.
  deprecated-and-frozen ([Apple OpenGL ES page](https://developer.apple.com/documentation/opengles);
  [iOS 12 deprecation note](https://stackoverflow.com/questions/52405665/opengl-deprecated-in-ios-12-how-to-safely-continue-to-use-opengl-on-ios-12-dev)).
  Two consequences: (i) RetroBug's **guest GLES 1.x/2.0/3.0** surface can, on iOS, be satisfied by a
  **host GLES shim** (a real option that does not exist on macOS, whose desktop GL stops at 4.1 and whose
  Catalyst/AppKit path has no iOS EAGL); (ii) it is a deprecated, 2.5.1-flaggable dependency — so **the GLES
  mapping must be a backend, not the architecture.** Preferred: translate guest GLES → RetroBug's own
  **hardware-abstraction command stream** → { Metal (native), MoltenVK, host GLES/EGL shim (iOS-only),
  Android GLES }. Guest GLES 3.2+ features do not exist on iOS (ES 3.0 max documented), so a guest-ES-3.1/3.2
  path must fall back to Metal compute or refuse.
* **Presentation:** `CAMetalLayer` (+ `MTKView` at most) for the guest framebuffer; `CADisplayLink` for pacing;
  retain a "software framebuffer" path for boot/headless and for guest drivers that never come up — UTM's
  "VGA mode / text mode" split in the SE listing is the model.

### 5.2 Audio

* No OpenSL ES / AAudio on iOS: RetroBug's guest-side `libOpenSLES.so`, `AudioTrack`/`AudioRecord` and the
  Java `Media*` classes are its **own reimplementation** (doc 02's cut), so the *host* sink must be Core Audio.
  Practical contract: `AURenderCallback`/`AVAudioEngine` + `AVAudioSession` (`.playback` category,
  `mixWithOthers`, and the hard real-time budget on the I/O thread — allocate and pre-decode; **never** take a
  lock the translator can hold, which also matters because of the Darwin JIT-mode rule in §1.4(2)).
* Latency: iOS audio is *better* than Android's (10–20 ms achievable), so guest audio should be resampled to
  the device's native sample rate/frame size, and buffers should be ping-pong over a **non-executable** ring.
* `AVAudioEngine`/audio playback is also the only mainstream way to keep running in background — with policy
  risk (§5.4) and guideline 2.5.4's "for their intended purposes".

### 5.3 Input

* `UIResponder`/`UIGestureRecognizer` multi-touch → guest MotionEvent injection; `GCController`
  (Game Controller framework) for MFi/Xbox/PS pads → guest `InputDevice`/keycode map; haptics via
  `CHHapticEngine`; `UIActivityViewController`/document picker for APK import; on iPads, `UIKeyCommand` +
  external keyboard (the "keyboard addition" that iSH built a whole UI around). Mice/pointers: `UIPointerInteraction`
  (iPadOS pointer) → guest touch events; note UTM needed mouse support work for SE's usability.
* **Never** block the main thread waiting for input; the guest's `Looper` must run on a RetroBug thread with the
  main thread reserved for UIKit/Metal presentation.

### 5.4 Filesystem, process model, lifecycle, memory — the actual blockers

* **Sandbox/paths.** Only the app container (`NSDocumentDirectory`, `Library/Caches`, `tmp/`) is writable; no
  `/data/data/...`, no `/sdcard`, no shared storage, no writing into the bundle after signing (invalidates the
  signature). RetroBug's per-app guest environment maps to a container subdirectory (doc 02's "isolated per-app
  environment" is already the right shape) and guest paths (`/system`, `/data`, `/sdcard`) are **virtual
  prefixes** resolved by the guest VFS.
* **No `fork`, no `execve`, no child processes.** iOS apps cannot spawn processes (Apple forums on
  `fork()`: ["fork" is no-no but why?](https://developer.apple.com/forums/thread/747499); the community
  restatement of "iOS … we can't spawn new processes" is in Saagar Jha's post). Consequences: Android's
  multi-process app model (`Zygote`, `:remote` services, `isolatedProcess`) has **no host analogue** —
  RetroBug must emulate extra guest processes as **extra threads with separate address-space views**
  (its own page tables + its own guest PID namespace), which is another reason guest memory protection must be
  tracked in software, not by host `mprotect`. `posix_spawn` is not an option for third-party apps.
* **Suspension.** Apple TN2277 is the canonical statement: "When iOS puts your app into the background, it may
  shortly thereafter suspend the app. **When the app is suspended, no code within the app's process runs**"
  ([TN2277](https://developer.apple.com/library/archive/technotes/tn2277/_index.html)). So: a guest Android app
  does not get to keep running when the user leaves RetroBug. There is no legitimate long-running "CPU
  emulator" background mode; `audio`/`VoIP` background modes for a non-audio use are a 2.5.4 violation and a
  removal risk. Required design: (i) on `applicationWillResignActive` → **quiesce the CPU** at a safe
  instruction boundary, (ii) on `didEnterBackground` → **write a full machine save-state** (guest RAM dirty map
  + registers + guest FD/table state + timer/clock offsets) within the ~few-second background task window,
  (iii) on foreground → restore, and freeze guest wall-clock-dependent logic during the gap (guest
  `SystemClock`/`uptimeMillis` virtualisation, watchdog/ANR suppression on resume). Save-states are also the
  only sane recovery path from jetsam kills.
* **Jetsam / memory ceilings.** Guest Android wants a big heap + a big code/IR cache + guest GL textures, on an
  app whose footprint is already the emulator. iOS caps app resident memory per device and kills silently;
  `os_proc_available_memory()` is the query, and Apple documents a **declarable** entitlement that raises the
  ceiling: `com.apple.developer.kernel.increased-memory-limit` — "**iOS 15.0+** … inform the system that some of
  your app's core features may perform better by exceeding the default app memory limit on supported devices …
  An increased memory limit is only available on some device models … Higher memory use can affect system
  performance" ([entitlement doc](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.increased-memory-limit)).
  RetroBug must therefore have a **hard, budgeted arena layout** (guest RAM, IR/TB arena, GL staging) with
  eviction, and must degrade when the grant is not honoured. (Related: §1.2's note that non-`dynamic-codesigning`
  apps get a *capped VA space* — a 4 GB guest address space plus caches must be planned against that, not
  assumed.)
* **Build/deployment reality:** App Store submissions must currently be built with **Xcode 26 / iOS 26 SDK**
  ("Since April 28, 2026", [Upcoming Requirements](https://developer.apple.com/news/upcoming-requirements/));
  realistic floor for an iOS RetroBug is **iOS 14+** (UTM SE's own floor) or iOS 15+ if it wants
  `increased-memory-limit`.

---

## Verdict — the iOS JIT question

> **RetroBug on iOS cannot use a JIT, and this is not a solvable problem — design for it.**
>
> 1. **On iOS, no ordinary app may obtain an executable code cache.** The kernel's code-signing monitor
>    refuses `MAP_JIT` unless blessed (`vm_map_csm_allow_jit` → EPERM), the entitlement that grants it
>    (`dynamic-codesigning`, surfaced on macOS as `com.apple.security.cs.allow-jit`, whose documented
>    availability is **macOS only**) is not offered to iOS apps, and — the clincher — Apple states
>    "**`pthread_jit_write_protect_np()` isn't available on iOS**", so the sanctioned Darwin programming model
>    for a JIT region does not exist there. Asking for RWX on iOS silently yields **RW**, so "it worked in the
>    simulator" is not evidence.
> 2. **Every loophole is either closed or non-shippable.** `ptrace(PT_TRACE_ME)` W^X-JIT: patched in iOS 14 and
>    needed `get-task-allow` (never App Store). The JSC trick: in-process JavaScriptCore has no JIT by policy,
>    and I found no evidence of a working variant on current iOS. Sideloaded JIT (AltStore/StikJIT/AltJIT) works
>    for dev-signed apps **with a tether or pairing file** — fine for a "RetroBug Pro" outside the store, not an
>    App Store product.
> 3. **The EU does not change this.** Apple's DMA JIT APIs are for **browser engines**; iSH's Art. 6(7)
>    interoperability request for the JIT entitlement was **refused** with "Apple … does not offer JIT
>    compilation for non-browser apps on iOS". Enterprise/ADRP signing is *more* restrictive, not less (UTM
>    treats enterprise-signed builds as no-JIT).
> 4. **Policy is the smaller problem.** Emulators and interpreter apps ship (UTM SE, RetroArch, iSH, PPSSPP,
>    Delta-class), 4.7 explicitly blesses emulator apps offering to download games, and 2.5.2's target is app
>    self-modification, not user content. Expect reviewer friction and mitigate by shipping zero APKs and no
>    in-app downloader.
> 5. **macOS is the opposite.** A Mac-native/Catalyst build can take `com.apple.security.cs.allow-jit` and run a
>    real A64 JIT (one `MAP_JIT` arena, per-thread write protect or write-callback allowlist,
>    `sys_icache_invalidate`). "Apple Silicon support" therefore means **two products**: iOS = IR-interpreter,
>    macOS = JIT (+ optional iOS-apps-on-Mac as a third, uncertain cell).
>
> **Recommended core strategy (decisive).** Build the translator as a **guest-agnostic front end + IR that is
> data, executed by a direct-threaded/gadget dispatcher**, and treat host machine-code generation as an
> optional *backend* selected by a single `CodeCache` abstraction with three implementations: (1) Android
> `mmap(PROT_READ|PROT_EXEC)` anon arena (+ explicit cache clean, respecting Android's own W^X rules),
> (2) Darwin `MAP_JIT` arena with per-thread write toggling / write-callback allowlist (**macOS only**), and
> (3) **the IR interpreter — the only tier compiled in on iOS, and therefore the tier that must hit the
> performance floor** (target: ≥1/3 of the JIT per iSH/UTM/libriscv evidence, i.e. playable 1.5 GHz A32 Android
> workloads on A17+-class silicon). Ship no code path that *assumes* (1) or (2) exist: iOS must be a first-class,
> tested, non-degraded-by-accident build from the first commit.

---

## Core design requirements imposed by iOS

Hard requirements on the shared C/C++ core (each traceable to §1–§5). Violating any of these makes iOS
unshippable later, so they belong in the architecture, not in a port phase.

* **Never assume `PROT_EXEC` is obtainable.** All execution must go through one `CodeCache` interface with
  three backends: `AnonExecMmap` (Android), `DarwinMapJit` (macOS only), `IrInterpreter` (iOS + fallback).
  Compile-time platform matrix must allow *removing* the two executable backends entirely (no dead iOS JIT
  calls, no "sim works so ship it" trap; iOS RWX requests silently return RW).
* **The translator's primary product is data.** Guest blocks are lowered to a compact, fixed-layout IR
  ("macro-op / gadget queue") stored in normal non-executable memory: operand fields at constant offsets, guest
  registers mapped to a fixed host location scheme (TCTI's x1–x15 discipline is the model), and each op
  terminated by an indirect branch to the next op (tailcall/threaded dispatch), *not* a `switch`.
* **Direct-threaded dispatch is mandatory in the default tier**, with computed-goto C as the portable
  fallback and a superinstruction/fusion table generated from a machine-readable spec (iSH's asm-gadget
  maintainability horror is the counter-example to avoid).
* **Decoding and lowering must be platform-neutral and pre-buildable.** Anything that can be computed offline
  (decode tables, gadget/opcode enumeration, per-ISA helper stub lists) must be generated at build time and
  shipped as data in the binary — never written to disk and `dlopen()`ed (impossible on iOS, §2.2).
* **Guest page protection is a software property, never a host `mprotect` call.** iOS/Darwin page size is
  **16 KB** on arm64 devices (reported as long-standing since the 64-bit transition —
  [HN 41330852](https://news.ycombinator.com/item?id=41330852); **verify on device with
  `sysconf(_SC_PAGESIZE)`**), Android arm64 is 4 KB and Android is moving to 16 KB configs
  ([Android 16 KB requirement](https://developer.android.com/guide/practices/page-sizes), deadline Nov 1 2025),
  and the guest ARMv7/Linux model is 4 KB. So: shadow guest PTEs with sub-host-page granularity + a dirty/page-
  changed bloom filter, and treat host mappings as advisory. Query `PAGE_SIZE`/`getpagesize()` at runtime;
  never hardcode 4096.
* **Self-modifying code and `mprotect(PROT_EXEC)` transitions must be detected without host page faults as the
  only mechanism.** Use explicit write-tracking in the store ops plus an optional guard-page fast path only on
  platforms where it is reliable; iOS Mach-exception delivery makes a fault-driven design fragile (§3.2).
* **One arena, budgeted.** Darwin permits a **single** `MAP_JIT` region; iOS caps virtual address space for
  non-`dynamic-codesigning` processes and jetsam caps resident memory. Plan: fixed-size guest RAM + IR arena
  (with LRU block eviction) + GL staging, sized from `os_proc_available_memory()` and a manifest; declare
  `com.apple.developer.kernel.increased-memory-limit` and behave correctly when the grant is absent.
* **Emitter/executor separation with an explicit mode guard.** Any Darwin codegen must compile into a
  plain buffer and commit with one validated copy inside `pthread_jit_write_with_callback_np()` (or an RAII
  `pthread_jit_write_protect_np(false/true)` scope). No guest code may execute inside an emit scope; no thread
  may cache a mode assumption across a thread hop. Cache invalidation (`sys_icache_invalidate` on Darwin,
  `__builtin___clear_cache` on Android) lives inside that commit step, so backends can't forget it.
* **No `fork()`, no `execve()`, no child processes, no `posix_spawn` of guest code.** Android's multi-process
  model is emulated as threads + per-"process" guest page tables inside the single host process; the guest VFS
  resolves `/data`, `/system`, `/sdcard` to sandbox subpaths.
* **All host-side libraries statically linked; nothing loaded at runtime.** Guest `.so` files are *data* for
  RetroBug's guest ELF loader (doc 02). The JNI seam on iOS cannot be a generated host `.so` — it must be a
  table of entry points inside the main executable.
* **No private API, no `ptrace`, no signature-bypass hacks** (2.5.1 + §1). Include an CI "public API only"
  symbol check for the iOS build.
* **A pause/restore contract at instruction granularity from day one.** Deterministic quiesce ("stop at the
  next safe op", like libriscv's instruction-count `machine.simulate(n)` and `stop()`), plus full save-state
  (registers, guest RAM dirty pages, IR/TB cache is *rebuildable* not serialised, guest FD/timer/clock state) —
  required by iOS suspension, useful for Android, and the only way to survive jetsam.
* **Virtualised guest clocks and a background-mode policy:** guest `uptimeMillis`/`nanoTime` advance under
  RetroBug's control; on `didEnterBackground` stop CPU threads, snapshot, and freeze; on resume, compensate so
  guest watchdogs/ANRs and network timeouts don't fire on the first frame back. Never hold a background mode
  that isn't the app's real purpose (2.5.4).
* **Real-time-safe audio path:** Core Audio render callback must never take a translator lock, never touch the
  IR arena, never allocate — which also keeps Android's AAudio/OpenSL ES shim honest.
* **Graphics abstraction with a command stream, not an API alias:** guest GLES1/2/3 → RetroBug GL-ish
  intermediate → backends {Metal, MoltenVK(Vulkan 1.4), host EAGL/GLES shim (iOS-only, deprecated API watch per
  2.5.1), Android GLES}, plus a software framebuffer path for bring-up and failure recovery.
* **Two products, one core, and a per-platform CI gate:** Android arm64 (JIT tier on), macOS/Catalyst
  (Darwin `MAP_JIT` tier on), iOS (**IR tier only**). Keep the "iOS has no executable backend" build as a CI
  job so no contributor can accidentally introduce a JIT dependency into shared code — the
  "two CMake entry points so iOS can never regress Android" pattern from doc 08 (`XenDroid/CMakeLists.txt`).
* **License hygiene stays a design constraint, not just policy:** TCTI/UTM's QEMU fork and Unicorn are GPL-2.0
  → **study for structure, reimplement** (doc 01's boundary); the *shape* of the IR and the gadget-generation
  approach may be informed; code may not be copied. Dynarmic-class 0BSD code and Capstone (BSD-3) remain the
  linkable references.

---

## Open questions / unverified

1. **iOS-on-Mac ("My Mac (Designed for iPhone)", and PlayCover-style locally re-signed iOS apps): can an iOS
   binary on Apple Silicon Macs get `allow-jit`/`dynamic-codesigning`?** Apple documents no such grant, and
   `pthread_jit_write_protect_np` is absent from the iOS runtime; but macOS-side tooling re-signs iOS apps with
   extra entitlements locally, and I could not retrieve primary evidence either way (Outflank's 2026-02
   "macOS JIT Memory" writeup was Cloudflare-blocked). **Action: hands-on test on an M-series Mac; and if a
   local re-sign path works, document it as a "Pro" channel, never as an App Store feature.**
2. **Whether an iOS-linked binary running on macOS sees the arm64 per-thread W^X mode** (the mode is enforced by
   the kernel/pmap on Apple silicon "for all apps", while the toggle API is macOS-only) — this is a real
   inconsistency I could not resolve from documents. Matters only if (1) turns out feasible.
3. **Does Apple's in-EU browser-engine JIT entitlement family (`com.apple.developer.web-browser-engine.*`)
   technically require the app to be a browser, and can a non-browser extension process piggyback?** iSH's
   answer was "officially, even developing against this API requires permission from Apple" and their request
   was refused. Assume no.
4. **Any public, current list of criteria for granting JIT to App Store apps** ("categories that got
   `dynamic-codesigning`") — I found none on iOS; on macOS the entitlement is self-declared. The brief's premise
   that some iOS categories received it is **unconfirmed** in this pass.
5. **JSC executable-memory trick status on iOS 18/26** — asserted dead by the ecosystem's drift to
   tether/pairing JIT enablers; needs a definitive "does X still work on iOS 26" test before it can be even
   listed as a debug path.
6. **Quantitative iOS numbers for RetroBug's exact workload.** I have ratios (interpreter ≈ 1/3 of JIT;
   threaded ≈ 3–5× of switch dispatch; iSH says JIT would buy ~10×) but **no measurement of an A32→A64
   IR-interpreter on A-series silicon**. A 2-week spike: hand-write an A32 macro-op IR + threaded dispatch, run
   a synthetic Android-ish compute loop + a real GLES2 draw loop, measure vs the Android JIT tier. This number
   decides whether iOS is a v2 or a v3 target. (Related unverified: RetroArch's per-core iOS performance
   statements, DuckStation/Flycast/DolphiniOS no-JIT frame rates, "Retro64", and box64's `dynarec_lightly`/FEX's
   `UCF` specifics — I found no primary text for those two names and should not be quoted on them.)
7. **Guest GLES on iOS via host EAGL**: whether Apple's ES 1.1/2.0/3.0 implementation survives iOS 26 on device
   and how it behaves under Metal-backed layer compositing; also whether Apple's ES 3.2 superset claims are
   usable. Needs a device test; plan the Metal backend as primary regardless.
8. **Exact jetsam ceiling per target device class** (iPhone 15/16/17, iPad M-series) and whether
   `increased-memory-limit` is granted to an emulator in practice (the entitlement is declarable, not applied
   for) — plus how much VA space a 4 KB-page guest + IR arena consumes under iOS's non-"jumbo" VM limit.
9. **16 KB page claim** for current iOS devices: widely reported as long-standing (A7+ 64-bit), but I lack a
   quotable Apple primary source. **Must be measured with `sysconf(_SC_PAGESIZE)` and `vm_page_size`** on the
   minimum supported device before finalising the guest-MMU design.
10. **App Review appetite for an *Android*-app runner** (as opposed to a retro *console* runner): 4.7 names
    "retro game console and PC emulator apps", not "Android app compatibility layer". A shipping app that runs
    modern-ish 32-bit Android apps may draw different treatment (3rd-party-app-installation framing, 2.5.2's
    "including other apps"). Needs a pre-submission inquiry (Apple offers one) and possibly a repositioning as
    "legacy app preservation / retro Android games".
11. **Legal review of "App Revival Patches" applied to third-party APKs** (modification of copyrighted works,
    DMCA §1201 exposure if any patch touches a licence check) — engineering guidance given in §4.3, not legal
    advice.
