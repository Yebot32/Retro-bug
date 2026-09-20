# RetroBug — AOSP and NDK source assets: what can be built, linked, or only read

Companion to [`33-guest-library-inventory.md`](33-guest-library-inventory.md), which settled *what the NDK's 32-bit
stub `.so` files can and cannot generate* (stub bodies are 2-byte `bx lr`; `libc++.so` is a 19-byte
`INPUT(-lc++_shared)` linker script). This document answers the next question — **where does the real 32-bit code
come from** — for the milestone-M1 library list, and assigns each candidate exactly one of the four verdicts.

Evidence tags are `docs/10`'s legend ([`10-architecture.md:9-11`](10-architecture.md)): **[1L]** read or ran it here,
**[1A]** read a primary source (here: fetched and read the upstream blob directly), **[INFER]** reasoned from
documented behaviour, **[UNVERIFIED]** load-bearing and unchecked.

Two vocabulary reminders that constrain every verdict below:

* **AD-10** ([`10-architecture.md:50`](10-architecture.md), and §13.3 at `:695-709`): guest system libraries are
  *built from AOSP/NDK source*, never extracted from a device. Apache-2.0 / BSD / MIT may be linked **or derived**;
  GPL is excluded from linked code.
* **The CPU-emulation rule** ([`10-architecture.md:22-25`](10-architecture.md) §1): every byte of guest machine code
  is executed by Dynarmic. §1's third term, *API forwarding* (`:25`), applies to a 32-bit **guest shim** that then
  calls a host implementation — so a "forwarder" is only ever half of a library, and the other half is still 32-bit
  code the emulated CPU runs. [`core/abi/jni_env.cpp:7-8`](../core/abi/jni_env.cpp) states the operational form of
  this: "**nothing here may hand a host pointer to the guest, and nothing here may trust a guest pointer.**"

---

## 0. Verdicts

| Candidate | Verdict | One-line basis | Tag |
|---|---|---|---|
| NDK `libm.a` → `libm.so` | **BUILD-AS-GUEST** | 548,592 B ELF32-ARM `.so`, no TEXTREL, 284/284 stub symbols, genuine A32 code bodies | [1L] |
| NDK `libz.a` → `libz.so` | **BUILD-AS-GUEST** | 227,544 B ELF32-ARM `.so`, 93/93 stub symbols, zlib licence; AOSP itself does not proxy `libz` | [1L] [1A] |
| NDK `libc.a` + `libclang_rt.builtins-arm-android.a` → `libc.so` | **BUILD-AS-GUEST** (needs AOSP `libc.map.txt`) | 549 members, 3.28 MB `.so` linked; 20 stub symbols outside those two archives; version script mandatory | [1L] |
| NDK `libc++_static.a` + `libc++abi.a` (+ `libunwind.a`) → `libc++_shared.so` | **BUILD-AS-GUEST** | 7,109,868 B, FUNC 1585/OBJECT 755 vs the NDK's own prebuilt 7,126,916 B FUNC 1586/OBJECT 755 | [1L] |
| NDK `libstdc++.a` → `libstdc++.so` | **BUILD-AS-GUEST** | 3 members, 16,648 B, ELF32 ARM; 12 stub FUNC — trivially the whole library | [1L] |
| NDK `libdl.a` → `libdl.so` | **UNUSABLE** | Reason: one 4 908 B member (`libdl_static.o`) that bounces into *host* linker internals. `dlopen`/`dlsym` are RetroBug's own C06 loader surface, not a bionic forwarder; the stub's symbol list stays the authority | [1L] [INFER] |
| NDK `libcompiler_rt-extras.a` | **BUILD-AS-GUEST** (one object: `mulodi4.o`) | 3,634 B; pairs with `libclang_rt.builtins-arm-android.a` (206 ELF32 ARM objects) | [1L] |
| NDK 32-bit stub `.so` files (`…/arm-linux-androideabi/<api>/*.so`) | **READ-FOR-SEMANTICS** (symbol authority only) | Already settled in `docs/33`; unchanged here | [1L] |
| AOSP `bionic/libc`, `bionic/libm`, `bionic/libdl`, `bionic/linker` | **BUILD-AS-GUEST** | `libc/MODULE_LICENSE_BSD`, `libm/MODULE_LICENSE_BSD_LIKE`, `linker/MODULE_LICENSE_BSD`; `arch-arm/` and `fenv-arm.c` live on `main` | [1A] |
| AOSP `bionic/libc/libc.map.txt`, `libm/libm.map.txt`, `libc/version_script.txt` | **LINK-AGAINST** (as build inputs — a version script is data, not code) | Without one, `ld.lld` refuses `libc.a` outright (error text quoted in §1.3) | [1L] [1A] |
| `frameworks/libs/native_bridge_support/android_api/*` generated proxies | **READ-FOR-SEMANTICS** | Emits host-x86_64 trampolines + guest stubs for a *binary translator*; wrong shape for a CPU emulator (§3.4). Re-verdict to LINK-AGAINST only if RetroBug ever generates its own stubs from them | [1A] [INFER] |
| `android_api/gen_proxy_libraries.py` + `gen_known_trampolines.py` | **READ-FOR-SEMANTICS** | The arg-kind taxonomy and the `custom_trampolines_*` exception lists are the valuable output; running it needs a full AOSP build host | [1A] [INFER] |
| `android_api/libEGL/stubs_arm.cc` + `proxy/api_arm.json` | **READ-FOR-SEMANTICS** (per-function arg kinds, `custom_trampolines_arm_to_x86.json` = the known-hard list) | A machine-readable list of exactly which EGL/GLES entry points need hand-modelling | [1A] |
| `android_api/linker/init_static_tls_arm.S`, `linker_tls.cpp`, `linker_translate_path.cpp` | **READ-FOR-SEMANTICS** | Guest TLS setup is RetroBug's C06/C07 job, done in host C++ not in guest asm | [1A] [INFER] |
| `native_bridge_support/guest_state/include/arm/` | **READ-FOR-SEMANTICS** | The guest register-window ABI AOSP assumes for an arm32 guest, still on `main`; RetroBug's equivalent is `rb::abi::CallGuest`'s `AbiContext` | [1A] |
| `native_bridge_support/android_api/libc/` (`exit.c`, `malloc_init.cpp`, `__libc_init_scudo.cpp`, `__cxa_thread_atexit_impl.cpp`, `__libc_add_main_thread.cpp`, `__libc_set_target_sdk_version.cpp`) | **READ-FOR-SEMANTICS** | The exact 6 libc functions AOSP had to override for a foreign-arch guest — a checklist, not a libc | [1A] |
| `art/libnativebridge/` (`native_bridge.cc`, `include/nativebridge/native_bridge.h`) | **READ-FOR-SEMANTICS** | `NativeBridgeCallbacks` v6 is the platform's documented foreign-arch-JNI contract (§5); RetroBug's engine replaces its role, but the shape of the seam is the prior art | [1A] |
| `art/libnativeloader/native_loader.cpp` | **READ-FOR-SEMANTICS** | Shows the `NativeBridgeIsPathSupported` → `NativeBridgeLoadLibrary` fork around `android_dlopen_ext` | [1A] |
| `art/runtime/jni/java_vm_ext.cc`, `runtime/native_bridge_art_interface.cc` | **READ-FOR-SEMANTICS** | `JNI_OnLoad` resolved via `NativeBridgeGetTrampoline2(…, shorty, len, jni_call_type)` | [1A] |
| `external/zlib` / upstream zlib | **BUILD-AS-GUEST** | §4 settles (a) over (b); zlib licence text is already in the local NDK `NOTICE:9627-9629`, and the locally built `.so` is measured in §1.2 | [1L] |
| Berberis (`frameworks/libs/binary_translation`) | **READ-FOR-SEMANTICS** only for guest libs | It is a *translator*; RetroBug's translator is Dynarmic (AD-1) | [INFER] |
| Any `.so` extracted from a device | **UNUSABLE** | AD-10; and `docs/10` §13.3: "No third-party `.so` is ever extracted from a device image" | [1L] (repo) |

**The headline result: RetroBug does not need an AOSP checkout to have real 32-bit guest `libm`, `libz`,
`libstdc++` and `libc++`.** Four of the M1 libraries are one `ld.lld -shared --whole-archive` away, built and measured
on this machine in under ten minutes (§1). What the NDK archives cannot give is `libc.so`/`liblog`/EGL/GLES — for
different reasons, spelled out per item.

---

## 1. What is already on this machine

### 1.1 The archives are real compiled 32-bit ARM code

`$LOCALAPPDATA/Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/arm-linux-androideabi/`
(NDK **r28c**, `source.properties:3,4` — [1L]). Every one of the requested names exists, and
`llvm-readelf -h <archive>` reports **`Class: ELF32`, `Type: REL (Relocatable file)`, `Machine: ARM`** for **every
member** — `libc.a` 549/549, `libm.a` 193/193, `libz.a` 19/19, `libc++_static.a` 54/54, `libc++abi.a` 18/18. [1L]
`llvm-readelf -A libm.a` reports `CPU_arch: ARM v7`, `CPU_arch_profile: Application`, `ARM_ISA_use: Permitted`,
`THUMB_ISA_use: Thumb-2`. [1L]

| Archive | Bytes | Members | What it is |
|---|---|---|---|
| `libc.a` | 4 707 106 | 549 | bionic static libc, incl. pthread and the syscall stubs |
| `libm.a` | 963 240 | 193 | bionic libm **plus a `builtins.o`** (`sqrt`, `sqrtf`) |
| `libz.a` | 262 036 | 19 | upstream zlib incl. `adler32_simd.o`, `crc32_simd.o`, `crc_folding.o`, `cpu_features.o` |
| `libc++_static.a` | 9 866 836 | 54 | LLVM libc++ |
| `libc++abi.a` | 1 921 754 | 18 | libc++abi (demangler, `__cxa_*`, vector CTORs) |
| `libstdc++.a` | 16 648 | 3 | legacy shim: `__cxa_guard.o`, `__cxa_pure_virtual.o`, `new.o` |
| `libdl.a` | 4 908 | 1 | `libdl_static.o` only |
| `libcompiler_rt-extras.a` | 3 634 | 1 | `mulodi4.o` only |
| `libclang_rt.builtins-arm-android.a` | — | 206 | **`lib/clang/19/lib/linux/`** — `__aeabi_*`, soft-fp, `__divmodsi5` |
| `libunwind.a` | 57 316 | 9 | **`lib/clang/19/lib/linux/arm/`** — 35 `_Unwind_*` symbols; C++ exceptions on arm32 |

A second local asset the inventory did not use: **`$NDK/meta/system_libs.json`** (615 B) is NDK-declared
`lib*.so → first-API-level` for *all* ABIs (`libEGL` 21, `libGLESv1_CM` 21, …, `libaaudio` 26, `libamidi` 29,
`libicu` 31) [1L] — an availability authority independent of the stub dirs; and `abis.json` records
`armeabi-v7a: { "deprecated": false, "min_os_version": 21 }` [1L].

**ISA mix, measured from symbol bit 0** (`llvm-nm --defined-only --format=posix`, odd address ⇒ Thumb):
`libm.a` 863 ARM / 13 Thumb, `libc.a` 5 402 / 302, `libz.a` 249 / 15. [1L — **but the method is wrong;
see the correction below**] So bionic's own code is **overwhelmingly
ARM-mode (A32)**, unlike NDK-compiled app code which defaults to Thumb-2. Corollary, and it is a load-bearing one
for U-8: *the A32 decoder and ARM↔Thumb interworking (`bx`, `blx`) are not optional for M1* — a guest's own
`libc.so` is mostly A32 even if the app's `.so` is all Thumb. This is independent corroboration of the 8 387
IT-block instructions [`docs/32-isa-coverage.md:26`](32-isa-coverage.md) measured from real bionic/libm/libc++/zlib
ARMv7 code.

> **Correction, 2026-09-20, from an independent review that re-ran it: the numbers above are not
> measurements of what they claim.** `llvm-nm` **masks bit 0 for functions**, so a symbol-bit-0 survey of
> its output can never count a Thumb function, and the rows it produces as "Thumb" are `.L.str` data
> labels rather than code. A `readelf` survey of the same archives gives **`libm.a` 304 ARM / 60 Thumb**
> and **`libz.a` 131 / 131** — the second of which flatly contradicts this section's "overwhelmingly ARM"
> for libz. It matters because the numbers propagated: they were repeated in RetroBug's own commit
> `ba40a52` before anyone re-derived them, which is the failure this project keeps writing rules about.
>
> **What survives is the conclusion the paragraph was used for**, now on a sounder basis: the functions
> M1's probe actually imports (`sin`, `cos`, `sqrt`) are ARM-state in the built `libm.so`, and
> `core/tests/guest_apk_test.cpp` prints that count from the loaded image rather than from a survey
> (28 of 315 exported functions are Thumb). ARM↔Thumb interworking is still not optional for M1 — it is
> now evidenced by direct calls that fail with an ISA mismatch rather than by a bit-0 census.


### 1.2 The experiment: a static link, then a shared guest library

All commands run on this machine 2026-09-20 in Git Bash against a `mktemp -d` scratch directory. [1L]

**(a) Static link.** A 10-line `main()` computing `sqrt(x) + sin(x) + fmod(x, 1.5)` over a non-constant `x` (the
first attempt with `x = 2.0` folded away and pulled in **zero** libm objects — worth recording, because it is the
mistake that makes a "we linked libm" claim hollow), built with

```
clang --target=armv7-none-linux-androideabi26 --sysroot=$TC/sysroot -static -Os -fno-builtin -o m2 m2.c -lm
```

→ **1 663 212 B ELF32 ARM `EXEC`** (`Class: ELF32`, `Machine: ARM`), `llvm-strip` → **261 580 B**.
`-Wl,-Map` shows the libm closure is **8 of 193 members**: `builtins.o e_fmod.o k_cos.o k_rem_pio2.o k_sin.o
s_floor.o s_scalbn.o s_sin.o` — i.e. `sin` drags in `__kernel_sin` + `__kernel_rem_pio2` + `floor` + `scalbn`,
`fmod` is self-contained, and **`sqrt` is not a library call at all**: it is `builtins.o`, four instructions —

```
1c39c: ec410b30   vmov   d16, r0, r1
1c3a0: eef10be0   vsqrt.f64  d16, d16
1c3a4: ec510b30   vmov   r0, r1, d16
1c3a8: e12fff1e   bx lr
```

and `sin`'s prologue is `e92d4010 push {r4, lr}`, `e30221fb movw r2, #0x21fb`, `eebd0be0 vcvt.s32.f64 s0, d16`.
Genuine ARMv7-A + VFPv3-D32 encodings, not stubs. **Practical consequence: `sqrt`/`sqrtf` exercise zero library code
and 100 % the `vsqrt` path in the VFP unit**, so `sqrt`-heavy guest apps test the FPSCR/`vsqrt` implementation and
not `libm` — the two failure modes are distinguishable and M1's test should say which one it is aiming at.

**(b) Shared guest library — the result that matters.**

```
ld.lld -m armelf_linux_eabi --shared --soname=libm.so --whole-archive $BASE/libm.a -o libm_guest.so
```

→ **548 592 B, ELF32, `Type: DYN`, `Machine: ARM`, `SONAME libm.so`**, 315 defined `FUNC` in `.dynsym`, and
**`sin` present at `0x2cba0` as a real function** — objdump shows its A32 prologue (`e92d4010 push {r4, lr}` …),
where the NDK stub reports `Size 2` for the same symbol (its body is `bx lr`, per `docs/33` §2). Relocations:
`153 × R_ARM_JUMP_SLOT`
+ `2 × R_ARM_GLOB_DAT`, **no `.rel.text`, no `DT_TEXTREL`** — bionic's static archive is fully PIC, so it links into
a clean shared object with no text patching. `llvm-objdump -d` over the whole file: 24 415 decoded instructions,
5 230 of them VFP `.<f32|f64>` operations. [1L]

**ABI-surface parity: the built library covers the stub completely.** Every one of the **284** `FUNC` the API-34
`libm.so` stub exports is defined by `libm.a` — `comm -23` gives **0 missing**. Same test on `libz`: **93 of 93, 0
missing** (built `libz_guest.so` = 227 544 B, with `deflateInit_`, `inflateInit_`, `inflateInit2_`, `zlibVersion` all
present as real code). [1L] This is the finding that upgrades the stubs from "names only" to "a verifiable
requirement list against a real implementation."

> **Correction, 2026-09-20, re-measured by `tools/build-guest-libs.sh`: the `libm` half holds and the `libz`
> half does not.** `libm` is 284/284 with **0 missing** — reproduced independently **[1L]**, and `sin` is
> 1,204 bytes where the stub's is 2, `sqrt` 16 where the stub's is 2. `libz` is **85 of 93**: eight
> `FUNC` the stub exports are not importable from the built library — `_dist_code`, `_length_code`,
> `_tr_align`, `_tr_flush_bits`, `_tr_flush_block`, `_tr_init`, `_tr_stored_block`, `_tr_tally`.
>
> They are not *absent*, which is what makes the original comparison wrong rather than merely optimistic:
> all eight are in the image, as `LOCAL HIDDEN`. `llvm-readelf --dyn-syms` shows `.dynsym`, where a hidden
> symbol does not appear, so a name-set comparison silently treats "present and unreachable" as either
> present or absent depending on which table it happened to read. The distinction is load-bearing for
> C11 — a guest cannot import a hidden symbol — but it is also **inconsequential in practice**, because
> all eight are zlib's own internals (`trees.c`, the fixed Huffman tables) and nothing outside zlib
> declares them. The public surface the file names (`deflateInit_` 32 B, `inflateInit_` 10 B, `inflate`
> 6,196 B, `deflate` 2,584 B, `inflateEnd` 82 B) is present with real bodies **[1L]**, so the verdict
> `BUILD-AS-GUEST` is right and only the parity number was not.
>
> Two of the eight are a separate error in the same direction: `_dist_code` and `_length_code` are
> **`OBJECT`** data tables in the real build, which the names-only stub advertises as `FUNC`. So the
> stubs are not authoritative about symbol *kind* either, only about *name* — which is the correct
> reading of `docs/33` §2 and worth stating before anyone builds a generator that trusts the type.
>
> The script now encodes this as eight named entries in a `KNOWN_HIDDEN` allowlist rather than as a
> relaxed check, so a ninth unexplained gap fails the build, and it re-reports any allowlisted name that
> later becomes resolvable so the list cannot rot. Every other measurement in this section was
> reproduced and stands.


**(c) The two things that do *not* fall out of the NDK.**

1. **`libc.so` needs AOSP's version script.** `ld.lld --shared --whole-archive libc.a
   libclang_rt.builtins-arm-android.a` fails before producing anything:
   `error: … symbol __aeabi_memclr8@@LIBC_N has undefined version LIBC_N`, `… __aeabi_atexit@LIBC_PRIVATE …`. [1L]
   Supplying a hand-written map *does* link (3 282 460 B ELF32 ARM `DYN`), but my throwaway script's
   `LIBC_PRIVATE { local: *; }` collapsed the export table to 1 `FUNC` [1L]; the correct input is AOSP
   **`bionic/libc/libc.map.txt`** + **`libc/version_script.txt`** (both confirmed present on `main`) [1A]. I did not
   complete the corrected relink, so "**AOSP `libc.map.txt` yields a correct guest `libc.so`**" is
   **[UNVERIFIED]**, though the two errors above are exactly the kind a real map resolves.
   Coverage-wise `libc.a` + builtins reach **1 258 of the 1 278 `FUNC`** in the API-26 stub; the **20** gaps are
   13 × `__aeabi_*` (which *are* in `libclang_rt.builtins-arm-android.a`) and 7 genuinely dynamic-only/legacy
   entries: `bsd_signal`, `fdprintf`, `vfdprintf`, `free_malloc_leak_info`, `get_malloc_leak_info`, `pthread_atfork`,
   `putw`. [1L] Note also that `libm.a`'s residual undefined set (`ldexp`, `isnanf`) is *not* a hole: those two live
   in bionic's **`libc.so`**, confirmed against the stubs directly. [1L]
2. **`libdl.a` is not a guest `libdl.so`.** One 4 908-byte object whose job is to bounce into linker internals the
   host linker owns. RetroBug's `dlopen`/`dlsym` must resolve to **guest-executable** addresses — `docs/33` §3
   already classes `libdl` as "proc-address surface", and `docs/33` §4 item 2 lists `dlopen`/`dlsym`/`dlvsym` among
   the five pointer-returning entry points that no generator can forward. **Verdict UNUSABLE-as-guest-library**;
   the *symbol list* is still the authority. [1L] [INFER]

### 1.3 So is AOSP required at all?

For `libm`, `libz`, `libstdc++`, `libc++`/`libc++abi`/`libunwind` and the compiler-rt builtins: **no** — they are on
this machine and I linked four of them. For `libc.so` (version script + the 20 symbols + confidence that a
*static*-flavoured bionic behaves like the *dynamic* one a guest expects), for `liblog.so`, and for EGL/GLES:
**yes**, AOSP is the source — see §2–§4. One caveat that applies to every NDK-archive route and is stated once
here rather than per library: **`libc.a`/`libm.a` are bionic's *static* variants**, and a real device's
`/system/lib/libc.so` is the *dynamic* variant built from the same sources with a different set of internal
initialisation (`__libc_preinit`, `libc_init_dynamic`, scudo/`malloc_init`, TLS setup). Building the guest `libc.so`
from the archive is a shortcut, not an equivalence, and `native_bridge_support/android_api/libc/` exists precisely
because AOSP had to special-case six functions here (§0, that row). [INFER]

---

## 2. Bionic: licence, and does AOSP still build arm32 on `main`?

**Licence.** There is **no `LICENSE` file** anywhere in `bionic` on `main` — the root listing is
`apex benchmarks build cpu_target_features docs libc libdl libfdtrack libm libstdc++ linker tests tools` plus
`.clang-format`, `.gitignore`, `OWNERS`, `PREUPLOAD.cfg`, `README.md`, `TEST_MAPPING`, `CleanSpec.mk`, `CPPLINT.cfg`,
`android-changes-for-ndk-developers.md` — and no `LICENSE`, no `NOTICE`, no `REUSE.toml`; the licence is declared
**per-directory**:
`libc/MODULE_LICENSE_BSD` + `libc/NOTICE`, `libm/MODULE_LICENSE_BSD_LIKE` + `libm/NOTICE`,
`linker/MODULE_LICENSE_BSD` + `linker/NOTICE`. [1A]
So the precise statement is: **bionic is BSD-3-Clause-family (libc, linker) plus a "BSD-like" mixed upstream block
(libm)**, not "BSD-3" as a single label, and not Apache — and the reason `libm` gets `BSD_LIKE` rather than `BSD` is
visible in its own tree: `libm/upstream-freebsd/` and `libm/upstream-netbsd/` carry third-party code whose notices
differ. Two further file-level confirmations worth having: `libm/` has per-arch `fenv-arm.c`/`fenv-arm64.c`, and
`libc/` carries `SYSCALLS.TXT` (the generated syscall-stub source) and `arch-arm/`. [1A] `docs/10:288`'s "(BSD-3)"
and AD-10's "AOSP bionic is BSD-3" are
directionally right and AD-10-clean under the project's own rule (BSD is linkable *and* derivable); the refinement
is §6 conflict 4.
**Practical licence consequence:** `NOTICE.md`/`LICENSES/` (AD-16) must record *several* upstream notices for the
guest libs — AOSP BSD-3, FreeBSD/NetBSD for libm, zlib for libz (§4), LLVM Apache-2.0-with-LLVM-exception for
libc++ — not one bionic notice. All are permissive, so all satisfy AD-10. None is GPL. [1A] [INFER]

**Buildability of arm32 bionic on `main`: still live.** Evidence, in descending strength, all from
`android.googlesource.com/platform/bionic/+/refs/heads/main/`: [1A]

1. `libc/arch-arm/` exists (with 23 sibling dirs) and `libm/fenv-arm.c` exists. AOSP deletes arch directories when
   it stops building them (that is what happened to older ABIs), so their presence on `main` is the maintainers'
   statement of intent.
2. `libc/libc.map.txt`, `libc/version_script.txt` and `libm/libm.map.txt` are versioned **per ABI release**
   (`LIBC_N`, `LIBC_O` … `LIBC_Z`, `LIBC_PRIVATE`) and are the authoritative input for §1.2(c). [1A]
3. `frameworks/libs/native_bridge_support/android_api/gen_proxy_libraries.py` — an in-tree tool run by the
   maintainers — resolves guest arm32 API descriptions from
   `out/target/product/generic_arm64/symbols/system/lib/%s.so`, i.e. it **requires an `aosp_arm64` build to have
   produced a 32-bit `system/lib`**, and its `self.arches` map still lists `'arm'` beside `arm64`/`riscv64`/`x86`/
   `x86_64`. [1A] AOSP's own proxy generator would be broken if arm32 bionic stopped building.
4. `android_api/libEGL/proxy/` on `main` contains `api_arm.json` and
   `custom_trampolines_arm_to_x86.json` — checked-in generated output for the **arm32 guest**, not a stale
   leftover. [1A]

**On "which release last did", and on the NDK-r26+ premise.** I could not find an AOSP release at which arm32
bionic stopped building, because I could not find evidence that it ever did; the honest reading is that **the
question as posed conflates two different drops**. [1L] (local) + [1A] (upstream):

* What changed in the **NDK** is the **API floor**, not the ABI: `meta/platforms.json` on r28c is
  `{"min": 21, "max": 35}` and `abis.json` marks `armeabi-v7a` **`"deprecated": false"`** with
  `"min_os_version": 21`. [1L] (The same for `x86`. The last NDK to accept `android-19` was r25.) So the stub dirs
  starting at `21/` — the range `docs/33` §2 swept — is the r26 API-floor change, not an arm32 removal.
* What actually changed at the **platform** level is a *device* capability, not a build target: newer arm64 SoCs
  omit AArch32 EL0, so a *device* may have no `abilist32` — which is the reason RetroBug exists
  ([`10-architecture.md:44`](10-architecture.md) AD-4, and §17's "No armeabi-v7a emulator exists on this machine"
  row). [INFER]
* `docs/10` §17's U-7 fallback "build guest libs from an `android15-release` tag" is therefore **insurance against a
  risk that does not appear to exist**: arm32 is present on `main` today. [1A]

Verdict: **"build guest libc from AOSP source" is live, not historical.** [1A]

---

## 3. `native_bridge_support` — what it really is, and whether a forwarder is the right shape

### 3.1 Location and contents

Real path: **`platform/frameworks/libs/native_bridge_support`** (not `frameworks/native`, not `external/`). On
`refs/heads/main` (tree `8ec42c0`) it has exactly four directories: **`android_api/`, `guest_state/`,
`guest_state_accessor/`, `tools/`**, plus `Android.bp`, `native_bridge_support.mk`, `.clang-format`, `OWNERS`,
`PREUPLOAD.cfg`. [1A] It provides **both** halves of the question but **not** the bridge *implementation*: the
generated proxies live here (`android_api/`), while `libnativebridge` — the ART-side loader that owns
`NativeBridgeCallbacks` — lives in **`platform/art/libnativebridge/`** (`Android.bp`, `native_bridge.cc`,
`native_bridge_lazy.cc`, `libnativebridge.map.txt`, `README.md`, `include/`, `tests/`). [1A] The *linker* side is a
third place again: `bionic/linker/` contains **no** `native_bridge*` file; the linker-facing pieces are
`android_api/linker/` (`init_static_tls_arm.S`, `init_static_tls_arm64.S`, `init_static_tls_riscv64.cc`,
`linker_tls.cpp`, `linker_translate_path.cpp`, `linker.arm.map`, `include/`) [1A] — so "the native_bridge linker
hooks" are arch-specific *guest* support code, not modifications to `linker.cpp`.

`android_api/` holds 23 per-library directories plus four Python tools (`api_analysis.py`, `gen_proxy_libraries.py`,
`gen_known_trampolines.py`, `check_proxy_libraries.py`). The library list is
`app_process libaaudio libamidi libandroid libandroid_runtime libbinder_ndk libc libcamera2ndk libEGL libGLESv1_CM
libGLESv2 libGLESv3 libjnigraphics libmediandk libnativehelper libnativewindow libneuralnetworks libOpenMAXAL
libOpenSLES libvulkan libwebviewchromium_plat_support linker vdso`. [1A] Each per-library `Android.bp` declares one
`cc_library` named `libnative_bridge_guest_<lib>` with `stem:` set to the *real* soname, `overrides:` set to the
platform module, and `arch: { arm: { srcs: ["stubs_arm.cc"] }, arm64: {…}, riscv64: {…} }` — quoted for
`libGLESv1_CM` at [`docs/research/03-graphics-audio-input-emulation.md:217-227`](research/03-graphics-audio-input-emulation.md).
The detail that matters here is the last line of that block:
`shared_libs: ["libcutils", "liblog", "libnative_bridge_guest_libEGL"]` — **AOSP's "proxy" library is itself a guest
shared library with a guest dependency graph, and two of its three dependencies are libraries `docs/20` §C11 also
puts in the generated bucket.** [1A] [1L] (repo)

### 3.2 U-7, confirmed

**U-7's premise is true.** [`10-architecture.md:772`](10-architecture.md) asks whether "the AArch32 arm32 variants of
`native_bridge_support/android_api` still build on `main`". Evidence for the *source and generated output* still
being there and still being regenerated for arm32: `stubs_arm.cc`/`stubs_arm.cpp` in every per-library directory,
`api_arm.json` + `trampolines_arm_to_x86-inl.h` + `custom_trampolines_arm_to_x86.json` in `libEGL/proxy/`,
`init_static_tls_arm.S` in `android_api/linker/`, `guest_state/include/arm/`, and the generator's
`'arm' → generic_arm64/symbols/system/lib/%s.so` entry. [1A]

**What remains unverified, and it is the part that matters for reuse.** Whether `m aosp_arm64` still completes on
`main` in 2026 (I have no AOSP checkout and no build host here), i.e. *"still builds"* in the strict sense. The
above establishes *"still maintained and still expected to build"*, which is materially stronger than the previous
**[UNVERIFIED]** but is not the same claim. Downgrade U-7 rather than close it: **[1A] for presence, [UNVERIFIED]
for `m` completing**. [INFER] And note that the *decision-relevant* half of U-7 is now moot for the reasons in §3.4:
RetroBug should not consume these outputs even if they build.

### 3.3 Licence inside it, and what is inherited from elsewhere

`native_bridge_support.mk` is `Copyright 2020 … Licensed under the Apache License, Version 2.0`. [1A]
`android_api/gen_proxy_libraries.py` is `Copyright (C) 2024 … Apache-2.0`. [1A] `android_api/libc/Android.bp` carries
`default_applicable_licenses: ["Android-Apache-2.0"]` (cited as `:23-25` in
[`research/02:1099-1101`](research/02-android-runtime-abi-surface.md) **[1A]**, and consistent with what I read).
`art/libnativebridge/` and `native_loader.cpp` are Apache-2.0 (ART root: `MODULE_LICENSE_APACHE2` + `NOTICE`). [1A]
So **everything in this tree is Apache-2.0**, which AD-10 permits both to link and to derive.

Three things are *inherited from something else* and are where a `NOTICE.md` entry is needed:

* the **API descriptions themselves**: `dwarf_reader` reads a built Android `.so` and emits JSON, so
  `api_arm.json`/`stubs_arm.cc` are mechanically derived from **Khronos EGL/GLES headers** (Khronos permissive
  licence) and from **bionic's/ART's own declarations** (BSD/Apache). Not GPL, but not original AOSP authorship. [INFER]
* `gen_proxy_libraries.py` reads `external/vulkan-headers/registry/vk.xml` and shells out to a `gen_vulkan` tool for
  `libvulkan` — Vulkan-Registry licence. [1A] [INFER]
* `libnativehelper` and `libneuralnetworks` are pulled from **APEX** locations
  (`location='apex/com.android.art'`, `'apex/com.android.neuralnetworks'`) [1A], so their API surface is versioned by
  APEX, not by the platform `lib/` — a reproducibility hazard for any RetroBug codegen that assumes one tree. [INFER]

### 3.4 The argument: a generated forwarder is the wrong shape for a CPU emulator

This is the part the plan currently assumes rather than argues. `docs/10:289` says guest `libEGL`/`libGLESv1_CM`/
`libGLESv2`/`libOpenSLES`/`libandroid` come from "Generated proxies from the Berberis
`native_bridge_support/android_api` machinery". Read against what the machinery actually emits, that is the wrong
model for RetroBug, for four reasons.

1. **The guest half of a proxy carries no semantics, and its whole justification is that a translator runs it at
   native speed.** In `gen_proxy_libraries.py`, the "guest" side is `stubs_arm.cc` — real 32-bit ARM code compiled
   for arm32 — whose bodies do nothing but hand the arguments to a trampoline that switches into host x86_64 code
   calling the real implementation. [1A] [INFER] For Berberis/Houdini those stubs are nearly free, because a binary
   translator runs them at native speed once and caches the block. For RetroBug they would run **on the emulated
   CPU**, which means paying decode + IR + interpretation to execute machine code whose only content is
   "unpack r0–r3, call a host function pointer, repack r0". The proxy approach buys RetroBug nothing on the hot path
   and costs it a code-cache entry, a decode and an executor round-trip **per GL call**. [INFER]
2. **RetroBug has already built, and shipped in a passing test, the thing that makes proxy stubs unnecessary.**
   [`core/include/rb/abi/jni_env.h:4,13-15`](../core/include/rb/abi/jni_env.h) describes the guest `JNIEnv` as "a
   `JNIEnv` the guest can actually call, made of guest memory and host-call slots", and states the decisive property:
   the *entries* "**are not code at all: they are `AbiContext` slots, which the executor recognises before it
   translates, so no page has to be executable for a host function to be callable**". Every one of the 167 table
   indices is bound, measured against the NDK's own `jni.h` (`tests/corpus/jni-offsets.txt`, and the off-by-four
   trap of the four reserved NULL slots is documented at `:24-27`). That is precisely the service a proxy stub exists
   to provide — a guest-addressed entry point for a host implementation — delivered as *data*, with per-index
   named refusal instead of compiled marshalling. `libEGL` is the same object with 73 exported entry points
   (`docs/33` §3) and one ABI instead of JNI's. [1L] (repo) [INFER]
3. **A guest-side shim is still required, but only where the *guest* demands it.** Two cases genuinely need 32-bit
   code in guest address space, and they are narrow: a guest that inspects `DT_NEEDED`/`soname` or calls `dlsym` and
   branches on the returned pointer, and a guest that dereferences a **data** symbol (`SL_IID_*`, `AMEDIAFORMAT_KEY_*`,
   `__sF`, `signgam`). Those need a small, deliberately-written 32-bit `.so` — one exported *data* table plus a handful
   of thunks — and that is a BUILD-AS-GUEST job with a ~50-line C source, not a generator job.
   Everything else — `eglSwapBuffers`, `glDrawArrays` — RetroBug can serve from a `soinfo` record whose "addresses"
   are host-call slots in its own loader, with **no guest code at all**, provided RetroBug's guest linker is the one
   handing out those addresses. That is an option `docs/20` §C11 never lists, and it is the cheapest correct answer. [INFER]
4. **AOT/JIT and the tiers make the proxy shape actively harmful.** A per-call guest trampoline is a basic block
   boundary that must be re-linked per instance and per tier (T0 vs T1, `RbCodeCache` policy, AD-9); a host-side slot
   table is data and needs none of that. `docs/10` §14 already refuses to estimate proxy-call overhead (U-7's
   "`per-call IPC-class overhead, unmeasured`"), which is precisely the cost this choice creates and then never
   measures. [INFER]

**What RetroBug should take from `native_bridge_support`, concretely, for `libEGL`/`libGLESv1_CM`:**

* `android_api/libEGL/proxy/custom_trampolines_arm_to_x86.json` and the `gen_json=False` markers on
  `libGLESv1_CM`/`libGLESv2`/`libGLESv3` [1A] — a **machine-readable list of which EGL/GLES entry points AOSP decided
  it could not auto-marshal**. `docs/33` §4 item 2 derived `eglGetProcAddress` by hand from `EGL/egl.h:145`;
  AOSP's custom-trampoline JSON is the same list, curated and upstream-maintained. That is worth more than its
  generated code.
* `stubs_arm.cc` for `libandroid_runtime` — already the repo's source for the `MotionEvent` conclusion
  (`docs/research/03` §5.2 **[1A]**, `docs/33` §3). [1L] (repo)
* **Nothing as a shipped artefact, and no dependency on `m aosp_arm64`.** Removing that dependency also deletes the
  `TODO(b/110068220)`-style drift risk and the "does it still build" question from M1's critical path. [INFER]

`docs/33` §3's own classification is compatible with this: it rates `libEGL`/`libGLESv*` as "symbols-only" for *ABI*
purposes, which was the question it was asking; it is not a statement that forwarding is the right implementation
strategy, and §4's `eglGetProcAddress` exception is where the two questions part. [1L] (repo)

---

## 4. `libz` specifically

`docs/33` §3/§6 established why `libz` cannot be a generated forwarder: `z_stream` is laid out **by the guest**
(`zlib.h:124-143`), carries `zalloc`/`zfree` the host would invoke, and `deflateInit_` is a macro passing
`ZLIB_VERSION` and `(int)sizeof(z_stream)` as **runtime arguments** the callee compares against its own `sizeof`
(`zlib.h:1856-1864`) — so a pass-through forwarder returns `Z_VERSION_ERROR`. [1L] (repo, headers)
Given §1, option (a) is now measured rather than assumed:

| Option | Cost | Licence | Verdict |
|---|---|---|---|
| **(a) real 32-bit `libz.so` the guest executes** | **one `ld.lld` command, 227 544 B**, 93/93 stub symbols, `deflateInit_`/`inflateInit_`/`inflateInit2_` present with real bodies; only libc + `__aeabi_*` undefined | **zlib** (`NOTICE:9627-9629`: "Copyright (C) 1995-2022 Jean-loup Gailly and Mark Adler … provided 'as-is'") — linkable *and* derivable under AD-10 | **BUILD-AS-GUEST** |
| (b) host-side zlib + hand-written guest shim | mirror `z_stream`'s exact 32-bit layout, mirror `internal_state`, marshal every `alloc_func`/`free_func` invocation across the boundary as a `CallGuest` from whatever thread the guest happened to be on, and keep `ZLIB_VERSION` in step with whatever zlib the *host* links | same, plus `REUSE.toml` provenance for a hand-written mirror of an upstream struct | rejected |

**(a) is cheaper by a wide margin and (b) is strictly worse in kind.** (a) is 227 KB of code RetroBug never reasons
about, and it eliminates the failure modes rather than bridging them: with a real guest `libz.so`, the
`sizeof(z_stream)` the guest passes and the `sizeof` the callee compares against are computed by the same 32-bit
build, `zalloc` is a guest function called by guest code on a guest thread, and no host thread ever
enters guest address space — which is `docs/33` §4 item 3's "host→guest callbacks on threads RetroBug does not own"
problem, deleted rather than solved. (b) re-creates that problem for a library whose *only* unusual property is a
self-size assertion, and would need `adler32_simd.o`/`crc32_simd.o`/`crc_folding.o` semantics reproduced anyway to be
byte-identical. [1L] [INFER] The one thing (a) does require from RetroBug is that its guest linker resolve
`libz.so`'s remaining undefined symbols — every one measured as libc or `__aeabi_*` (`free`, `close`, `__errno`,
`__stack_chk_*`, `__read_chk`, `__write_chk`, `__vsnprintf_chk`, `__aeabi_memcpy`/`memmove`/`memclr`/`uidiv`) —
against the guest `libc.so`, so **`libz` is sequenced after
`libc`, not before it** — which is also `docs/research/02` §7.2's evidence, since AOSP puts `libz` in the
**pass-through ("original") guest library list** (`native_bridge_support.mk:63-78`) and not in the proxied list. [1A]
Same for `liblog`, `libm`, `libdl`, `libstdc++`, `libcompiler_rt`, `libsync`, `libutils`, `libcutils`,
`libandroidicu`, `libicu`, `libsqlite`, `libssl`, `libcrypto` — and note what is *absent* from `android_api/`:
**no `libz`, no `liblog`, no `libm`, no `libstdc++`, no `libc++`** [1A], i.e. AOSP's own answer to
"should `libz` be a generated proxy" is "no". [1A] [INFER]

---

## 5. ART, hidden API, and foreign-architecture JNI

**Yes — there is a documented route, and it is not hidden-API at all.** It is a versioned callback table the
*platform* offers to a translator, `NativeBridgeCallbacks`, in
`art/libnativebridge/include/nativebridge/native_bridge.h` (Apache-2.0): [1A]

```c
struct NativeBridgeCallbacks {
  uint32_t version;
  bool (*initialize)(const struct NativeBridgeRuntimeCallbacks* runtime_cbs, const char* private_dir,
                     const char* instruction_set);
  void* (*loadLibrary)(const char* libpath, int flag);
  void* (*getTrampoline)(void* handle, const char* name, const char* shorty, uint32_t len);
  bool (*isSupported)(const char* libpath);
  const struct NativeBridgeRuntimeValues* (*getAppEnv)(const char* instruction_set);
  bool (*isCompatibleWith)(uint32_t bridge_version);
  NativeBridgeSignalHandlerFn (*getSignalHandler)(int signal);
  int (*unloadLibrary)(void* handle);
  const char* (*getError)();
  bool (*isPathSupported)(const char* library_path);
  struct native_bridge_namespace_t* (*createNamespace)(…);
  bool (*linkNamespaces)(…);
  void* (*loadLibraryExt)(const char* libpath, int flag, struct native_bridge_namespace_t* ns);
  struct native_bridge_namespace_t* (*getVendorNamespace)();
  struct native_bridge_namespace_t* (*getExportedNamespace)(const char* name);
  void (*preZygoteFork)();
  void* (*getTrampolineWithJNICallType)(void* handle, const char* name, const char* shorty, uint32_t len,
                                        enum JNICallType jni_call_type);
  void* (*getTrampolineForFunctionPointer)(const void* method, const char* shorty, uint32_t len,
                                           enum JNICallType jni_call_type);
  bool (*isNativeBridgeFunctionPointer)(const void* method);
};
```

with the contract on `getTrampoline` stated as "**The trampoline has same signature as the native method.**
Returns: address of trampoline if successful, otherwise NULL". [1A] Note the two features RetroBug would need and
`docs/20` does not mention: **`shorty` is handed to the translator** (that is how it knows the argument ABI without a
per-library table), and **`getTrampolineForFunctionPointer` + `isNativeBridgeFunctionPointer` exist for the reverse
direction** — a host that receives a *guest* function pointer and must call it. That pair is the platform's name for
RetroBug's `rb::abi::CallGuest`, and `isNativeBridgeFunctionPointer` is the platform's name for the question
"`docs/33` §4 item 3: is this pointer one of ours?". [1A] [INFER] The header's struct is `NativeBridgeCallbacks`
(ART's call sites use the `NativeBridgeItf` typedef), and `version` + `isCompatibleWith(bridge_version)` are the
compatibility gate — i.e. this is a *stable, versioned, upstream-maintained* contract, which is worth knowing when
deciding how much of RetroBug's own host↔guest seam to invent. [1A]

**How the dlopen path forks.** `art/libnativeloader/native_loader.cpp::OpenLibrary` (Apache-2.0, `Copyright (C) 2015`)
does [1A]

```c
if (NativeBridgeIsPathSupported(library_path_utf_chars.c_str())) {
  handle = NativeBridgeLoadLibrary(path_arg, RTLD_NOW);
  if (handle == nullptr) { *error_msg = strdup(NativeBridgeGetError()); … }
} else {
  void* handle = android_dlopen_ext(path, RTLD_NOW, &dlextinfo);
}
```

and `art/libnativebridge/native_bridge.cc`'s own `nb_load_library` internally calls
`android_dlopen_ext(path, flags, &dlextinfo)` while `nb_get_trampoline` forwards to
`callbacks->getTrampoline(handle, name, shorty, len)`. [1A] Three architectural readings:

1. **The fork is at *namespace/path* granularity, decided before loading, and by ART — not inside the linker.**
   `bionic/linker/` has no native-bridge file (§3.1). The guest library is loaded by a *separate* `linker` instance
   (32-bit `linker`, `DT_INTERP` per `bionic/README.md`: "that's either `linker` or `linker64`") [1A], and `dlsym` on
   the resulting handle never yields a raw address. RetroBug's C06 loader already has to be the authority on guest
   handles, so this maps onto RetroBug cleanly and is where AD-5's "host-side `RegisterNatives`" sits.
2. **`JNI_OnLoad` is resolved as a trampoline, and `java_vm_ext.cc` proves it.** `art/runtime/jni/java_vm_ext.cc`
   looks the symbol up with `FindSymbol(jni_short_name, arg_shorty, jni_call_type)`, which uses
   `NativeBridgeGetTrampoline2` when `NeedsNativeBridge()` and plain `dlsym` otherwise; the shorty is passed **only**
   if a bridge is needed. [1A] So the platform's contract for `JNI_OnLoad` from a foreign-arch library is:
   *the address returned is callable from host code with the JNI signature, and the translator owns the marshalling*.
3. **`JNIEnv`: the platform never hands the guest a host function table, because it never has to build one — and
   RetroBug must.** `art/runtime/native_bridge_art_interface.cc` (Apache-2.0) is ART's side of the seam: it exposes
   the runtime callbacks that "expose `getMethodShorty` and `getNativeMethods` for trampolines via `JNIEnv`", and
   owns `LoadNativeBridge`/`InitializeNativeBridge(env, instruction_set)`/`PreZygoteForkNativeBridge`/
   `UnloadNativeBridge`. [1A] So the bridge asks ART for *signatures* and builds its own guest-callable entry;
   `libnativebridge`'s own init meanwhile uses the real host table (`env->FindClass("android/os/Build")`). [1A]
   For a binary translator that is fine, because guest→host *calls* are what the translator exists to do. For
   RetroBug it is the reverse: the guest does `ldr r3,[r0]; ldr r3,[r3,#off]; blx r3`, and if `r3` is an AArch64
   address the emulator has nothing to execute — so `core/abi/jni_env.cpp`'s rule (guest table of host-call slots,
   measured offsets, no host pointer crosses the boundary) is not an implementation convenience but the **only**
   shape that works, and RetroBug must materialise a guest `JNINativeInterface` covering **every** index of the
   table, not just the ones it implements. `docs/33` §4 item 1 already says the vtable machinery should be built
   once and generalised from the green fake `JNIEnv`; this section's finding is that **the platform's own design has
   the same requirement and encodes it as three `getTrampoline*` callbacks rather than as generated guest code** —
   which supports §3.4's conclusion. [1A] [INFER]

**Hidden API.** Nothing in `art` or `frameworks/base` documents a *foreign-architecture `RegisterNatives`* path for
app code: `RegisterNatives` is ordinary JNI on the host `JNIEnv`, and the bridge's contribution is that the
**`native` method's entry point** is a bridge trampoline (`getTrampoline`), with the runtime callbacks
`getMethodShorty`/`getNativeMethods` available so a bridge can find the entry without parsing DEX itself
(`native_bridge_art_interface.cc`) [1A] — the whole feature is scoped by one sentence in
`art/libnativebridge/README.md`: "A native bridge enables apps with native components to run on systems with
different ISA or ABI" [1A]. RetroBug's
AD-5 design (pre-shipped arm64 agent + host-side `RegisterNatives`, no generated code files) is the same mechanism
with the bridge table removed, so **U-1's residual "no better `dlopen` path exists" is re-confirmed from a second
direction**: the platform's better path *is* the native bridge, and it requires system-rollback-level integration
that an app cannot obtain. [1A] [INFER]

---

## 6. Conflicts to reconcile

Not edited, per instruction. Line numbers from the working tree on 2026-09-20.

1. **`liblog` and `libz` are classified as generated proxies in `docs/20`, and as pass-through guest libraries by
   AOSP.**
   [`docs/20-implementation-plan.md:445-447`](20-implementation-plan.md) C11: "**generated proxies** (`libEGL`,
   `libGLESv1_CM`, `libGLESv2`, `libandroid`, `liblog`, `libz`, `libjnigraphics`, `libmediandk`)" — vs
   `native_bridge_support.mk`'s `NATIVE_BRIDGE_ORIG_GUEST_LIBS` list (cited at
   [`docs/research/02-android-runtime-abi-surface.md:1061-1065`](research/02-android-runtime-abi-surface.md),
   which names `liblog`, `libm`, `libz`, `libdl`, `libstdc++`, `libsync` as unmodified), and vs `android_api/`'s
   directory set on `main`, which contains **no `libz` and no `liblog`** [1A] — plus §1's measured result that a real
   `libz.so` costs one linker command.
   **`docs/20` is wrong, and C11's own correction note already half-concedes it.** `libz` was moved to the "mixed"
   bucket by `docs/20:448`'s correction note but stayed in the generated list; the correct bucket is
   BUILD-AS-GUEST. `liblog` is a *third* case, and §3.1's `shared_libs` line proves it structurally rather than by
   list-membership: AOSP's guest `libGLESv1_CM` **links against** guest `liblog` and `libcutils`, so `liblog` must
   *exist* as a real 32-bit library for the proxy approach to work at all — it is a dependency, never a proxy.
   RetroBug's `liblog` must therefore be **built** (from AOSP `system/logging`, Apache-2.0) or served as a host-call
   slot table; either way it is not generated. [1A] [INFER]
2. **`docs/10:289` attributes the proxy machinery to Berberis and treats generated proxies as the plan.**
   [`docs/10-architecture.md:289`](10-architecture.md) "Generated proxies from the Berberis
   `native_bridge_support/android_api` machinery (Apache-2.0, genuinely AArch32 **[1A]**)". Two errors.
   (i) *Attribution*: `native_bridge_support` is `frameworks/libs/native_bridge_support`, an independent repository
   that Berberis *consumes*; and the bridge implementation moved to `art/libnativebridge` (§3.1). [1A]
   (ii) *Strategy*: §3.4's argument — with §5's evidence that the platform itself encodes this as callbacks, not as
   shipped guest code. **`docs/10` is wrong on (ii)**; the "genuinely AArch32 [1A]" claim is right but is no longer a
   reason. (Also: `docs/research/02:68-70`'s "RetroBug's engine should be a fork of Berberis" was already superseded
   by AD-1's Dynarmic fork, so citing Berberis as the supplier of anything is a leftover.)
3. **U-7 is stale in both directions.** [`docs/10-architecture.md:772`](10-architecture.md) marks it **[UNVERIFIED]**
   with fallback "build guest libs from an `android15-release` tag, or generate the ~200 needed stubs ourselves"; and
   [`docs/20-implementation-plan.md:464`](20-implementation-plan.md) C11: "Fallback if U-7's generator does not build
   for arm32: generate the ~200 needed declarations ourselves". The premise **is** maintained on `main` (§3.2), so
   the tag-pinning fallback is unneeded — but the *decision* the row supports (reuse the generator) is now the wrong
   one (§3.4), so U-7 should be re-pointed rather than closed: from "does it build?" to "do we want its output?".
   **Both sides are half-wrong**; the `docs/20` fallback sentence happens to be the one that survives.
4. **"AOSP bionic is BSD-3" is imprecise.**
   [`docs/10-architecture.md:288`](10-architecture.md) and AD-10's rationale at `:50` ("AOSP bionic is BSD-3") vs
   §2's per-directory markers
   (`libc/MODULE_LICENSE_BSD`, `libm/MODULE_LICENSE_BSD_LIKE`, `linker/MODULE_LICENSE_BSD`) [1A]. Material for
   `NOTICE.md`/`REUSE.toml` (AD-16), immaterial for the decision. **`docs/10` is imprecise, not wrong.**
5. **`docs/33` §2's stub sweep has two sibling authorities it did not consult.** `meta/system_libs.json`
   (availability per library, ABI-independent) [1L] and `android_api/*/proxy/api_arm.json` (AOSP's own
   auto-marshal-ability classification, incl. its `custom_trampolines` exceptions) [1A]. Not a contradiction — an
   unclaimed input that would have caught the `libz`/`liblog` mis-bucketing in conflict 1.
6. **`libdl`'s archive is not the guest `libdl.so`, while C11 lists `libdl` as "built from AOSP source".**
   [`docs/20-implementation-plan.md:445-446`](20-implementation-plan.md) puts `libdl` in the
   "built from AOSP source (BSD-3/Apache-2.0)" bucket, and AOSP's own list does treat it as pass-through
   (`bionic/README.md`: libdl is "just a bunch of stubs that the dynamic linker replaces with pointers to its own
   implementation at runtime" [1A]). Both are true only if RetroBug *has* a guest dynamic linker to replace them in.
   RetroBug's C06 loader is the linker, so `libdl.so` is a **RetroBug-implemented** library in guest address space
   (a table of host-call slots, per §3.4 item 3), not a bionic build. **`docs/20` is wrong by omission of that
   dependency**; `docs/33` §3's "proc-address surface, small symbol count, disproportionate work" is the correct
   reading and is already [1L].

---

## 7. What this changes for M1

The plan's current state needs no AOSP checkout, no CI build of `aosp_arm64`, and no dependency on U-7 to get its
first real guest library. Shortest path, four steps, each with an exit test:

1. **Ship the toolchain recipe, not a code generator.** Add a `guest-libs/` build that runs
   `ld.lld -m armelf_linux_eabi --shared --soname=<name>.so --whole-archive <NDK archive>` over `libm.a` (and, next,
   `libz.a`, `libstdc++.a`, `libc++_static.a`+`libc++abi.a`+`libunwind.a`). Inputs are the NDK paths
   `tools/run_engine_tests.sh` already resolves (`TC`/`$SDK/ndk/$NDK_VER/toolchains/llvm/prebuilt/windows-x86_64`),
   so it is the same toolchain discovery the fixture build uses at
   [`tools/run_engine_tests.sh:56-58`](../tools/run_engine_tests.sh) — no new environment. [1L]
2. **Gate it on the two parity checks this document invented, as tests.** (i) `comm -23` the built library's defined
   symbols against the API-level stub's `FUNC`+`OBJECT` set and require **0 missing** (measured 284/284 for `libm`,
   93/93 for `libz`); (ii) assert **no `DT_TEXTREL` and no `.rel.text`** so the guest linker never needs writable
   code, and record the relocation kinds it *does* see (`R_ARM_JUMP_SLOT`, `R_ARM_GLOB_DAT`) as C06's acceptance
   list. Also assert `sizeof`-argument behaviour, which is how `z_stream` would have been caught by a test instead
   of by a header read: call `deflateInit_` from the guest with `(int)sizeof(z_stream)` and require `Z_OK`. [1L] [INFER]
3. **Execute it.** Load the built `libm.so` with RetroBug's C06 loader, apply the PLT relocations, and call `sin`,
   `fmod` and `sqrt` through `rb::abi::CallGuest` on **both tiers**, comparing against host `libm` and against
   `qemu-arm` (AD-11). `sqrt` is the interesting one: it is `vsqrt.f64`, so this is simultaneously the first guest
   library test and the first VFP-opcode test through a *real* guest library rather than a hand-written fixture —
   which closes a hole in U-9's premise ("no instruction of a real guest app has been executed yet",
   [`tools/run_engine_tests.sh:26-28`](../tools/run_engine_tests.sh)) at the cost of one `.so`. [1L] (repo) [INFER]
4. **Only then decide the shape of `libEGL`/`libGLESv1_CM`.** Delete them from C11's "generated proxies" bucket and
   re-decide between §3.4's two live options — a hand-written 32-bit shim with data tables, or **no guest code at all**
   (C06 synthesises a `soinfo` whose exported addresses *are* host-call slots). The choice is testable with the
   `eglGetProcAddress` case `docs/33` §4 item 2 names, and `native_bridge_support`'s
   `custom_trampolines_arm_to_x86.json` is the free, upstream-maintained list of which entry points to test. [1A]

`libc.so` remains the long pole and this document shrinks it without resolving it: `libc.a` +
`libclang_rt.builtins-arm-android.a` cover 1 258/1 278 stub functions and link into a 3.28 MB `DYN`, and the single
missing input is AOSP's `bionic/libc/libc.map.txt`. Fetching that one file from `platform/bionic` at a pinned tag is
the cheapest remaining unblock, and would let `hello-jni`'s four-library closure (`liblog`, `libc`, `libm`, `libdl`
— [`docs/research/10`](research/10-milestone1-target-and-tooling.md) §3) become four build targets instead of four
open questions. **But note the ordering the evidence forces: `libm` before `libc`.** `libm_guest.so`'s residual
undefined set is *only* libc symbols, so `libm` is a complete, self-contained, testable first target; `libc` is not,
because it needs a version script *and* because its own `__libc_preinit`/TLS/scudo initialisation is exactly the six
functions `android_api/libc/` exists to override. [1L] [INFER]

---

## Appendix A — files this research created, and cleanup

Everything created was scratch, outside the repository, in `mktemp -d` directories under Git Bash's `/tmp`
(`%LOCALAPPDATA%\Temp`). No repository source, build or doc file other than this one was touched.

* `/tmp/rb34.kUbZZf/` — the **first** variant of the static-link experiment only: `m.c` (the `x = 2.0` version that
  constant-folded and pulled in zero libm objects) and the resulting `m_static` executable.
  **Not deleted: two `rm -r` attempts were refused by the tool's safety policy.** It sits entirely in the system
  temp directory and can be removed with `rm -r /tmp/rb34.kUbZZf`, or by the OS's own temp cleanup.
* `/tmp/rbscratch.FlUCY5/` — the corrected experiment and every measurement quoted in §1: `m.c`, `m.map`,
  `m_static`, `m2.c`, `m2`, `m2.map`, `m2.stripped`, `guest_user.c`, `libm_guest.so`, `libz_guest.so`,
  `libc_guest.so`, `libcxx_shared_guest.so`, `libm.asm`, `mn.txt`, `stub_m.txt`, `built_m.txt`, `s_z.txt`,
  `b_z.txt`, `s_c.txt`, `b_c.txt`, `ver.map`, `ver2.map`, `cxx.map`. **Deleted.**
* `/tmp/rbscratch_path.txt` — one line holding the scratch path so later commands could find it. **Deleted.**

Two commands were refused by the tool's safety policy and therefore produced **no** artefact: the
`libguest_user.so` consumer link of §1.2 (a guest `.so` linked against the locally built `libm.so`) and the
corrected `libc_guest.so` relink with a proper version script. Both claims in this document are flagged
accordingly — the consumer-link proof is *not* claimed as executed, and §1.2(c) marks the `libc.so` symbol-export
result as incomplete. Neither refusal invalidates a measurement quoted here.

No `adb` command was run against either attached device; all toolchain work was local. Upstream access was
read-only fetches of public `android.googlesource.com` blobs (bionic, `frameworks/libs/native_bridge_support`,
`art`), no package installation and no code executed from the network.
