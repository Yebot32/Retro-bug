# 90 — Cross-check: contradictions, adjudications, and the decisions Phase 1 must implement

> Phase 0 cross-check pass. Input: docs 01–10 (written independently). Method: adversarial
> re-reading plus first-hand verification against primary sources (AOSP `system/sepolicy`,
> ART source, Dynarmic source, upstream project trees). Nothing in this file is a summary;
> every line is either a verdict, a resolution, or a named experiment.
>
> Status: complete. Sections 2–5 are the deliverable; the **decisions** are the paragraphs marked
> **DECISION**, and everything else exists to justify or falsify one of them.
>
> Primary sources I pulled and read for this pass (nothing below rests on a doc's own citation where I could
> check it): `system/sepolicy` @ main (full clone), `art/runtime/jni/java_vm_ext.cc` @ main,
> `art/libnativeloader/native_loader_namespace.cpp` @ main, `bionic/linker/linker.cpp` @ main,
> `frameworks/libs/binary_translation/README.md` @ main, `packages/modules/ANGLE/Android.bp` @ main,
> `azahar-emu/dynarmic` @ master (full clone, v6.7.0) + `lioncash/dynarmic` @ master, GitHub repository API,
> and Apple's *Porting just-in-time compilers to Apple silicon*.

## Verdicts carried forward

Confidence is *my* assessment after re-checking the primary source, not the originating doc's confidence.

| # | Verdict | Evidence (first-hand unless noted) | Conf. |
|---|---|---|---|
| V-1 | **A normal Android app may allocate executable memory.** RetroBug's JIT inside an `untrusted_app` — and inside an `isolated_app` — is permitted by platform policy. | `system/sepolicy` @ main, `private/app.te:189` = `allow appdomain self:process execmem;` (verified verbatim; matches doc 09 and the product owner). No neverallow touches `process execmem` for app domains. | **HIGH** |
| V-2 | **…but an app may not execute code it wrote into its own data dir.** | `private/app_neverallows.te:69-73` (verbatim, `execute_no_trans` neverallow on `app_data_file`/`privapp_data_file` for all modern-target untrusted apps) **plus a stronger rule nobody cited**: `private/app.te:731-746` neverallows `no_x_file_perms = { execute execute_no_trans }` (`public/neverallow_macros:5`) for `isolated_app_all`/`sdk_sandbox_all`/`system_app`/`bluetooth`/`nfc`/`radio`/`shared_relro` on all `data_file_type` except `apex_art_data_file`, `dalvikcache_data_file`, `system_data_file`, `apk_data_file`. | **HIGH** |
| V-3 | **Code shipped inside an installed APK is executable by an app**, including from an `isolated_app` (so a pre-built agent `.so` in `jniLibs/` is the correct place for RetroBug's host code). | `private/app.te:441-443` (`apk_data_file:file { … map x_file_perms }`) + `public/global_macros:24` (`x_file_perms = { getattr execute execute_no_trans map }`); `apk_data_file` is not `app_data_file_type` (`public/file.te:381` vs `:517`), so V-2's neverallow does not reach it. | **HIGH** |
| V-4 | **memfd/anonymous executable memory is available to every app domain**, which is the sanctioned substrate for a code cache (and is how ART's JIT works). | `public/te_macros:245-247` inside `app_domain()`; `appdomain_tmpfs` declared `file_type` only (`public/app.te:9`) → outside V-2's neverallow; `app_domain(isolated_app)` at `private/isolated_app.te:12`. (ART side: doc 09 §3.4 / `jit_memory_region.cc`, not re-read here.) | **HIGH** |
| V-5 | **The Android native bridge is unreachable to a normal APK, and so is ART's trampoline contract.** No supported path lets an app hand ART a trampoline for `(name, shorty)`: the branch is gated on `NeedsNativeBridge()`, which only bridge initialisation sets. | `art/runtime/jni/java_vm_ext.cc:154-186` (`FindSymbol` → `dlsym` unless bridge) and `:335` (`shorty` passed only when bridged), fetched @ main; plus doc 02 §1.4's four cited blockers (`AndroidRuntime.cpp:1105-1122`, `native_bridge.cc:178-208,260,47-67`, `native_bridge_art_interface.cc:103-107`). | **HIGH** |
| V-6 | **ART calls `JNI_OnLoad` with the loading class loader overridden**, and `RegisterNatives` from host code is a fully supported, public path for binding natives without any `dlsym` hit. | `java_vm_ext.cc:1125-1142` (`self->SetClassLoaderOverride(class_loader)` … restore) — verified @ main. | **HIGH** |
| V-7 | **Dynarmic is 0BSD and its A32→AArch64 path is real, default-on and CI-differentially-tested; it has no interpreter, `MemoryProcessor` no longer exists, and upstream is archived.** | `LICENSE.txt` verbatim ("Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted", © 2017 merryhime); `backend/arm64/{a32_address_space,emit_arm64_a32,emit_arm64_a32_coprocessor,emit_arm64_a32_memory}.cpp`; `.github/workflows/aarch64.yml` (`ANDROID_ABI=arm64-v8a`, `diff <(qemu-aarch64 … generator thumb …) <(build-x64/…)`); `CMakeLists.txt` option list (no `USE_JIT`); `grep MemoryProcessor` → 0 hits; GitHub API `lioncash/dynarmic` → `archived: true`, pushed 2024-03-05. | **HIGH** |
| V-8 | **Berberis cannot be RetroBug's engine, but its AArch32 ABI/shim layer can be reused.** | `frameworks/libs/binary_translation/README.md` @ main, first line verbatim: "Dynamic binary translator to run Android apps with **riscv64** native code on **x86_64** devices or emulators"; build = `lunch sdk_phone64_x86_64_riscv64-trunk_staging-eng` (AOSP-tree-only). Doc 06 §2 (fetched) confirms `guest_abi/arm`, `stubs_arm.cpp`, `linker.arm.map`, `init_static_tls_arm.S` are genuinely AArch32, Apache-2.0. | **HIGH** |
| V-9 | **A shippable iOS app cannot JIT.** The escape hatch is macOS-only, and the developer's own probes agree. | Apple, *Porting just-in-time compilers to Apple silicon* (fetched this pass, markdown form): verbatim **"The `pthread_jit_write_protect_np()` function isn't available on iOS."**, and `MAP_JIT` requires `com.apple.security.cs.allow-jit`. Corroborated by doc 08 item 1 ("`allow-jit` … does NOTHING on iOS — an active red herring") and item 2 (uncatchable SIGKILL; iOS 26 TXM/SPTM `brk #0xf00d` handshake; "even get-task-allow doesn't save an unprepared execute" on iOS 27 beta) — **first-party, but I did not re-run those probes.** | **HIGH** (Apple quote) / **MEDIUM** (per-release iOS 26/27 specifics — treat as re-verify-per-release, per doc 08 item 2) |
| V-10 | **The OS-enforced sandbox tier is as restrictive as doc 05 says, and in three ways more so:** no GPU driver, ≤3 findable system services, cannot `open()` its own data files, cannot execute any data-file path. | `private/isolated_app_all.te:73` (gpu_device neverallow), `:65-70` (service_manager: only activity/display/webviewupdate), `:42-43` (app_data_file open neverallow), `private/app.te:393-395,731-746`, `public/app.te:407` (`binder_call(…, gpuservice)` excluding isolated). | **HIGH** |
| V-11 | **Guest page granule is 4 KiB, fixed; host granule must be queried.** | doc 09 §5 (Android 15+ 16 KB devices, Play deadline; `getauxval(AT_PAGESZ)`), reinforced by the engine: `interface/A32/config.h:156-158` `PAGE_BITS = 12`. | **HIGH** (decision) / **MEDIUM** (how often mixed-protection granules actually occur) |
| V-12 | **Graphics: forward on Android, ANGLE-on-Metal on iOS, software renderer as the universal reference and sandbox path; Vulkan is not the host abstraction.** | doc 03 §6/§Recommendation (well-evidenced internally); doc 06 §5 licence facts (ANGLE BSD-3, MoltenVK Apache-2.0, SwiftShader Apache-2.0, gl4es MIT); my own check of `packages/modules/ANGLE/Android.bp:67-110` showing AOSP ships `libGLESv1_CM_angle` for `arm` **and** `arm64` over `libGLESv2_angle`→`libvulkan`. | **MEDIUM** — rests on U-4 and U-5 |
| V-13 | **No shipping system has RetroBug's exact shape** (64-bit host process + emulated 32-bit guest in one address space + host runtime APIs). This is a novelty claim, not a risk claim; note that dynarmic's v6.7.0 changelog shows at least one *engine* explicitly supports the address-space-sharing mode. | doc 07 §6 (negative search results, no device/product found); my own read of `README.md` v6.7.0 note ("guest to have the same address space as the host"). | **MEDIUM** (absence of evidence) |
| V-14 | **Old apps break for non-ABI reasons too**, so "CPU emulated" ≠ "app works", and the zero-native-cohort is not free. | doc 02 §6.5, §8 (honest: the share of 32-bit-only pure-DEX apps is *unmeasured*; the 72.5% figure is a biased sample). | **LOW** on the numbers, **HIGH** on the direction |
| V-15 | **The milestone corpus can be built on this machine without Gradle or a device**, and its ABIs verified. | doc 10 §4.1–§4.4, §5, §6.1 (measured paths/versions on this box: NDK r28.2/r29.0, clang 21, JDK 21, build-tools 35/36.1/37, `llvm-objdump`, `dexdump`, `readelf -A`, 32-bit linker stub libs). | **HIGH** |

## Contradictions and resolutions

### C1 — guest JNI binding vs. the `execute_no_trans` neverallow

**Conflict.** Doc 02 TL;DR (lines 18–25, 44–57) makes the milestone-1 mechanism: `ClassLoader.findLibrary()`
returns a path to an **arm64 shadow `.so` that RetroBug generates per guest library at runtime**, and every
exported JNI symbol in it is an arm64 stub. Doc 02 itself flags this as its own #1 risk (line 75–78, OQ-3 at
1251–1255: "This decides whether shadow libs are generated at … or shipped"). Doc 04 §2.2 (line 215–217)
independently declares the plan Android-only and says the iOS JNI seam "must be a symbol inside the main
binary reached through a table, not a dylib." The product owner's sepolicy finding kills the *generated* part
on Android too.

**What I verified myself** (fresh clone of AOSP `system/sepolicy` @ main, and of
`platform/art/runtime/jni/java_vm_ext.cc`, `libnativeloader/native_loader_namespace.cpp`,
`platform/bionic/linker/linker.cpp` @ main):

| Rule / code | Location | Effect |
|---|---|---|
| `neverallow { all_untrusted_apps -untrusted_app_25 -untrusted_app_27 -runas_app } { app_data_file privapp_data_file }:file execute_no_trans;` | `private/app_neverallows.te:69-73` | CONFIRMED verbatim. |
| `neverallow { bluetooth isolated_app_all nfc radio shared_relro sdk_sandbox_all system_app } { data_file_type -apex_art_data_file -dalvikcache_data_file -system_data_file -apk_data_file } :file no_x_file_perms;` with `no_x_file_perms = { execute execute_no_trans }` | `private/app.te:731-746`, `public/neverallow_macros:5` | **STRONGER than C1 as stated.** The sandbox tier cannot obtain `execute` (mmap PROT_EXEC) at all on any `data_file_type` path outside the four exceptions. |
| `x_file_perms = { getattr execute execute_no_trans map }`; `allow appdomain apk_data_file:file { getattr open read ioctl lock map x_file_perms };` | `public/global_macros:24`, `private/app.te:441-443` | CONFIRMED. Code shipped inside an installed APK **is** executable by an app (including `isolated_app`, because `apk_data_file` is *not* `app_data_file_type` — `public/file.te:381` vs `:517`). |
| `app_domain($1)`: `type_transition $1 tmpfs:file appdomain_tmpfs; allow $1 appdomain_tmpfs:file { execute getattr map read write ioctl };` | `public/te_macros:245-247` | memfd/ashmem exec IS permitted for every app domain incl. `isolated_app` (`appdomain_tmpfs` is `file_type` only — `public/app.te:9` — so it is outside the `data_file_type` neverallow above). This is why ART's JIT and RetroBug's code cache work. |
| `SharedLibrary::FindSymbol` = `NeedsNativeBridge() ? FindSymbolWithNativeBridge(→ NativeBridgeGetTrampoline2) : dlsym(handle_, name)` | `java_vm_ext.cc:162-186` | CONFIRMED. |
| `const char* arg_shorty = library->NeedsNativeBridge() ? shorty : nullptr;` | `java_vm_ext.cc:335` | CONFIRMED: ART **only** hands a shorty to a trampoline provider when the *native bridge* is on. `SetNeedsNativeBridge` (`:154`) is reached only through bridge initialisation, which doc 02 §1.4 proves is zygote/system-namespace only. |
| `self->SetClassLoaderOverride(class_loader); … jni_on_load(...) … SetClassLoaderOverride(old)` | `java_vm_ext.cc:1125-1142` | CONFIRMED. During `JNI_OnLoad` the *loading* class loader is active for `FindClass`. |
| `extinfo.flags = ANDROID_DLEXT_USE_NAMESPACE;` and nothing else | `native_loader_namespace.cpp:168-171` | **Decisive for option (ii)** — ART never sets `ANDROID_DLEXT_USE_LIBRARY_FD`. |
| fd-based load path bypasses `open_library()` (and therefore `is_path_allowed`) entirely | `linker.cpp:1321-1340`, `realpath_fd()` at `:379-398` | The linker *can* load from a raw fd / memfd (`readlink("/proc/self/fd/N")` is its own canonicalisation helper) — but only for a caller that passes `extinfo->library_fd`. A `findLibrary()` string cannot express that. |

**Adjudication of the four options.**

- **(ii) memfd + `dlopen("/proc/self/fd/N")` — REJECTED, and not for the reason the question assumed.** The
  SELinux half is *fine* (`appdomain_tmpfs:file execute`). It dies one layer up: ART, not RetroBug, issues the
  `dlopen`, and it issues it inside a linker namespace whose `permitted_paths` are the class loader's library
  directories (`native_loader_namespace.cpp:92-143`). A `/proc/self/fd/N` string is an absolute path that
  `android_namespace_t::is_path_allowed` will refuse ("not accessible for the class loader context"), and the
  fd-carrying `extinfo` flag is unreachable from a path string. Cost to disprove: one 20-line test app (see
  unverified table, U-1) — but I would not spend the time; the code path is closed by construction.
- **(i) pre-built shadow libs shipped in RetroBug's APK — REJECTED as the general mechanism.** It only works
  for APKs whose export tables were known when RetroBug was built. RetroBug's product is *"import an APK"*
  (brief (f)); a corpus-bounded JNI binding layer is exactly the per-app hardcoding brief (g) forbids. Keep it
  only as a *test fixture* (it is how you stand up milestone 1 against a fixed corpus without building the DEX
  index first).
- **(iii) `RegisterNatives` from the host side — ADOPT, and it is strictly better than the shadow lib, not a
  compromise.** The insight doc 02 missed: RetroBug does not need `dlsym` to resolve `Java_*` names, because
  **the authoritative list of native methods is in the guest DEX, not in the guest `.so`.** The DEX says
  class, method name and full signature (⇒ `shorty`) for every `native` method, with no guessing about return
  type (which the JNI symbol mangler hides). So: at import time RetroBug indexes the guest DEX's native
  methods; at load time its own pre-shipped host code calls `JNIEnv::RegisterNatives` with arm64 trampolines
  that enter the translator. No file is generated, nothing new is executable, no hidden API, no reflection.
  `RegisterNatives` also happens to be *the* mechanism doc 02 §2.2 (line 368) already identified as "the single
  most important interception" for the guest→host direction.
- **(iv) better thing = (iii) + one pre-shipped agent `.so` + keeping the guest's own `JNI_OnLoad` as the
  driver.** Concretely, the design Phase 1 must implement:
  1. Ship **one** generic arm64 `libretrobug_agent.so` inside RetroBug's own APK (`jniLibs/` → `apk_data_file`
     → executable per `app.te:443`). `findLibrary(name)` returns that same path for *every* guest library name.
     No symbol tables are generated anywhere, ever.
  2. RetroBug's guest linker loads the real `EM_ARM` `.so` **as data**, resolves its relocations against the
     emulated 32-bit bionic/proxy libs, and reads its `.dynsym`.
  3. RetroBug then runs the *guest's* `JNI_OnLoad` under the translator with a guest-side `JNIEnv` proxy table
     (doc 02 §2.1 design (A)); a guest `RegisterNatives` call is intercepted and re-issued host-side with an
     arm64 trampoline per `(guest fnPtr, shorty)`. This cohort needs no DEX index — the app tells us.
  4. For the *implicit*-JNI cohort (no `RegisterNatives`, which is most pre-2015 apps), RetroBug registers
     host-side trampolines for every native method in its import-time DEX index whose JNI long/short name
     resolves in a loaded guest library, resolving classes through its own `DexClassLoader` — so it does not
     depend on the `SetClassLoaderOverride` window at all.
  5. If a guest `native` method matches *no* loaded guest export, leave it unbound and let ART throw
     `UnsatisfiedLinkError` — that is what happens on real 32-bit Android, so fidelity is preserved.
  6. Optional refinement if a per-library independent `JNI_OnLoad` turn is ever needed: ship a small *pool*
     (`libretrobug_agent0..7.so`) — same bytes, distinct realpath, so ART's path-keyed dedup gives distinct
     handles. This is a build-time packaging decision, not runtime generation.

**Prototype order: (iii)+(iv) first, in week 1, and it costs almost nothing** — it is "a pre-shipped .so +
`RegisterNatives`", i.e. an ordinary JNI sample app, testable on any arm64 device with no emulator at all. The
generated-shadow-lib plan is deleted from the architecture, which also removes doc 04's iOS-specific
exception (line 215–217) as a *duplicated* special case: **the same agent-table design works on iOS unchanged,
because iOS has no ART and therefore no `dlopen` requirement in the first place.** C1 therefore disappears
entirely as a cross-platform divergence once shadow libs are dropped — that is the real resolution.

**Consequences for other docs.** Doc 02's risk #1 (line 75–76) is closed. Doc 02's §2.5 "guest-visible address
space" and §2.2 handle tables are unaffected. Doc 02's claim that `System.loadLibrary` interception is the only
app-level hook survives, but the payload changes from "generated library" to "constant library". Add one new
requirement doc 02 does not have: **`System.load(absolutePath)`** (`Runtime.java:914`) bypasses `findLibrary`
entirely and hands ART a path to an `EM_ARM` file → hard failure. Mitigation is at the DEX layer (rewrite
`System.load` call sites to RetroBug's loader during import — RetroBug already parses the DEX for the native
index), so it is cheap, but it must be in the architecture, not discovered later.

### C2 — original interpreter+DBT vs. building on Dynarmic

**Conflict, restated more widely than the brief did.** There are **three** positions, and doc 01 is
self-inconsistent:
- Doc 01 TL;DR (29–33) + §8: "Build **(F) an original ARMv7 interpreter + selective DBT** as the load-bearing
  engine… mine Dynarmic's decode + regalloc *ideas*" — but doc 01 §8 tier 1 (line 456) then says
  "**fork/extend Dynarmic `arm_32` (0BSD)** for the instruction executors". Both cannot be the plan.
- Doc 02 (lines 54, 67–73): "Translator = **fork of Berberis + new ARM32 frontend**… the biggest single reuse
  opportunity."
- Doc 06 (line ~626–637, "Component guidance"): "**CPU: reuse Dynarmic `arm_32`** for decode + A32→AArch64 JIT +
  VFP/NEON", and separately refutes any AArch32 *translator* in Berberis.

**Evidence I gathered first-hand** (clone of `azahar-emu/dynarmic` @ master, v6.7.0, last commit 2026-06-24;
plus GitHub API, and `lioncash/dynarmic` @ master for comparison):

1. **The A32 → AArch64 path is real, default-on, and CI-differential-tested — harder evidence than doc 06
   cites.** `src/dynarmic/backend/arm64/` contains `a32_address_space.cpp`, `a32_core.h`, `a32_interface.cpp`,
   `a32_jitstate.cpp`, `emit_arm64_a32.cpp`, `emit_arm64_a32_coprocessor.cpp`, `emit_arm64_a32_memory.cpp`
   (726 `EmitIR<IR::Opcode::…>` specialisations in that backend). `.github/workflows/aarch64.yml` builds
   `-DANDROID_ABI=arm64-v8a` (lines 58–68) **and** runs
   `diff <(qemu-aarch64 … ./build-arm64/tests/dynarmic_test_generator thumb …) <(./build-x64/tests/dynarmic_test_generator thumb …)`
   for **arm**, **thumb** and **a64** at three seeds each (lines 108–126) — i.e. upstream CI *asserts*
   A32-on-AArch64 ≡ A32-on-x64 on generated instruction streams. That is precisely the oracle harness doc 01
   §8.6 proposes to build. The "did Citra ever run ARM32 guests on AArch64 hosts in anger" question is
   therefore moot: it is worse for us and better for us than that — the guarantee lives in dynarmic itself.
2. **`fastmem` — the thing doc 07 concluded was necessary — already exists and is documented in the config
   header I read** (`src/dynarmic/interface/A32/config.h:181-199`): "This should point to the beginning of a
   4GB address space which is arranged just like what you wish for emulated memory to be. If the host page
   faults on an address, the JIT will fallback to calling the MemoryRead\*/MemoryWrite\* callbacks", with
   `recompile_on_fastmem_failure`, `fastmem_exclusive_access`, `check_halt_on_memory_access` (accurate
   protection-fault emulation), plus a **page table at `PAGE_BITS = 12`** (4 KiB guest pages, independent of
   host page size) as the non-fastmem alternative. And v6.7.0's API-change note (README line 46–48):
   *"To support use cases where one wants to have **the guest to have the same address space as the host**,
   `nullptr` is now a valid value for `fastmem_pointer`."* That sentence describes RetroBug's exact shape
   (doc 07 §1: "nobody has built RetroBug's exact shape") — upstream supports it as a first-class mode.
3. **Every "what does it lack" item in the brief, checked:**
   - **`svc`/syscalls: PRESENT as a callback.** `UserCallbacks::CallSVC(std::uint32_t swi)`
     (`config.h:106-107`), emitted from `a32_exception_generating.cpp:26` (`arm_SVC`) and
     `thumb16.cpp:954`. RetroBug writes the ARM-Linux syscall ABI on top; `r7` dispatch, `ARM_NR_*` magic
     calls and `cacheflush` are all ours. Doc 01 §5.1's "no `svc`→Linux-syscall table" is *true but trivial*:
     the hook exists, the table is 300 lines of our own data.
   - **CP15 / TLS: HOST-SUPPLIABLE, AND JIT-FAST.** There is no built-in P15 for A32 (by design — dynarmic is
     SoC-agnostic), but `UserConfig::coprocessors[16]` + `Coprocessor::CompileGetOneWord/SendOneWord`
     (`interface/A32/coprocessor.h:55-90`) return either a callback **or a raw host word address**, so
     `MRC p15,0,Rt,c13,c0,3` (TPIDRURO) and `c13,c0,1` (TPIDRURW) can be lowered by the JIT into a plain
     `LDR`/`STR` against our per-thread state — zero overhead, which is what Android's `tp`-register
     convention needs. `emit_arm64_a32_coprocessor.cpp` proves the AArch64 backend implements this lowering.
     TPIDRPRW (privileged) is out of scope: `README` "Only user-mode is emulated".
   - **Exclusive monitor across host threads: SOLVED, with a perf caveat.** `interface/exclusive_monitor.h` is
     a `SpinLock`-guarded global monitor keyed by `processor_id`, with `ReadAndMark`/`DoExclusiveOperation`
     and a `RESERVATION_GRANULE_MASK` of all-ones (granule = exact address). `UserConfig::global_monitor` +
     `processor_id` let N host threads share it; `fastmem_exclusive_access=false` by default and the config
     comment admits the x64 fastmem path leans on `cmpxchg` semantics "which may not provide fully accurate
     emulation". **Consequence for Phase 1:** one `processor_id` per guest thread, and a decision to make about
     whether to lower `LDREX/STREX` to `LDXR/STXR` ourselves (fast, needs our own monitor) or take dynarmic's
     locked path (correct, contended). Also note the honest README line: "Exclusive monitor behavior may not
     match any known physical processor" — a differential-test target, not a blocker.
   - **kuser page at `0xffff0000`: free under fastmem.** Doc 01 §7.6 calls this "a genuine 32-on-64 trap"
     because a 64-bit host reserves the top of the address space. With a flat 32-bit guest window
     (`fastmem_pointer` + 4 GiB arena), guest `0xffff0000` is simply `base + 0xffff0000` inside our own
     reservation: we place our A32 helper stubs there as guest *data* and the JIT executes them like any other
     guest code. QEMU's issue #447 is a symptom of not having a 4 GiB window. This is a *simplification* doc
     01 never saw because it evaluated dynarmic through a stale API name (next bullet).
   - **`MemoryProcessor`: THIS INTERFACE NO LONGER EXISTS.** Grep over the whole current tree: zero hits.
     Modern dynarmic uses `UserCallbacks::MemoryRead8/16/32/64` + `Write…` + `MemoryWriteExclusive…` +
     `IsReadOnlyMemory` + `page_table` + `fastmem_pointer`. Doc 01 §6/§8 and the brief both reason from a
     ~2021 API. The capability the brief asked about ("does `MemoryProcessor` support a shared host/guest
     address space with fastmem") is present, but under different names — so **doc 01's assessment of dynarmic
     is based on an obsolete interface and must not be trusted for API fit.**
   - **JIT invalidation / self-modifying code: PRESENT.** `A32::Jit::InvalidateCacheRange(u32 start, size_t)`
     (`interface/A32/a32.h:48`), `HaltReason::CacheInvalidation`, `hook_isb` config flag, plus
     `block_range_information.cpp` for write-tracking. Sufficient for a guest that `mprotect`s text or
     self-patches (bionic's `__relocate_object`, ART's own guest-side thunks). Doc 01 §7.8's requirement is met.
   - **Thumb-2 / VFP / NEON completeness:** decoders `arm.inc`, `thumb16.inc`, `thumb32.inc`, `vfp.inc`,
     `asimd.inc` + `ITState.h` + `conditional_state.{h,cpp}` (the IT-block predicate machinery) + `PSR.h` +
     `FPSCR.h`; guest list in README explicitly includes **v7A**; `ArchVersion` selectable. Caveat from
     upstream's own README "Disadvantages": "**FPSR state is approximate**", "misaligned loads/stores are not
     appropriately trapped in certain cases". Two real gaps for *userspace* (not 3DS): misalignment trapping
     (bionic's `memcpy`/`sqlite` will hit it) and approximate FPSR/exception-bit behaviour.
   - **Interpreter tier: DOES NOT EXIST.** There is no `DYNARMIC_USE_JIT` option in `CMakeLists.txt` (the
     options are `ENABLE_CPU_FEATURE_DETECTION`, `ENABLE_NO_EXECUTE_SUPPORT`, `FATAL_ERRORS`,
     `IGNORE_ASSERTS`, `TESTS`, `TESTS_USE_UNICORN`, `USE_LLVM`, `USE_PRECOMPILED_HEADERS`,
     `USE_BUNDLED_EXTERNALS`, `WARNINGS_AS_ERRORS`, `FRONTENDS`). `UserCallbacks::InterpreterFallback` is a
     callback the *host* must implement, for single-stepping, not a CPU model. So the brief's hypothesis that
     `DYNARMIC_USE_JIT=OFF` yields doc 04's iOS tier is **FALSE** — and that matters a lot for C3.
   - **Maintenance status — WORSE THAN DOCS 01/06 REPORT.** `github.com/merryhime/dynarmic` → HTTP 404 (doc 06
     noticed). **`lioncash/dynarmic` is `archived: true`, last push 2024-03-05** (GitHub API, checked). The
     living tree is `azahar-emu/dynarmic` (fork of Citra's dynarmic; pushed 2026-06-24; contains A32, A64
     frontends and **x64 + arm64 + riscv64 backends**). So the A32 frontend is maintained by a *3DS emulator
     project*, on a cadence set by 3DS needs, with breaking API changes (v6.7.0 changed `fastmem_pointer`'s
     type semantics "not backwards-compatible").
4. **Doc 02's "fork Berberis" is not viable for a different reason than licensing**: doc 06 §2 establishes the
   only buildable engine there is riscv64→x86_64, the tree is Soong/GreenAndroid-only ("no CMake / standalone
   build"), and ARM/ARM64 guest translation "has been removed". Vendoring it means maintaining an out-of-tree
   build of an AOSP-internal module. Meanwhile the *part that is actually valuable* — `native_bridge_support`'s
   genuinely-AArch32 ABI layer (`guest_abi/arm`, `stubs_arm.cpp`, `linker.arm.map`,
   `init_static_tls_arm.S`, the `android_api` proxy-lib generator) — is Apache-2.0 and reusable as
   *shim/ABI data*, exactly as doc 06 says.

**VERDICT: `BUILD-ON-DYNARMIC` (a strong, specific flavour of HYBRID). Not build-our-own.** Reasons, in order
of weight:
1. **The 3 expensive, boring, correctness-critical assets are already there and already tested on the exact
   host we ship for**: ~2500-instruction A32/T32/VFP/NEON semantics, the AArch64 backend (726 ops), fastmem +
   page table + monitor + invalidation. Doc 01 §7 identifies IT-block, kuser/CP15 and `LDREX/STREX` as the
   three highest-risk areas — dynarmic's CI differentially tests the first two of those three *today*. Writing
   our own means re-incurring that risk with zero product benefit, and RetroBug's actual novel work is
   *elsewhere* (Android runtime surface, JNI marshalling, VFS, App Revival, iOS tier).
2. **Every "Linux/Android userspace" gap the brief asked about is a *host-side* component dynarmic
   deliberately delegates**, with a hook already in place (`CallSVC`, `Coprocessor`, `MemoryRead/Write`,
   `InvalidateCacheRange`, `ExceptionRaised` incl. `NoExecuteFault`) — i.e. the seams exist at exactly the
   boundaries RetroBug needs. By contrast, the two things dynarmic *admits* it gets wrong (approximate FPSR,
   misalignment trapping) are gaps we'd have to fix in **any** engine, ours included.
3. **Licensing is not a constraint here** (0BSD, confirmed verbatim; doc 06 §1), so choosing our own would be
   paying for principle.

**What we must therefore do, concretely (Phase 1 inputs):**
- **Vendor a pinned snapshot** at `third_party/dynarmic/` from `azahar-emu/dynarmic` @ a named commit, with
  `LICENSE.txt` in `LICENSES/` and a `NOTICE.md` entry (doc 06's plan, unchanged). Do **not** track a branch.
- **Fork-patch minimally and keep the patch series in-tree, reviewable**: (a) CMake gate so the A32 frontend +
  IR build with **no** backend (backends are `if("<arch>" IN_LIST ARCHITECTURE)` blocks in
  `src/dynarmic/CMakeLists.txt:264+`; the iOS tier links none of them); (b) an IR interpreter *as a new
  backend* (see C3); (c) optional: a real reservation-granule + `LDXR/STXR` exclusive path; (d) misalignment
  trapping behind a config flag. Budget: this is a *patch series against a vendored tree*, not a fork — that
  distinction is a Phase-1 policy decision and must be stated in the architecture doc (doc 08's P2
  "never edit the vendored core; wire everything from the outside" is the counter-recommendation; **for
  dynarmic, P2 is not achievable**, because the iOS tier and the trapping fixes must live inside it. Record
  that as an explicit, deliberate deviation from doc 08.)
- **Do NOT plan on its API stability.** Everything consumed from dynarmic must sit behind one RetroBug header
  (`rb_cpu.h`) — `UserConfig`, `Jit`, `UserCallbacks`, `ExclusiveMonitor`, `HaltReason`, and the IR types. Pin,
  wrap, and re-vendor on our schedule. Upstream is archived; its fork's API broke incompatibly at v6.7.0.
- **Keep QEMU/Unicorn/binutils strictly out-of-process oracles** (GPL) — and note dynarmic *itself* has
  `DYNARMIC_TESTS_USE_UNICORN` + `multiarch.yml`, i.e. its maintainers already differential-test against the
  same oracle we plan to use. Capstone stays a **test-only** dependency (doc 06's `REUSE.toml` already marks it
  `TEST-ONLY, not linked into the shipped binary`) — so doc 01's "Capstone as a slow-path *fallback decoder* in
  the shipped product" must be **rejected**: a shipped dependency on a disassembler's tables brings the
  binutils-provenance residual risk doc 06 §3 flags, for a code path we should not have.
- **Reject Arm MRA/ASL-generated decoder tables outright** (doc 06 §4: "MRA-derived artifacts may NOT be freely
  redistributed"; "do not ship MRA-generated tables"). This **directly contradicts doc 01 §6**, which calls
  auto-generating from MRA "strategically the right long-term move" and lists "check redistribution" as a
  caveat. It is checked, and the answer is no. Since dynarmic already ships the decoders, the MRA plan is also
  pointless.

### C3 — data-form IR core vs. conventional host-code DBT

**Conflict.** Doc 04 "Core design requirements" (line 616–623) mandates: "Never assume `PROT_EXEC` is
obtainable… The translator's **primary product is data**… direct-threaded dispatch is mandatory in the default
tier", with three `CodeCache` backends. Doc 01 §8 tier 1 and doc 07 §3 assume a conventional host-code DBT and
optimise for it (doc 07 §3.2 wants guest NZCV resident in host NZCV; doc 01 §2 wants QEMU-style TB chaining).
Doc 07 §4 + doc 02 §2.5 assume a fastmem address model whose perf rationale only pays off in a JIT.

**This is a false conflict, and the resolution is unusually clean — because C2 decided it.** Dynarmic's
architecture is *already* "front-end → IR → pluggable back-end": `src/dynarmic/ir/` (~700 micro-ops in
`opcodes.inc`) with **three** independent consumers today (`backend/x64`, `backend/arm64`, `backend/riscv64`,
726 `EmitIR<>` specialisations in the arm64 one). "Lower ARMv7 → IR → {AArch64 codegen | interpreter}" is not a
design we have to invent; it is the design the engine we just chose already has, with one backend missing.

**Where the fork goes (Phase 1 must draw exactly this line).**

*Shared, single copy, platform-neutral:*
- Guest state layout (`A32JitState`-equivalent: r0–r15, `cpsr_nzcv`, `cpsr_q`, IT state, `d0–d31`/`q0–q15`,
  FPSCR, TPIDR words, exclusive-monitor tag) — one struct, both tiers, so they cannot diverge (doc 01 §8.1's
  requirement, satisfied for free).
- Decoder + **instruction semantics**, i.e. `frontend/A32/translate/**` → IR. **This is the crux most people
  miss: in dynarmic the semantics *are* the IR emission. There is no independent per-instruction executor to
  fall back on.** Therefore any interpreter tier that wants to reuse 0BSD-tested ARMv7 semantics **must
  interpret dynarmic's IR** — a "macro-op interpreter over the decoder" is not an option without re-writing
  ~2500 instruction semantics a second time.
- Memory model: fastmem window + `UserCallbacks` fallback + page table (4 KiB granularity) + write-tracking.
- Translation cache: block keyed by guest PC, `InvalidateCacheRange`, `block_range_information`, and —
  critically for iOS — **the block's payload is IR bytes in ordinary `malloc`/arena memory, so IR can be
  produced at import time and *persisted on disk* without any signature or entitlement.** That is Rosetta's
  shape (doc 02 §1.5 precedent) adapted to a no-JIT platform, and it is a product differentiator for iOS, not
  just a workaround.
- Syscall boundary (`CallSVC` → ARM Linux EABI table), signals, kuser page, CP15 emulation, JNI marshalling.
  All tier-independent.

*Fork (one seam, everything below it):*
- Tier **J** (Android arm64, macOS/Catalyst): dynarmic `backend/arm64` → memfd-backed executable code cache
  (`appdomain_tmpfs:file execute` + `self:process execmem`, both verified in C1) → native.
- Tier **I** (iOS, and any Android device/build where JIT is unavailable or undesirable): a **new dynarmic
  backend that walks `IR::Block` and interprets `IR::Inst`** with direct-threaded dispatch over a per-block
  temp array (dynarmic IR is effectively 3-address with a result per inst, so this is a value-stack machine,
  matching libriscv's "3-register machine" argument quoted by doc 04 §3.4). No executable memory anywhere.

**Answering the two named technical risks:**
- **IT-block:** a non-issue at this seam. dynarmic resolves predication in the *front end*
  (`frontend/A32/translate/conditional_state.{h,cpp}`, `ITState.h`) — by the time IR exists, the IT block has
  been expanded into predicated/no-op'd ops and the TB has been split where the architecture requires it
  (doc 01 §7.2's "highest-risk area" is upstream-tested, per C2 evidence #1). Neither tier sees IT state.
- **Lazy flag evaluation:** also resolved above the seam. dynarmic's IR carries flags as *per-subset* ops
  (`SetCpsrNZ`, `SetCpsrNZC`, `SetCpsrNZCV`, `GetCFlag`, …, `A32OPC` entries in `opcodes.inc`) so dead-flag
  elimination happens during translation; **keeping guest NZCV in host NZCV (doc 07 §3.2's plan) is purely a
  Tier-J backend patch and must not change IR**, and Tier-I just keeps `cpsr_nzcv` in the state struct, which
  is dynarmic's *current* behaviour (doc 07 cites `// TODO: Flags in host flags` and a `LDR/MSR` "per block" —
  I verified the comment at `src/dynarmic/backend/arm64/emit_arm64_a32.cpp:27`, and **corrected doc 07's
  sizing**: it sits inside `EmitA32Cond`, so it is paid **per emitted conditional branch**, i.e. per TB exit,
  not once per block. Bigger opportunity than doc 07 stated, and the same TODO is duplicated at
  `emit_arm64_a64.cpp:26`, so a fix is shared with the A64 frontend.) Doc 07's line "that is RetroBug's
  opportunity, not its problem" is correct and it is an *optional* optimisation, deferrable behind a measured
  benchmark. **Do not let anyone put a flag-strategy change in the front end.**
- **Hard requirement falling out of this:** a **mode guard** in the build system, exactly as doc 04 demands
  ("compile-time platform matrix must allow *removing* the two executable backends"). Concretely: the iOS
  CMake target must not compile `backend/arm64/**` at all (dynarmic gates backends by `ARCHITECTURE` in
  `src/dynarmic/CMakeLists.txt:264+`, so this is a small patch, not a rewrite), and CI must build iOS with a
  link-time assertion that no executable-memory symbol resolves.

**Cost of the IR tier — honest estimate, with the numbers doc 04 cites.** Doc 04 §3.4's table says a best-case
threaded/gadget interpreter is ≈ **1/3 of a good JIT** (iSH: gadgets 3–5× faster than `switch` dispatch;
libriscv-class interpreter ≈ 1/3 of QEMU's JIT; iSH-with-JIT would be "an order of magnitude" faster), and
separately cites arXiv 2501.03427 (a *direct* DBT beating QEMU's IR-based TCG by up to **35×**) as a warning
about IR layering. Both readings are compatible with what dynarmic's IR actually is, and the honest conclusion
is: **the 35× warning applies to QEMU's generic, host-agnostic TCG op stream, not to dynarmic's
A32-specific micro-op IR** — dynarmic's IR keeps guest semantics (IT, flag subsets, shift-with-carry) folded
in and its args are typed (up to 4), so the layering tax doc 07/04 fear is much smaller. **But** it is
*micro*-op-grained, and the expansion ratio is the unknown that sets iOS expectations. My estimate:
**~4–8 IR ops per ARMv7 instruction**, giving **Tier I ≈ 0.05–0.12× of native arm64**, i.e. ~8–20× slower than
Tier J — worse than doc 04's "1/3 of a JIT" best case, because dynarmic's IR does a state load/store per micro-op
unless the interpreter keeps a temp array. **Do not commit to a number; measure the expansion ratio in Phase 2
week 1** (instrument dynarmic's `TranslateBlock` over 5 real functions pulled from a shipped `armeabi-v7a`
`.so`; no device needed — this runs on the dev machine, see C7). If the ratio is >10, add an IR→IR "fusion"
pass (superinstructions) *in front of both tiers*, which is the same lever iSH/libriscv use.

**Decision Phase 1 must implement.**
1. One core, one IR (dynarmic's), two executors behind a single `rb::cpu::ExecutionUnit` interface:
   `Run(thread_state) → ExitReason`. Selection is a *runtime* choice and a *build* choice, so Android can fall
   back to Tier I if a device denies execmem or if a user opts into a "no-JIT" diagnostics mode (doc 08's P8
   A/B-without-rebuilds pattern) — this is also the only way to *test* Tier I on Android hardware.
2. Block linking/chaining is Tier-J-only. Tier I dispatches by looking up the next block in the IR cache
   (hash on guest PC). Do **not** pretend there is one dispatch loop.
3. IR cache + block metadata must be **serialisable and instance-tagged** (needed for iOS import-time AOT-to-IR
   and for doc 05's per-instance code cache; see C5).
4. Both tiers must pass the *same* differential suite (C7), and the iOS build must be compiled with Tier J
   absent, per doc 04.
5. Rejected: doc 04's literal "three `CodeCache` backends {AnonExecMmap, DarwinMapJit, IrInterpreter}" as a
   *CodeCache* framing — `IrInterpreter` is not a code cache, it is an executor. Get the abstraction names right
   in Phase 1 or the architecture will conflate memory provisioning with execution strategy, which is exactly
   the mistake that produced C1.

### C4 — graphics backend strategy per platform

**Conflict as posed.** Doc 03 (TL;DR 50–62, §6.2, "Backend strategy recommendation" 871–893) rejects "Vulkan as
the single host backend" and wants GLES forwarding on Android. Doc 08 reports the developer's *proven*
Vulkan→MoltenVK work in Vita3K/xemu, and doc 03 itself routes iOS through ANGLE's Metal backend. So: two
backends per platform, or one path everywhere?

**Adjudication: the conflict is mostly a category error, and dissolving it is the decision.** The two "sides"
are about different things:

- Doc 08's Vulkan experience is about a **host-side renderer for an emulator whose front end already speaks
  Vulkan** (xemu/Vita3K *produce* Vulkan). RetroBug's guest does not: `armeabi-v7a` Android apps speak
  **EGL + GLES 1.x/2.x/3.x** (doc 03 §1.1), and guest `libvulkan.so` is an explicit separate case (doc 03
  §1.2: vendor/mythical libs — almost nothing links it in the 32-bit era). So the developer's proven skill
  transfers to (a) RetroBug's iOS Vulkan path and (b) *nothing else on Android*. It is not evidence for
  "make Vulkan the host abstraction".
- Doc 03's rejection is sound and the reason it gives is the right one, but the *decisive* argument is one it
  only half-makes: a GLES→Vulkan→driver layer in front of a host driver that already implements GLES adds a
  translation **whose correctness surface is the fixed-function-emulation problem doc 03 §2.2 quantifies**
  (ANGLE gates shaders on a 13-bit enable mask ⇒ up to 8192 variants) — that is a large amount of work
  RetroBug would be signing up to own forever. Forwarding to the host's GLES costs ~0 and inherits the vendor's
  own GLES 1/2/3 conformance.
- The one place the "two-backend" framing is wrong is **the abstraction boundary**: RetroBug must not have
  "a GLES backend and a Vulkan backend". It needs **one guest-facing proxy layer** (generated from AOSP
  `native_bridge_support/android_api`, which doc 03 §1.6 and doc 06 §2 independently confirm ships genuine
  `arch: { arm: … }` variants for `libEGL`/`libGLESv1_CM`/`libGLESv2`/`libGLESv3`/`libOpenSLES`/`libvulkan`…)
  and **two host executors selected per guest API**, not per platform:
  `GLES-on-host-GLES` (Android default) · `GLES-on-ANGLE-Metal` (iOS default) · `GLES-on-SwANGLE` (deterministic
  reference + sandbox tier, see C5) · `Vulkan-on-host-Vulkan` (Android, rare) · `Vulkan-on-MoltenVK` (iOS).

**Verdict / DECISION.** Two host *families*, one seam, **five** selectable executors, chosen by a per-instance
data-driven setting (App Revival), never by hardcoded package name:

| Platform | Guest GLES 2/3 | Guest GLES 1.x | Guest Vulkan |
|---|---|---|---|
| Android arm64 host | forward to host driver (`libGLESv2.so`/`libGLESv3.so`) | **forward to host `libGLESv1_CM.so` by default, SwANGLE/ANGLE fallback on artefact mismatch** (see caveat below) | forward to host `libvulkan.so` |
| iOS / visionOS | ANGLE `renderer/metal` | ANGLE `GLES1Renderer` (same code path) | MoltenVK, with `vkCreateInstance` forced to set `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` (doc 03 §6.1) |
| Anywhere, CI + sandbox | SwANGLE (ANGLE→SwiftShader, Apache-2.0) — CPU-only, deterministic | same | n/a |

**Two caveats I must put against doc 03, because it is load-bearing and I could not confirm them:**
1. **The CDD citation.** Doc 03's "forward on Android is safe because the Android 16 CDD still mandates GLES
   1.1" (`[C-1-1]`) is the premise of the whole Android column. I fetched the Android 16 CDD page and **could
   not extract the graphics section** (the fetch returned only a Vulkan 1.1 statement), so I am *not* able to
   confirm the GLES-1.1 mandate — recorded as **U-4**, cheap to settle by grepping `android-cdd.pdf` for
   "OpenGL ES". Why it matters: **the counter-evidence is in AOSP itself.** `packages/modules/ANGLE/Android.bp:67-90`
   (fetched) ships `libGLESv1_CM_angle` as a **Mainline prebuilt with `arm` *and* `arm64` variants**, depending
   on `libGLESv2_angle` which depends on `libvulkan` (`:108-110`). Google does not ship an ANGLE GLES 1.1
   implementation for fun; it ships one because a meaningful slice of real drivers cannot serve GLES 1.x
   anymore. So RetroBug must treat GLES 1.x-on-native-host as **best-effort with an automatic, observable
   fallback**, not as a guaranteed forward.
2. **The per-app ANGLE escape hatch is probably not RetroBug's to pull.** Doc 03 proposes "ANGLE as a per-app
   drop-in replacement host backend — reusing `libEGL_angle`/`libGLESv1_CM_angle`/`libGLESv2_angle` exactly as
   Android itself uses them". AOSP's ANGLE selection is made by the *platform's* `libEGL` inside the calling
   process, driven by platform-owned rules keyed on the app's own package (and RetroBug is one package hosting
   N guests), and `libGLESv*_angle.so` are `product_specific` modules that are **not NDK/public libraries**, so
   an ordinary app `dlopen` of them is expected to fail namespace checks. **U-5** — test by `dlopen("libGLESv2_angle.so")`
   from an app. If it fails (likely), the *portable* answer for RetroBug is the one it needs anyway: **vendor
   ANGLE/SwANGLE ourselves as a static or bundled host executor**, which is also what the sandbox tier and CI
   need (C5). That converts a possible gap into a reason to own the software renderer, and it is the same
   component either way.

**Rejected:** "one ANGLE-on-Vulkan-ish path everywhere" — it forces RetroBug to own GLES 1.x/2.x/3.x emulation
correctness on Android where a conformant native path exists, and doc 03 §6.1's MoltenVK portability caveats
mean it does not even buy iOS uniformity. **Rejected:** "Vulkan-only host backend" for the same reason plus it
buys nothing for a guest whose API surface is EGL/GLES.

### C5 — `isolated_app` has no `gpu_device`; cost and tiering of isolation

**Doc 05's position** (line 484, 503, 528–530): one bound `android:isolatedProcess` service per instance; host
process is resolver/fd-broker/only GPU holder; sandbox has "NO gpu_device, NO non-AF_UNIX socket creation, NO
app_data_file open — all neverallow-enforced"; guest GL commands ride a ring into the host which replays them.

**Verification I did myself** (sepolicy @ main, exact lines):

| Fact | Line | Note |
|---|---|---|
| `neverallow { isolated_app_all -isolated_compute_app } gpu_device:chr_file { rw_file_perms execute };` | `private/isolated_app_all.te:73` | This is a **neverallow**, not merely a missing grant — no OEM policy and no targetSdk escape hatch can open it. Doc 05 quotes it correctly; the added precision is that it is unfixable-by-config. |
| `allow { appdomain -isolated_app_all } gpu_device:chr_file rw_file_perms;` + `binder_call({ appdomain -isolated_app_all }, gpuservice)` | `private/app.te:393-395`, `:407` | The exclusion is by *set difference* — the default-deny shape. |
| `neverallow { isolated_app_all -isolated_compute_app } { service_manager_type -activity_service -display_service -webviewupdate_service }:service_manager find;` | `private/isolated_app_all.te:65-70` | **Stricter than doc 03/05's summaries suggest: the sandbox cannot find `audio_service`, `SurfaceFlinger`, `media.player`, `window`, `clipboard`, `notification`, `sensor_service`…** Only AMS, DisplayManager and webviewupdate. Every Android service the guest touches must be proxied by the host process. |
| `neverallow isolated_app_all app_data_file_type:file open;` ("Isolated apps should not directly open app data files themselves.") | `private/isolated_app_all.te:42-43` | Doc 05 has this one (line 506). Verified verbatim. `apk_data_file` is *not* `app_data_file_type` (`public/file.te:381` vs `:517`), so `/data/app/**` **is** openable and executable from the sandbox — which is how RetroBug's pre-shipped agent `.so` still loads there (C1 cross-ref), and why doc 05's "copy the APK into the instance" rule is a *privacy* choice, not an OS requirement. |
| `allow appdomain self:process execmem;` | `private/app.te:189` | CONFIRMED at the line doc 09 cites; `app_domain(isolated_app)` (`private/isolated_app.te:12`) means the sandbox keeps JIT. |
| `neverallow { bluetooth isolated_app_all nfc radio shared_relro sdk_sandbox_all system_app } { data_file_type -apex_art_data_file -dalvikcache_data_file -system_data_file -apk_data_file }:file no_x_file_perms;`, `no_x_file_perms = { execute execute_no_trans }` | `private/app.te:731-746`, `public/neverallow_macros:5` | New, and it *helps* RetroBug: the sandbox provably cannot execute anything it can name by path in `/data`. Combined with `appdomain_tmpfs:file { execute … }` (`public/te_macros:245-247`) the sandbox's only executable memory is its own memfd/anon arena — i.e. **the translated-code cache is architecturally forced to be a sandbox-created memfd**, which is what doc 05 wants for instance tagging. |

**Cost assessment (the question asked).** Graphics-over-IPC is *not* a pixel-shipping problem and Phase 1 must
say so loudly, because naive pixel shipping is the first thing an engineer will try and it is dead on arrival:
1280×720 RGBA at 60 fps ≈ **220 MB/s of copies**, plus a Binder round-trip per frame, on a bus (binder) whose
per-transaction size limit is ~1–2 MB and which will not tolerate 60 transactions/s of that size. So the
mechanism must be one of:
- **(A) command channel** (doc 05's plan): guest GL commands → shared ring in the instance root (host-created
  file, fd-brokered — the *only* sanctioned direction per `isolated_app_all.te:12`'s comment) → host replays on
  the real backend; buffer objects stay host-side and are named by a 32-bit guest handle aliasing a host id
  (doc 03 §3.3's handle/fd/fence table is already the design). Cost: an IPC hop per swap + a real serialised
  command-stream subsystem (this is VirGL/Wayland-class work, roughly the size of RetroBug's JNI marshalling
  layer, and it must be *reliable*, because a desync in a command stream shows up as corruption, not a crash).
- **(B) in-sandbox software renderer: vendored SwANGLE (ANGLE→SwiftShader, Apache-2.0).** No `gpu_device`, no
  service discovery, no ring, no second product — SwiftShader is pure CPU and needs only the code+data already
  in the sandbox. Cost is fps (a 2010-era GLES 2.x app at 720p on 4 big CPU cores behind a translator is
  plausibly 5–20 fps), and ~10–20 MB of shipped binary.
- **(C) hybrid, and the recommended one:** default **(B)** for the isolated tier (it is honest, simple,
  verifiable, and doubles as the CI reference renderer per C4 — **one component serving three purposes**),
  with **(A)** as an *opt-in* "hardware graphics in isolated environment" mode that Phase 2 can defer entirely.
  `AHardwareBuffer`-fd sharing stays behind a capability probe (doc 05's T-36) because the dmabuf heap is a
  `chr_file` this domain has no claim on.

**Does this invalidate "same CPU core, sandbox only FS/network/config"?** No — and this is the important
structural reading: the CPU core *is* reused unchanged (doc 05's own topology puts the vCPU + IR/JIT arena in the
sandbox). What the `gpu_device` neverallow invalidates is the claim that the **runtime layer** is
"Android-shaped" in one place: RetroBug's host-side GL/EGL/audio/Window executor must be an addressable,
independently-hosted component with a serialised protocol in front of it, i.e. the runtime layer is
**split-host**, and its proxy libs must be buildable against a transport that is either in-process (normal tier)
or ring-based (isolated tier). Phase 1 must therefore specify the graphics/audio surface as an
**`RbGfxChannel` interface with two implementations from day one** — `InProcessChannel` and `RingChannel` —
otherwise the isolated tier becomes a fork of the renderer. The second real cost is that **the sandbox cannot
reach any Android system service** (`isolated_app_all.te:65-70`), so *all* of doc 02's `Context`/`Binder`
proxying must live in the host process: the isolated tier moves the Trusted Computing Base from "RetroBug's
monitor" to "RetroBug's host process", exactly as doc 05 line 506 states.

**Tiering decision (brief (c) + (h)).** Three tiers, and the naming discipline is not cosmetic — it is the
compliance mechanism:

| Tier | Subject | Enforcement | May be called | Network |
|---|---|---|---|---|
| **T0 normal** | guest in RetroBug's own process, own profile dir, monitor-mediated FS/Settings | advisory (complete mediation by RetroBug, bypassable by a RetroBug bug) | "**separate profile**" — never "isolated", never "sandbox" | the *host app's* network; not a per-guest claim |
| **T1 isolated** | one fresh `isolatedProcess` service per instance | **OS-enforced**: UID 99000+ band, SELinux domain (`gpu_device`/`service_manager`/`app_data_file open`/socket-family all neverallowed), plus RetroBug's own `SECCOMP_FILTER_FLAG_TSYNC` filter as a backstop (doc 05 §2.6, `seccomp` is in `SECCOMP_ALLOWLIST_COMMON.TXT`) | "**isolated environment**" | **off by default**; opt-in cannot be granted by T1 itself — the host's network is not the sandbox's |
| **T2 (later)** | app zygote / `:slotN` pre-warm | not per-instance | must never be presented as isolation | — |

**In-process monitor-only isolation as a "weaker, labelled" tier: ACCEPTABLE, with three conditions, and it
does not violate (c)/(h) *provided* those conditions hold.** Rationale against the brief's own words: (c)
requires the isolated environment to be a *distinct subsystem* with per-app-scoped FS/network/settings, network
off by default, never default and never a silent fallback — it does **not** say "OS-enforced"; (h) requires
claims to be *actively verified*. So:
1. **Tier names must not overclaim** — "isolated" is reserved for T1 and is asserted *only* after a runtime
   self-test (start the service, `Binder.getCallingUid()` ∈ [99000, 99999], prove `open()` of its own
   `files/` path fails `EACCES`/avc, prove `socket(AF_INET)` fails, prove a probe file in another instance's
   root is invisible). This is doc 05's leak-probe APK; it becomes a **release gate, not a test suite**
   (brief (h)).
2. **T0 must be entered only by explicit user choice with the difference spelled out in one sentence** ("this
   keeps the app's files and settings separate from other apps in RetroBug, but a bug in RetroBug, or a guest
   that reaches the host directly, can cross that line; the isolated environment uses Android's own process
   sandbox"). Never as a fallback after a T1 failure — doc 05 line 554's structural rule (guest loader lives in
   the sandbox and cannot be instantiated in-process; T-37 asserts it) is the right shape and Phase 1 must keep
   it.
3. **Every isolation claim must name its enforcer.** FS/config/network claims in T1 are enforced by the kernel
   + SELinux; "no cross-instance sharing" in T0 is enforced by RetroBug's own resolver. Doc 05's own TCB
   sentence (line 506) is the template for the user-facing wording.

**Disclosure per tier — what the user must see.** T0: "profile separation, not OS isolation". T1: "runs in a
separate Android process with its own user ID; no network unless you turn it on; no hardware 3D (software
renderer) unless you opt into the experimental hardware channel". And one item nobody has written down:
**T1's cold start and fps are materially worse** (no `gpu_device`, no service discovery, ring-copy latency), so
the UI must not present T1 as "the same app, but safer" — it is a different performance contract, and (f)
"zero configuration for the common case" applies to T0, not T1.

### C6 — guest page model (4 KiB guest pages vs 16 KiB host pages) and fastmem

**Positions.** Doc 01 §1 treats hardware AArch32-at-EL0 as an "opportunistic accelerator" and §7.6/§7.9 as
raising the `0xffff0000` and 4 KiB-vs-16 KiB problems. Doc 02 line 78 lists "16 KB-page hosts vs 4 KB-aligned
32-bit ELFs" as risk #3. Doc 09 §5 recommends "fix guest page size at 4 KB, do not make it configurable,
report 4096 in `AT_PAGESZ`" and (line 167) concludes RetroBug "must have a shadow page table" with
host-protection-union + software enforcement, calling it "a real, non-trivial subsystem". Doc 07 §4.3 wants an
identity-mapped low 4 GiB reservation, and §4.3.8 repeats the shadow-table requirement for 16 KiB hosts.

**Adjudication.**

1. **Guest page size: FIXED 4 KiB. Decided, not configurable.** Adopt doc 09 §5.5(5) verbatim. This is now
   *forced* rather than merely recommended, because C2 chose dynarmic: `UserConfig::PAGE_BITS = 12` and
   `NUM_PAGE_TABLE_ENTRIES = 1 << (32 - 12)` (`interface/A32/config.h:156-158`) — the engine's page table is
   4 KiB-granular by construction, with a 2^20-entry table covering the whole 32-bit guest space. A
   configurable guest granule would mean changing the engine's core memory abstraction for a capability nobody
   needs. Report `AT_PAGESZ = 4096` in the guest auxv (doc 02 §5.5), answer `sysconf(_SC_PAGESIZE)` = 4096
   from the guest libc, and keep `HostPageSize()` (`getauxval(AT_PAGESZ)`) strictly separate from
   `GuestPageSize()` per doc 09 §5.5(3) — with a **lint rule**, not a code review norm, since the failure mode
   is silent corruption on Pixel 8/9-class 16 KiB devices.
2. **The protection-granularity problem is smaller than docs 07/09 think, because of `page_table` +
   `recompile_on_fastmem_failure`.** The requirement is real: a 16 KiB host page covers four guest pages, so a
   guest `mprotect(4 KiB subrange)` cannot always be expressed on the host. Docs 07/09 respond by designing a
   shadow-PTE + dirty-tracking + bounds-checked-slow-path subsystem. But dynarmic already has the exact
   mechanism: a per-4 KiB host-side table where a `nullptr` entry routes accesses through
   `MemoryRead*/Write*` callbacks, plus `recompile_on_fastmem_failure` and
   `check_halt_on_memory_access` (`config.h:176-199, 225`). **Decision: for any host page whose four guest
   pages do not share one protection set, drop those 4 KiB pages out of fastmem (null their table entries) and
   let the callback path enforce the guest's own 4 KiB permissions in software.** That is a ~200-line policy
   inside RetroBug's memory manager, not a subsystem, and it degrades speed only on the mixed pages (rare:
   text/data adjacency inside one 16 KiB granule). Keep the shadow permission table (it is needed for
   `mprotect` bookkeeping and SMC invalidation anyway) but **do not** build a generic soft-TLB.
3. **Guest ELF loading is unaffected.** RetroBug's guest linker maps segments itself; `p_align < host page` is
   handled by mapping the whole segment span at host granularity and applying guest protections per 4 KiB
   entry as in (2). Doc 02 §4.3's "reuse the real guest linker" plan is untouched.
4. **`0xffff0000` disappears as a problem under the C2 decision.** Doc 01 §7.6 calls the kuser page a "genuine
   32-on-64 trap"; with a flat 32-bit guest window (`fastmem_pointer`, or the 4 KiB page table with
   `absolute_offset_page_table`) guest `0xffff0000` is just guest memory inside our reservation. RetroBug
   synthesises the five documented helpers (`__kuser_get_tls` @ `0xffff000e`, `__kuser_cmpxchg` @ `0xffff0020`,
   `__kuser_cmpxchg64` @ `0xffff0060`, `__kuser_memory_barrier` @ `0xffff00f0`, the two signal trampolines) as
   **A32 guest code**, which the translator then executes like any other guest code — maximum fidelity, zero
   special-casing. (Modern bionic uses `MRC p15,0,Rt,c13,c0,3` for TLS instead, which C2 covers via the
   CP15 `Coprocessor` word-address hook; keep both because old guests use the kuser sequence.)
5. **Hardware AArch32: REMOVE from the v1 architecture, and the probe as doc 01 specifies it is impossible.**
   Doc 01 §1.2's recommended detection is "`fork()+execve()` a tiny static ARM ELF **we ship in the APK's
   `assets/`**". Verified consequence of C1's rules: `assets/` unpacks to `app_data_file`, and
   `neverallow … { app_data_file privapp_data_file }:file execute_no_trans`
   (`private/app_neverallows.te:69-73`) makes `execve` of it a guaranteed `EACCES` for a modern-target app —
   **the probe cannot succeed even on a device with AArch32 EL0.** The only app-executable-by-path location is
   `apk_data_file` (`private/app.te:441-443`, incl. `execute_no_trans`), i.e. RetroBug's own extracted
   `lib/armeabi-v7a/` directory — which on a 64-bit-only device the package manager may not extract at all, so
   the probe's own payload is conditional on the thing being probed. **Decision: no AArch32 path in v1, not
   even as an accelerator** (three independent reasons: doc 01 §1.4's target-mismatch argument; a second
   32-bit process cannot share RetroBug's in-process guest address space, JNI tables, VFS or code cache, so it
   is a *different product* that would fork the runtime layer; and it is unavailable on iOS, which constraint
   (d) makes a first-class target). Keep the *capability report* (read `ro.product.cpu.abilist32`, plus, if
   ever implemented, a probe relocated to `jniLibs/armeabi-v7a/`) as **diagnostics only**, feeding doc 08's P7
   "diagnostics-first" rule — never as a code path.
6. **The 4 GiB identity reservation stays the plan, but its two prerequisites must be recorded as unverified**
   (U-2, U-3): (a) `mmap(0, 4 GiB, PROT_NONE, MAP_NORESERVE|MAP_FIXED_NOREPLACE)` succeeding *inside a
   zygote-forked Android app process* on a 39-bit-VA kernel; (b) the same on iOS, where doc 07 §4.3.2 itself
   calls it unproven. Both are testable before any Android device exists (see C7), and the fallback (biased
   `GUEST_BASE` + low arena for all host→guest pointers, doc 07 §4.3.3) is already designed — but if (a)
   fails, RetroBug ships in biased mode and **every** pointer-crossing in doc 02 §2/§3 must be re-audited, so
   it must be measured in week 1, not discovered in month 6.
7. **Code-cache provisioning is NOT what doc 09 recommends, and Phase 1 must know it.** doc 09's "fallback
   ladder" ranks `memfd_create` + `MAP_SHARED` dual-alias (ART's mechanism) as **primary**, "because it is what
   ART ships". dynarmic's AArch64 backend does not do that: the code buffer is an `oaknut::CodeBlock`
   (`backend/arm64/address_space.h:73`), i.e. allocation lives in the external oaknut library (vendored as a
   submodule; exact syscall sequence to be read — U-6), while dynarmic's **x64** backend has an explicit
   W^X path (`backend/x64/block_of_code.cpp:128`: `is_executable ? PROT_READ|PROT_EXEC : PROT_READ|PROT_WRITE`,
   `mprotect` at `:129`, `MAP_JIT` at `:92-93` under `DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT`). Android permits the
   simple shape (`allow appdomain self:process execmem`, `private/app.te:189`, verified), so **v1 = keep
   oaknut's allocator behind RetroBug's own `RbCodeCache` seam**; the memfd dual-alias is the *hardened*
   variant to swap in if an OEM policy or future `PR_SET_MDWE` ever bites. Making the seam explicit in
   Phase 1 is mandatory — otherwise the code-cache substrate is unreplaceable and C5's instance-tagging
   requirement (one cache per environment) has nowhere to live.

**DECISION for Phase 1.** One constant `RB_GUEST_PAGE_SHIFT = 12`, one host accessor `RB_HOST_PAGE_SHIFT` read
at init; fastmem = flat 32-bit window with 4 KiB table; mixed-protection granules leave fastmem rather than
gaining a soft-TLB; kuser page synthesised as guest code; no AArch32 code path in v1; code cache behind
`RbCodeCache` with `AnonRwx` (default, oaknut-shaped) and `MemfdDualAlias` (hardened) implementations.

### C7 — what is verifiable before any device exists

**Doc 10's finding** (§5, measured on this machine): x86_64 Windows, NDK r28/r29 + JDK 21 + build-tools
present, AVD boots on an `android-37.1 google_apis_playstore_ps16k x86_64` image whose `build.prop` says
`abilist=x86_64,arm64-v8a`, **`abilist32=` empty**, `ro.dalvik.vm.native.bridge=libndk_translation.so`; no
device attached; no WSL, no Docker, no `qemu-arm`, no Python (Store stub only). Conclusion drawn: "Milestone 1
must be validated on a physical 64-bit-only Android arm64 device" and "Do not add a 'run on the emulator' step
to the plan."

**Adjudication: that conclusion is right about the *arm64 JIT tier* and wrong about milestone 1, because it
was written before C2 existed.** Doc 10 rejects the emulator with the argument "running RetroBug's AArch64 code
generator on top of `libndk_translation` … would mean translating a translator — slow, and ndk_translation is
not a JIT-safe target." True. **But dynarmic — the engine C2 just selected — ships a first-class, upstream-CI-
tested x86-64 back end for the same A32 front end** (`backend/x64/**`, `DYNARMIC_FRONTENDS` default
`"A32;A64"`, and `.github/workflows/x86-64.yml` is the primary lane). Therefore:

> **Build RetroBug's engine so that the *host back end* is selected by the same mechanism that selects the
> execution tier (C3), and ship a `x86_64` debug variant of RetroBug's own APK.** On the local emulator the
> engine then runs **natively** (x86-64 codegen, no ndk_translation involvement), and the *guest* is still
> ARMv7 — so the entire runtime-integration layer becomes device-free.

That single decision converts most of "needs hardware" into "needs an emulator I already have":

| Milestone element | Locally verifiable **today** (x86_64 Windows) | On the local **x86_64 emulator** with an x64 engine build | Needs real hardware |
|---|---|---|---|
| dynarmic vendored + builds (C2) | ✔ CMake/Ninja/NDK present | ✔ | — |
| A32/T32 decode + IR translate, IR interpreter (Tier I) | ✔ pure C++, runs on the host | ✔ | — |
| AArch64 codegen (Tier J) correctness | △ build + `llvm-objdump -d --triple=aarch64` invariants only (doc 10 §6.2) | ✔ **executed natively if the CI runner is arm64** (`ubuntu-24.04-arm`, free — doc 10 §6.4.6) | only for *perf*, not correctness |
| Differential semantics vs `qemu-arm` linux-user | ✗ **not on bare Windows**: `qemu-*` user-mode is Linux-only and WSL is absent (doc 10 §5) → install WSL2 or move the loop to a Linux CI worker | ✔ (same Linux requirement) | — |
| Golden vectors: QEMU `tests/tcg/arm/*` + `.ref` (doc 10 §6.3) | △ same Linux caveat | ✔ | — |
| Guest ELF loader / relocations / guest linker / proxy-lib ABI conformance (doc 10 §6.4.4) | ✔ as host unit tests | ✔ | — |
| Syscall ABI table + `ARM_NR_*` + fabricated `sigcontext` | ✔ | ✔ | — |
| **C1's JNI mechanism** (agent `.so` in `apk_data_file`, `findLibrary` override, host-side `RegisterNatives`, `InMemoryDexClassLoader`) | △ unit-testable as a plain JNI sample | ✔ **fully: this is AOSP `untrusted_app` sepolicy + real ART** | OEM variance only |
| 4 GiB low reservation (U-2) | △ not representative | ✔ | confirm on 2–3 real devices |
| `execmem` / memfd-exec / `app_data_file` non-exec (C1, C5, doc 09) | ✗ | ✔ (`untrusted_app` + `isolated_app` on AOSP policy) | ✔ OEM variance (doc 09 OQ-6) — 6-device matrix |
| **`isolated_app` leak-probe matrix** (doc 05's T-01…T-38) | ✗ | ✔ mostly — an emulator *is* `isolated_app`-capable AOSP policy | vendor ROMs |
| GLES forwarding / ANGLE / SwANGLE (C4) | △ `llvm`-based shader tools + SwiftShader CPU path | ✔ | fps and driver bugs |
| Activity hosting, hidden-API bypass (doc 02 §6.3) | ✗ | ✔ (Android 16 image, and the emulator is exactly where a bypass breaks first) | some OEMs differ |
| Real app `.so`s from the corpus (doc 10 §3) | ✔ extracted + parsed offline | ✔ | — |
| RetroBug's own 16 KiB-page compliance | ✔ NDK r28 default flags + `zipalign -P 16`; the installed image **is** `page_size_16kb` | ✔ | Pixel-class devices for the real matrix |

**Consequences Phase 1/2 must encode.**
1. **`RbCodeCache` + host-backend selection are architectural requirements *for testability*, not
   portability polish.** The engine must compile for `x86_64-android` and `aarch64-android` from day one; if
   the x64 path is deferred, milestone 1 has no local proof at all.
2. **Install WSL2 (or pin a Linux CI lane) before any semantics work starts** — it is the only way `qemu-arm`
   runs, and per C8 (below) the external oracle is now load-bearing in a way it was not under the
   "interpreter is the reference" plan.
3. **The one thing no amount of local work can substitute for**: Tier-J *performance* and OEM *policy variance*.
   Budget exactly one 64-bit-only arm64 phone (Snapdragon 8 Gen 3 / Dimensity 9300 class — AArch32-absent in
   silicon per doc 01 §1.1, so it also proves the accelerator is dead) and buy it at the start of Phase 2,
   not the end.
4. **Do not test RetroBug's arm64 build on the emulator via ndk_translation** (doc 10's warning stands):
   translated execution of a runtime-generated code cache is not a valid signal about either.

### C8+ — additional contradictions found in this pass

**C8 — the project's central correctness strategy is invalidated by the C2/C3 decisions, and no doc notices.**
Doc 01 §8.2 makes the interpreter "the **reference model / fallback** the JIT never trusts blindly" and §8.6
proposes fuzzing "interpreter vs JIT vs ASL oracle". Doc 07 §4.3(7) states the rule to keep: "the interpreter is
always correct… an invalidation bug degrades to slow, not wrong". Doc 10 §6.4.1 builds the harness on it:
"(a) the interpreter (**reference semantics, always correct**), (b) the DBT". All three assume the two tiers
have **independently written semantics**. After C2+C3, Tier I and Tier J consume the *same* A32→IR lowering —
so **agreement between them proves nothing about ARMv7 semantics**; it only proves the two back ends agree with
the front end's (possibly wrong) model. A dynarmic front-end bug (e.g. its self-declared "FPSR state is
approximate", or "misaligned loads/stores are not appropriately trapped in certain cases", README
"Disadvantages") would be **invisible** to the planned differential suite.
**DECISION:** the *only* semantic oracle is **external**: `qemu-arm` linux-user as a subprocess (GPL-clean by
construction — process boundary, no linking; doc 10 §6.3 already sanctions it), plus real AArch32 hardware
where available (CI arm64 runners may or may not expose AArch32 EL0 — doc 10 §6.4.6 flags it). The I-vs-J diff
is **relabelled** in Phase 1 as a *back-end equivalence* test (which is still valuable: it is exactly the test
that makes Tier I shippable on iOS). Sequencing consequence: **the QEMU subprocess harness is a week-1
deliverable, before tier work**, not a Phase-3 review nicety.

**C9 — doc 02's milestone-1 mechanism cannot run inside doc 05's sandbox.** Doc 02 §6.2/§1.2 loads guest DEX
with `PathClassLoader` on an extracted APK and hooks `findLibrary`. Verified rule:
`neverallow isolated_app_all app_data_file_type:file open;` (`private/isolated_app_all.te:42-43`, comment: "Isolated
apps should not directly open app data files themselves.") — so an `isolated_app` **cannot open the extracted
APK by path**, hence cannot construct a `PathClassLoader`/`DexClassLoader` over it at all. Two further
consequences of the same rule set: (a) guest `.so` text **cannot be `mmap(PROT_EXEC)`-ed from a brokered file
fd** either, because `isolated_app_all` is neverallowed `execute` on `data_file_type` outside
`apk_data_file`/`dalvikcache_data_file`/`system_data_file`/`apex_art_data_file` (`private/app.te:731-746` +
`public/neverallow_macros:5`) — so doc 02 §4.3(A)'s "run the *real* AOSP 32-bit linker as translated guest
code", whose `mmap(PROT_READ|PROT_EXEC, fd, MAP_PRIVATE)` of each `.so` is the normal bionic path, must be
rewritten in the sandbox as **copy-into-anon-then-map-RX**; (b) the whole `findLibrary`+shadow-lib idea (already
killed by C1) is killed a second time here, independently.
**DECISION:** T1 uses `dalvik.system.InMemoryDexClassLoader` over an `appdomain_tmpfs` memfd written by the
host and passed over Binder (public API since API 26, needs no path open), with natives bound by RetroBug's own
`RegisterNatives` (C1) — which is *why* C1's resolution is the right one: **it is the only JNI mechanism that
works identically in T0 and T1.** Phase 1 must state that the DEX-loading and native-binding layers have two
backends (path-based for T0, fd-based for T1) and that the fd-based one is the conformance target.

**C10 — a kernel feature one doc relies on is denied by another.** Doc 09 §5(2) specifies guest W^X handling as
"trapping writes via **`userfaultfd`**/`mprotect` on the coarse host granularity". Doc 05 §2.6/§Recommended
(line 534, and its own table row at line 74/246) puts `userfaultfd` in the **`ERRNO(EACCES)` list of RetroBug's
own seccomp filter in the sandbox process** (correctly, as a hardening measure: it is a known kernel-attack
surface, and doc 05 line 74 notes it is reachable by default today). So the SMC/protection mechanism differs
between tiers by construction, undocumented.
**DECISION:** do not use `userfaultfd` at all. Use what C2 gives us instead: dynarmic's write-tracking
(`block_range_information`), `check_halt_on_memory_access`, `recompile_on_fastmem_failure`, plus nulling the
4 KiB page-table entries for mixed granules (C6.2). One mechanism, both tiers, and it is also the one that
works on iOS, where doc 04 §"Core design requirements" (line 638) explicitly demands SMC detection *not* rely
on host faults. Amend doc 09's §5(2).

**C11 — doc 01's AArch32 capability probe is itself sepolicy-blocked.** See C6.5. Recorded separately because
it is the kind of error that survives review: it is a *detection* mechanism that cannot fire on the devices
where the answer is "yes", so it would have produced a permanent false negative and a misleading device report.
Related and also worth stating: doc 01 §1.2's alternative ("`MRS` of `ID_AA64PFR0_EL1` from EL0 is trapped and
sanitised") is correct, so on Android the *only* honest capability signal is `ro.product.cpu.abilist32` plus a
"did the guest need this" question that v1 no longer asks (no AArch32 path).

**C12 — two licensing statements conflict with reuse recommendations (brief's category iii).**
- **Capstone in the product**: doc 01 §6 sanctions Capstone "as a slow-path **safety net**" in the shipped
  engine; doc 06 §3 flags its ARM tables' binutils/GAS lineage as a residual `UNVERIFIABLE` risk and its own
  `REUSE.toml` marks Capstone `TEST-ONLY, not linked into the shipped binary`. → **Test-only. No shipped
  dependency.** After C2 there is no use case for it in the product at all.
- **Arm MRA/ASL**: doc 01 §6 calls MRA-generated decoders "strategically the right long-term move"; doc 10
  §6.3 lists MRA as a vector source and §6.4.3 says to fuzz "**from the MRA-derived decode tree**"; doc 06 §4
  concludes "MRA-derived artifacts may **NOT** be freely redistributed" and "do not ship MRA-generated tables".
  → **Banned as an artifact source.** Replace with: dynarmic's decoders (0BSD, already ours per C2) as the
  implementation, and **LLVM `llvm-objdump`/MC (Apache-2.0 WITH LLVM-exception — already installed on this
  machine, doc 10 §6.1)** as the independent encoding oracle. That also removes the `gas/testsuite` GPLv3
  temptation. If MRA is consulted at all, it is a *human reading aid* (doc 06 §4's own wording: "your reading
  of the prose + encoding diagrams — NOT a transcription").
- **gl4es**: doc 06's dependency table describes it as "GLES2-on-GL1/3 shims"; doc 03 §1.5/TWICE-corrected line
  29 establishes the direction is desktop-GL→GLES, i.e. it is the *wrong shape* for RetroBug and useful only as
  a fixed-function reference. No licence conflict (MIT both say); just a wrong role, and it should not appear in
  RetroBug's `NOTICE.md` if it is never linked.
- **Berberis**: doc 02's "fork Berberis" vs doc 06's "no AArch32 translator, Soong-only" — resolved in C2; what
  survives is the `native_bridge_support/android_api` **shim/ABI** reuse, which docs 02 §7.3, 03 §1.6 and 06 §2
  *independently* recommend and which is Apache-2.0. **This is the strongest cross-doc agreement in the whole
  phase and Phase 1 should treat it as settled**: guest proxy libraries come from that generator, not from
  hand-written stubs. Caveat both docs 02 and 06 miss: doc 02 line 1120-1124 already flags "whether arm32
  `libnative_bridge_guest_*` still builds on `main`" as unverified → **U-7, and it is a week-1 build
  experiment**, because if the arm32 variant is stale RetroBug must revive it (a maintenance liability, not a
  blocker — Apache-2.0 lets us).

**C13 — "`:guestN` services" are recommended as isolation by one doc and disqualified as isolation by
another.** Doc 02 §6.2 row 1: "declare ~4-8 `android:process=":guestN"` services in RetroBug's manifest and pin
each guest app to one. **This is the clean, non-fragile way to get process isolation without root.**" Doc 05
line 484: "`:slotN` (§2.3) is **disqualified**" — because the pre-declared processes share RetroBug's UID and
SELinux domain, so no kernel-stamped instance token exists. **DECISION:** `:guestN` is a *lifecycle/crash
containment* mechanism (valuable: it keeps a guest SIGSEGV or OOM from taking down RetroBug's UI and other
guests) and may be used for that. It may **not** be described as isolation, and it does not substitute for T1.
Amend doc 02's wording — as written it is exactly the kind of claim brief (h) exists to catch.

**C14 — "zero configuration" is satisfied only if per-app data is single-sourced and probe-defaulted
(brief (f)+(g)).** Four docs each invent a per-app data surface: doc 08's per-game TOML patch system, doc 03's
per-app GL backend choice, doc 05's per-instance config row (`ProfileStore(instanceId)`), doc 09's per-device
tier selection. **DECISION:** one store, one schema (`App Revival profile`), keyed by `(guestAppId, instanceId)`
with *no* per-package entry required to launch: the default must be produced by the capability probes at first
launch (GPU probe per doc 08, `execmem` probe per doc 09's ladder, reservation probe per C6.6, GLES1 artefact
probe per C4), and a profile may only *override* a probe result. A missing profile must never mean "fail" and
must never mean "use another app's settings" (doc 05 line 561's "no API accepting an un-namespaced key" is the
right rule; extend it: no API returning a default that was sourced from a *different* app's row).

**C15 — terminology drift that would corrupt the architecture doc (brief (vi)).** Doc 04 line 448 recommends
framing the product as "a *player/**compatibility layer***"; the brief forbids RetroBug being "a thin
compatibility shim" and requires it to be CPU+runtime emulation. Doc 02 line 321 quotes a competitor's "
interpreter and translation, **not emulation**" framing approvingly. Meanwhile doc 04 line 720 notes App Store
category 4.7 speaks of "emulator apps", **not** "Android app compatibility layer". **DECISION:** Phase 1 §1
defines the vocabulary once — *CPU emulation* (executing guest machine code, translated), *runtime
reproduction* (the Android surface RetroBug supplies itself), *API forwarding* (host-provided GL/audio where
the host already has it, which is not the same thing as API-call translation of guest code) — and the same
three terms are used in the README, the Play listing and the App Store metadata. Also fix the phrasing at
doc 02 line 1230, which lists "**`execmem`** changes" under "requires system integration" while doc 02 line
1195-1196 and doc 09 §1.1 correctly say `execmem` is already granted (`private/app.te:189`, verified). The
intent is "changing sepolicy is privileged", but as written an architect reading only the bullet list would
conclude RetroBug cannot JIT — and drop the project.

**C16 — doc 07's identity-mapped arena collides with doc 08's *measured* iOS experience (and doc 07 did not
know about doc 08).** Doc 07 §4.3.1 recommends reserving `[0, 4 GiB)` at address 0 as the primary layout, with
biased mode as "fallback (3)", and calls iOS "unproven". Doc 08 "iOS/JIT lessons" item 7 reports first-party
experience from XenDroid/xemu: **"use OS-chosen base (`MapViewsMac`-style) rather than a fixed window when
extended VA addressing may be absent; the reservation 'fails intermittently, which presents as a startup crash
on some devices and not others'"** (`xenra.entitlements:52-64`), and item 6 adds that
`com.apple.developer.kernel.extended-virtual-addressing` / `increased-memory-limit` are **restricted**
entitlements that are "silently inert unless the App ID carries the capability", so "a successful install is not
evidence that either is active — measure at runtime". **DECISION:** this is not "primary + fallback", it is
**two build-time address models with a probe**: Android defaults to identity/flat fastmem with the reservation
verified at startup (report it, never crash on it); **iOS defaults to biased (`GUEST_BASE`, OS-chosen base)**
and never assumes `extended-virtual-addressing`. Consequences that must be designed in, not discovered:
`jobject`/`jmethodID`/host→guest pointer aliasing (doc 07 §4.3.4's handle table, doc 02 §2.3) become
**mandatory on iOS and only an optimisation on Android**, and any code that assumes "32-bit guest pointer is a
valid host pointer" must be confined to one module. This is the cross-doc pairing that most changes Phase 1's
memory-model section, and neither doc could have found it alone.

**Checked and *not* in conflict (so that Phase 1 does not re-litigate them).** Category (iv) of the brief —
a hidden assumption of a guest OS image or a VM: I grepped every doc for `guest OS|rootfs|VM|system image|
full-system` and found **no doc proposing either on the normal path**; every hit is an explicit rejection
(doc 01 §1.4/§2 row C, doc 02 §6's title, doc 05's title, doc 07 §5.1's VMOS dismissal) or a *precedent cited for
interpreter performance* (doc 04 §3.1–3.2's UTM SE/iSH, doc 09 §3.3's Termux-QEMU). **But** note the caveat
recorded in C3/C8: iSH and UTM SE are full-system/rootfs workloads, so their no-JIT speed ratios are the
*optimistic* end of what RetroBug will see on iOS — cite them with that caveat, not as a forecast. Category
(ii) — a doc assuming a syscall/kernel feature another doc proved unavailable: beyond the `userfaultfd` case
(C10), `openat2`/`landlock`/`io_uring`/user-namespaces are treated consistently (doc 05 §1.4/§2.4 denies them to
apps; doc 02 §5.7 independently reaches the same conclusion from `seccomp_policy.cpp`; doc 07 §4.2's QEMU
`MAP_FIXED_NOREPLACE` caveat is about kernels, not app policy). Category (vi) — terminology drift: only doc 04
(C15).

## Claims that are load-bearing but still unverified

| # | Claim | Which doc relies on it | How to test it cheaply | Fallback if it fails |
|---|---|---|---|---|
| U-1 | `dlopen()` of a runtime-generated host `.so` from app-writable storage is impossible | doc 02 TL;DR risk #1, OQ-3; C1 (I derived the rejection from `native_loader_namespace.cpp:168-171` + `linker.cpp:1321-1340`, not from a device) | 20-line test APK: write an arm64 `.so` to `getFilesDir()`, `System.load()` it; then try `findLibrary`→`/proc/self/fd/N` on a memfd. Emulator, 1 hour | none needed — C1's decision already avoids it. The test only *confirms* we are not leaving a better option on the table |
| U-2 | A contiguous 4 GiB low reservation succeeds inside a zygote-forked Android app process | doc 07 §4.3.1; C6.6; implicitly all of fastmem's perf | `mmap(0,4GiB,PROT_NONE,MAP_NORESERVE\|MAP_FIXED_NOREPLACE)` from `JNI_OnLoad` on the local emulator, then 2–3 real devices; log result in the device report | biased `GUEST_BASE` mode (doc 07 §4.3.3) — dynarmic supports a nonzero `fastmem_pointer`, so JIT perf survives; pointer aliasing does not (see C16) |
| U-3 | The same on iOS | doc 07 §4.3.2 calls it unproven; **doc 08 item 7 is first-party evidence it fails intermittently** | one `mmap` probe on a real iPhone + a `task_info` jetsam/VA report on the diagnostics screen (doc 08 item 6's own suggestion) | iOS ships biased mode only (C16 decision) |
| U-4 | "Android 16 CDD still mandates GLES 1.1 `[C-1-1]`" | doc 03 TL;DR line 27 — the premise of "forward GLES1 on Android" | grep `android-cdd.pdf` for "OpenGL ES"; and functionally: `dlopen("libGLESv1_CM.so")` + `glGetString(GL_VERSION)` on 5 devices | GLES 1.x defaults to the vendored SwANGLE path everywhere (C4), losing some fps on old fixed-function apps |
| U-5 | RetroBug can select ANGLE per app on Android the way the platform does | doc 03 §"Backend recommendation" (per-app drop-in host backend) | `dlopen("libGLESv2_angle.so")` from an app; inspect `/data/misc/angle` write access | vendor ANGLE/SwANGLE inside RetroBug (needed for C5's sandbox tier anyway) |
| U-6 | What `oaknut::CodeBlock` actually calls to get executable memory (submodule, not fetched) | ~~C6.7~~ | **RESOLVED 2026-09-19 — fetched and read during vendoring.** `externals/oaknut/include/oaknut/code_block.hpp`: `mmap(PROT_READ\|PROT_EXEC, MAP_ANON\|MAP_PRIVATE)`, an RWX anon form, a Darwin `MAP_JIT` + `pthread_jit_write_protect_np()` form, and a Linux `PROT_MPROTECT(...)` form. `dual_code_block.hpp`: `memfd_create` + two `MAP_SHARED` mappings, one RW one RX — i.e. the `MemfdDualAlias` backend already exists, MIT-licensed. See `external/PROVENANCE.md` §"oaknut resolves `RbCodeCache`" | no longer needed; `Interpreted` (iOS / OEM-denied fallback) remains ours to write |
| U-7 | arm32 variants of `native_bridge_support/android_api` (`libnative_bridge_guest_*`, `TARGET_ARCH=arm`) still build on `main` | doc 02 §7.3 (lines 1120–1124 flags it), doc 03 §1.6, doc 06 §2 — all three recommend reusing this generator | AOSP/GreenAndroid build of one module (`android_api/libc`, `TARGET_ARCH=arm`) in CI; or a standalone reimplementation of `gen_proxy_libraries.py` against the NDK's 32-bit stub libs (doc 10 §4.3 — already on disk) | build the arm32 guest libs from a stable AOSP tag (`android15-release`), or generate the ~200 needed stubs ourselves from the NDK's `linker stubs` + `.map` files (Apache-2.0) |
| U-8 | dynarmic's A32 front end covers the ARMv7-A/VFPv3-D32/NEON subset *bionic and real apps* use, incl. misaligned-access trapping and FPSR accuracy | C2's whole verdict; doc 01 §9.3 lists it as unconfirmed | (a) run dynarmic's own `dynarmic_test_generator` differential lanes locally; (b) **disassemble every `armeabi-v7a` `.so` in the doc 10 corpus with `llvm-objdump` and diff the mnemonic set against dynarmic's decoder coverage** — pure host-side, no device, ~1 day | patch the missing ops in the vendored tree (0BSD allows it), and/or route unimplemented encodings to `ExceptionRaised` → a *per-instruction* host helper. Do **not** route them to Capstone in the shipped binary (C12) |
| U-9 | Tier I (IR interpreter) costs ~3× Tier J, not 10–20× | doc 04 §3.4 (iSH/libriscv numbers, secondary sources), C3 | instrument `TranslateBlock` over 5 functions from a real guest `.so`, print IR-ops-per-guest-instruction; then CoreMark-ARMv7 under both tiers on the emulator | add an IR→IR fusion/superinstruction pass ahead of both tiers; lower iOS marketing claims (and say so in the listing) |
| U-10 | `InMemoryDexClassLoader` over a host-brokered memfd works in an `isolated_app` process, and RetroBug can then `RegisterNatives` into it | C9's decision; doc 05's whole T1 topology | test APK with an `isolatedProcess` service: host memfd-writes a tiny dex, passes the fd over Binder, sandbox mmaps + loads + calls a registered native | **T1 covers native code only, and RetroBug must say so** — DEX stays in T0. That weakens (c)'s per-app FS isolation story for managed code, so it would need explicit disclosure and would make T1 much less attractive: this is the second-most-expensive unknown after U-2 |
| U-11 | hidden-API `Activity.attach` / `HiddenApiBypass` still works at `targetSdk 36` / Android 16 | doc 02 §6.3 item 3 (OQ-5), and therefore v1b UI hosting | one build on the emulator + one OEM device; it either works today or it doesn't | doc 02 §6.4's degraded modes: v1a "guest as library" (no UI) or a `SurfaceView`-rendered guest window; do not promise "any APK" on Android 16 in the listing until this is green |
| U-12 | Google Play's dynamic-code clauses do not bar executing *user-supplied* ARMv7 code that RetroBug translates | doc 06 §7c, doc 09 §9 (both flag as open) | read the current "Device and Network Abuse" text + a compliance question to Play; RetroBug ships **zero** downloaded code, so the analogy is DexClassLoader-from-external-storage rules | sideloading/own-site distribution for the Android build, or a ROM-integrated tier (doc 09 §7) as the fallback channel |
| U-13 | `execmem` + memfd-exec survive on Samsung/Xiaomi/OPPO/vivo/Huawei builds (policy overwrites are possible) | doc 09 OQ-6, doc 05 §2.2 ("make-or-break") | 6-device probe matrix: `mmap(RWX)`, `memfd_create`+`mmap(RX)`, `mprotect` RW→RX; capture `errno` (apps cannot read `dmesg`, doc 09 line 202) | Tier I in that process (C3) — no execmem needed. This is the strongest product argument for building Tier I *early* rather than "for iOS later" |
| U-14 | free `ubuntu-24.04-arm` CI runners expose AArch32 EL0 | doc 10 §6.4.6 | one CI job running a static ARM32 hello-world | use `qemu-arm` as the oracle only; RetroBug's own arm64 tests still run (they don't need AArch32) |

## Corrections to make in the research docs

Amend these before Phase 1 cites them. (I did not modify the docs.)

**Doc 01**
1. §8 tier 1 ("fork/extend Dynarmic `arm_32`") contradicts the TL;DR/§8 posture ("original interpreter +
   selective DBT"). Pick one; C2 chose **build on dynarmic**, so the TL;DR row F must be re-labelled.
2. §6/§8: **`MemoryProcessor` no longer exists** — replace with `UserCallbacks::MemoryRead*/Write*` +
   `UserConfig::page_table`/`fastmem_pointer`/`absolute_offset_page_table`/`detect_misaligned_access_via_page_table`
   (`interface/A32/config.h:80-199`). Every dynarmic API claim in this doc was made against a ~2021 tree.
3. §5.1/§6/§9.3: the "no `svc`→Linux-syscall table" framing must be balanced by `UserCallbacks::CallSVC` and
   `Coprocessor::CompileGetOneWord`'s word-address mode; the real gaps are the *Android* layer, not hooks.
4. §7.6: the `0xffff0000` "32-on-64 trap" is dissolved by C6.4 (flat guest window) — the page is a *product*
   requirement (synthesize the helpers), not an address-space problem.
5. §6 and §8 tier 3: **remove Arm MRA/ASL-generated decoder tables and Capstone-as-shipped-fallback** (C12).
6. §1.2: the AArch32 probe cannot work from `assets/` (C11). If the probe stays at all, relocate it and
   re-label §1.4's "opportunistic accelerator" as out-of-scope (C6.5).
7. §9.3 open question is now answerable and should be deleted in favour of the U-8 experiment.

**Doc 02**
8. **Whole-document:** delete the runtime-generated shadow `.so` mechanism (TL;DR 18–25, the ASCII diagram at
   48–49, §1.5 "Chosen", §6.4 v1, boundary-table row 1168, and "no privilege" list items 2 and 11). Replace with
   C1's decision (single pre-shipped agent `.so` + host-side `RegisterNatives` + import-time DEX native index +
   guest `JNIEnv` proxy). Also add `System.load(absolutePath)` handling, which the doc does not list as a gap.
9. Line 54 + 67–73: "engine = fork of Berberis + new ARM32 frontend" → **wrong**; see C2 item 4 / doc 06 §2.
   Correct to "engine = dynarmic (0BSD); reuse Berberis/`native_bridge_support` **ABI+shim** only".
10. Line 78 (risk 1) is closed; risk 3 (16 KB) needs the C6 answer.
11. Line 1230: re-word "…or `app_data_file:file execute`/`execmem` changes" so it cannot be read as "we cannot
    JIT" (C15).
12. Line 935 (`:guestN` = "process isolation") → re-label as crash/lifecycle containment (C13).
13. §6.2's `PathClassLoader` preference argument is now moot in T1 and partially moot in T0 (C9): add the
    `InMemoryDexClassLoader` + brokered-memfd variant as the conformance target.
14. §1.4's "no `dlopen_passthrough`/`native_bridge_code_cache_callbacks` on main" and the "`nativebd` is dead"
    findings were checked against `native_bridge.h` (27 KB, fetched) — keep; they're load-bearing and correct.

**Doc 03**
15. Downgrade "forward GLES 1.x to the host driver by default" from decision to **probe-conditional** (U-4):
    AOSP itself ships `libGLESv1_CM_angle` for arm and arm64 (`packages/modules/ANGLE/Android.bp:67-90`).
16. Fix "ANGLE as a per-app drop-in host backend" — RetroBug probably cannot select it (U-5); state the
    vendored-SwANGLE plan instead, and note it is *also* the sandbox tier's renderer (C5) so the two needs are
    served by one component.
17. Add a row for the isolated tier: guest GL in T1 has **no driver access at all** (`isolated_app_all.te:73`),
    which is a stronger constraint than "graphics stays host-side".

**Doc 04**
18. §"Core design requirements" line 616–619: rename `CodeCache` backends — `IrInterpreter` is an executor, not
    a code cache (C3 decision 5). Also note the engine already *is* IR-first (C3), so the requirement is
    satisfied by design rather than by new invention; and **the iOS JNI seam no longer needs a special case**
    once C1's agent-table design is adopted (line 215–217).
19. §3.4: the iSH/libriscv/"1/3 of a JIT" figures are for single-threaded, no-GPU, no-FFI-churn Linux userlands;
    RetroBug's guest is threaded with JNI round-trips per call. Keep the numbers but add the caveat the doc
    already makes once at line 326–327 **into the table itself**, so nobody quotes "1/3" as a RetroBug forecast.
20. Line 220–223 (iOS needs its own managed-code tier) is the **single largest unpriced item in the phase** and
    is not mentioned by doc 02 at all. It must become an explicit scope decision in Phase 1, not an open
    question — see sequencing S-6.

**Doc 05**
21. Add the two verified rules it cites by paraphrase, with lines: `isolated_app_all.te:73` (gpu neverallow),
    `:65-70` (≤3 services), `:42-43` (no `open` on app data), `private/app.te:731-746` (no `execute` on
    data_file_type for `isolated_app_all` **and** `sdk_sandbox_all`). The `sdk_sandbox_all` half is new
    information: Android's own SDK sandbox is in the same boat, which is a useful design precedent.
22. §Recommended/graphics: replace the single "command stream into the ring" plan with C5's (A)/(B)/(C) options
    and the `RbGfxChannel` seam requirement; and drop `userfaultfd`-dependent or `mmap(PROT_EXEC, fd)`-dependent
    assumptions in favour of copy-into-anon (C9).
23. §2.2's "make-or-break question" is answered and can be de-riskified to a citation of V-1/V-4.

**Doc 06**
24. §1 note: `lioncash/dynarmic` is not merely a fork — **it is archived (2024-03-05)**; the maintained tree is
    `azahar-emu/dynarmic` (v6.7.0, 2026-06-24). Strengthen the "pin a commit, vendor a copy" recommendation and
    add "expect breaking API changes" (v6.7.0 changed `fastmem_pointer`'s null semantics).
25. §1: rename `arm_32` → **`A32`** throughout (upstream directory rename); keep the capability conclusions
    (they hold), and record that a **riscv64** back end also exists (three hosts, not two).
26. §5/§dependency table: gl4es's role description is wrong (C12) — delete it unless it is actually linked.
27. Add: `Capstone` **test-only, never linked** (it currently reads as reusable), and `llvm-mc`/`llvm-objdump`
    (Apache-2.0+LLVM-exception) as the sanctioned decode oracle replacing both Capstone and MRA.

**Doc 07**
28. §3.1(1): the `LDR/MSR` is per **conditional branch emission**, not per block (`emit_arm64_a32.cpp:24-31`,
    `EmitA32Cond`) — re-size the opportunity accordingly.
29. §4.2/§4.3: "`MemoryProcessor`" and the "nobody has built this exact shape" line both need updating from
    V-7/C16 (dynarmic explicitly supports guest-host same-address-space; fastmem with fault fallback is
    already the mechanism for §4.1's problem). §4.3.1's identity-first recommendation must be re-scoped to
    Android after C16.
30. §4.3.8 / doc 09 §5(2): the shadow-PTE subsystem can be replaced by C6.2's "leave fastmem" policy.

**Doc 08**
31. No errors found; it is the only doc whose evidence is first-party-and-local. Two of its items
    (**P2 "never edit the vendored core"**, and item 7's fixed-vs-OS-chosen base) are *load-bearing
    contradictions* with the C2/C16 decisions and must be explicitly adjudicated in Phase 1 rather than
    inherited. Also promote item 4 ("capability probes must be refreshable; point-of-use `QueryJitStatusRefresh`")
    into the shared diagnostics contract — it is the general form of doc 05's "verify, don't assume".

**Doc 09**
32. §5(2): remove `userfaultfd` (C10). §Fallback ladder #1: memfd-dual-alias is **not** what the chosen engine
    does (C6.7) — re-rank #2 (anon RWX, matches `oaknut::CodeBlock` + `app.te:189`) as v1 and keep memfd as the
    hardened variant. §3.5 "Dynarmic/Citra/yuzu NOT VERIFIED" and OQ-2 are now resolved by V-7.
33. OQ-1/OQ-3/OQ-8 (which SELinux perm is consulted for anon `PROT_EXEC`, `mprotect`→exec, `file:execmod`,
    artd's JIT pool) remain open but are **no longer load-bearing**: V-1/V-4 already establish the permission
    exists; downgrade them to hardening TODOs.

**Doc 10**
34. §0 item 2 must stop pointing at `getTrampoline(handle,name,shorty,len)` as the seam to reproduce (V-5: that
    path is bridge-only) — replace with "host-side `RegisterNatives` + guest `JNIEnv` proxy".
35. §5's "milestone 1 must be validated on a physical device" is **too strong**: with an x86_64 engine build
    (C7) the runtime-integration layer is emulator-verifiable today. Keep the device for Tier-J perf and OEM
    variance. Also add: `qemu-arm` needs Linux, and this machine has no WSL/Docker (C7 consequence 2).
36. §6.3/§6.4: drop MRA and gcc-`gas` as vector sources (C12); relabel §6.4.1's "(a) the interpreter (reference
    semantics, always correct)" — after C2/C3 it is *not* an independent reference (C8).
37. §4.3's NDK 32-bit linker **stub libraries** finding should be promoted from "ABI surface reference" to
    "fallback input for the proxy-lib generator if U-7 fails" — it is the only on-disk, legally clean, per-API
    32-bit symbol list RetroBug already has.

## Sequencing constraints implied by the above

**S-1 — Nothing else can start until the vendored dynarmic builds and executes an ARM32 block on the host.**
C2 makes the engine a *dependency*, so "vendor + pin + build for `x86_64-linux`/`windows` + `arm64-android` +
`x86_64-android`" is the first work item; C7 makes the **x64 build a product requirement**, not a convenience.

**S-2 — U-8 (decoder coverage of real app binaries) gates the roadmap's confidence, and needs no device.**
Disassemble the doc 10 corpus and diff against dynarmic's coverage *before* Phase 2 commits to milestones that
assume "the hard part is the Android layer". If coverage is <99% of observed mnemonics, the patch series
becomes the critical path, not a side quest.

**S-3 — The external-oracle harness (qemu-arm subprocess) must exist before either tier is optimised** (C8).
Ordering: WSL2/Linux CI lane → `qemu-arm` differential runner → corpus integration → then tiers. Building Tier I
before the oracle exists means building it untestable.

**S-4 — U-2/U-3 (address model) must be settled before any pointer-marshalling code is written.** C16 shows
identity vs biased mode flips which pointers need handle tables and bounce buffers. A week of probe work
prevents an audit of doc 02 §2/§3. Do it in the same sprint as S-1 (same build).

**S-5 — C1's mechanism is the cheapest device-free de-risking available and must be milestone 0, not part of
milestone 1**: an emulator-only spike (pre-shipped agent `.so` in `apk_data_file` + `findLibrary` override +
`RegisterNatives` + a hand-written trampoline that calls back into Java) proves the whole ART/sepolicy
interaction with **zero emulator or CPU code**. It also settles U-1 and (on the emulator) most of U-13's AOSP
half.

**S-6 — Scope decision required from the product owner before Phase 1 closes (the biggest single ambiguity in
the phase):** constraint (d) says the *CPU core* must be shared with iOS — doc 02's architecture shares much
more than the core (host ART runs the guest's DEX), and **on iOS there is no ART**, so an iOS RetroBug needs its
own managed-code tier (doc 04 line 220–223), which is plausibly larger than every other component combined
(doc 04 line 326–327: "orders of magnitude more work" than a GBA core). Phase 1 must state which of
(i) "iOS = CPU core + native-code-only guest running, no Android framework" or (ii) "iOS = full Android runtime
port, own DEX interpreter" it is committing to; the two imply different module boundaries, and (ii) changes the
project's cost by a factor. **I cannot resolve this from the evidence; it is a product decision, and the
architecture doc must not be written without it.**

**S-7 — The `RbGfxChannel` and `RbCodeCache` seams must be drawn in Phase 1 even though only one
implementation of each is written in Phase 2** (C5, C6.7). Retro-fitting a transport under a working in-process
renderer, or an allocator under a working JIT, is where projects die.

**S-8 — Tier I must ship on Android before it is "the iOS tier"** (C3 decision 1, U-13). It is the fallback for
OEM policy variance and the only way to test iOS-shaped behaviour on Android hardware; if it is deferred as
"iOS work", U-13 stops being a fallback and becomes a product cliff.

**S-9 — Isolation claims become a release gate, not a test suite** (C5, brief (h)). No UI string containing
"isolated" may ship before the leak-probe matrix passes on: emulator (AOSP policy) + one `isolated_app`-capable
device + the tier-name assertion in T-37 (no in-process guest loader instantiation). U-10 gates whether T1 can
be described as running "the app" at all, or "its native code".

**S-10 — U-11 (hidden-API Activity hosting) decides when RetroBug may publicly claim "import an APK, select
it, launch it"** (brief (f)). Until it is green on a shipping device, the honest milestone-1 definition is
doc 02 §6.4's **v1a** ("guest as library", no window) — which is also the only definition compatible with
"zero configuration" *and* "no hidden-API roulette" *and* the App Store/Play policy reads in doc 04 §4 /
doc 06 §7c. Do not sequence the UI-hosting spike after the translator; run it in parallel from week 1, because
it is the one item that can invalidate the product rather than the design.
