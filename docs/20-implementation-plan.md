# RetroBug Implementation Plan — Phase 2

Turns [`10-architecture.md`](10-architecture.md) (AD-1…AD-18, owner-approved 2026-09-19) into components with
defined interface boundaries, a build/test capability baseline measured on this machine, and four milestones.
Scope instruction carried from the owner: **App Revival v1 is escalation-ladder mechanisms 1 and 2 only**
(API-level and syscall-level overrides); **mechanism 3 (binary patching) is held** until a real detected case
exists — §6 defines the detection signal so "real case" means evidence, not a hunch.

---

## 1. Verification capability baseline — measured, not assumed

Everything below was run on this machine on 2026-09-19. It matters because it decides what can be built green
*before* a device exists (§7 M0), and what cannot.

| Capability | Status | Evidence from this session |
|---|---|---|
| Compile ARMv7 (`armeabi-v7a`) native code | **works** | NDK r28.2 `clang --target=armv7-none-linux-androideabi21 -mfloat-abi=softfp -mfpu=neon` → ELF `Machine: ARM`, `Flags: 0x5000200`, exports `Java_com_retrobug_spike_MainActivity_stringFromJNI`, `NEEDED liblog/libdl/libc` |
| Corpus exercises ARMv7-only encodings | **works** | `llvm-objdump` of that `.so` shows `movw` (Thumb-2 / ARMv7-only) alongside Thumb-1 `push`/`pop`/`ldr` |
| Authoritative 32-bit guest ABI symbol lists | **works locally** | NDK `sysroot/usr/lib/arm-linux-androideabi/21/` stub `.so`s: `libc.so` 1094 funcs, `libGLESv1_CM.so` 278, `libGLESv2.so` 202, `libandroid.so` 177, `libEGL.so` 47, plus `libGLESv3`/`libOpenMAXAL`/`libOpenSLES`/`libjnigraphics`/`libmediandk`/`libc++`/`libm`/`libz`/`libdl` |
| Recover *per-API-level* symbol availability | **works locally, measured** | stub dirs exist for 21…35 and their symbol sets genuinely differ: `libc.so` exposes **1094 funcs at API 21 → 1226 at 24 → 1392 at 29 → 1470 at 33 → 1493 at 35**. Diffing the dirs recovers the API-availability table with no AOSP checkout. **No `.map` files ship in the NDK**, so this diff is the local source of truth |
| Build + sign a complete APK **without Gradle, Python, or `gh`** | **works** | `javac -source 8 -target 8 -bootclasspath platforms/android-36/android.jar` → `d8 --output dexout/classes.zip --lib $AJ --min-api 21` → `aapt2 link -I $AJ --manifest … --min-sdk-version 21` → `jar -cfM0` → `zipalign -f -p 4` → `apksigner sign` (keystore from `keytool`) → `apksigner verify` clean. 29 103-byte APK, `aapt2 dump badging` reports `native-code: 'armeabi-v7a'`, `minSdkVersion:'21'` |
| Disassembly / readelf / dexdump for review passes | **works** | NDK `llvm-objdump.exe`, `llvm-readelf.exe`; build-tools `dexdump` |
| Build a **Windows host binary** (M0's original "desktop tests" plan) | **impossible here** | no `gcc`, `cc`, `clang`, `cl`, or mingw on PATH. §9 item 2 and M0's exit tests are re-homed to x86_64 Android as a result — this was the plan's most wrong assumption and it was caught before writing code |
| Run x86_64 Android locally | **works** | `emulator.exe -accel-check` → "AEHD (version 2.2) is installed and usable"; an AVD already exists (`Medium_Phone`, android-37.1 google_apis_playstore/x86_64); `adb` 1.0.41 |
| 16 KiB-page Android locally | **available in the image, not yet confirmed at runtime** | the only installed system image is the `ps16k` variant, which should mean 16 KiB pages → confirms AD-7's mixed-granule path locally. **Must be proven with `getconf PAGESIZE` on a booted emulator before it is claimed**, which also contradicts `docs/21` §1's "not testable locally" line in our favour |
| `arm64-v8a` inside the x86_64 emulator | **available, by translation** | image props read `ro.system.product.cpu.abilist=x86_64,arm64-v8a`, `abilist64=x86_64,arm64-v8a`, `abilist32=` **empty**. Two uses: RetroBug's own arm64 build can run locally translated, and the empty `abilist32` independently reproduces the target device's 32-bit absence in a disposable environment |
| **Run** an `armeabi-v7a` APK | **impossible here** | host CPU is `x86_64`; the only installed system image is `android-37.1/google_apis_playstore_ps16k` with empty `abilist32`. Needs a physical 64-bit-only arm64 phone |
| Run the `qemu-arm` semantics oracle | **CI-only** | `linux-user` QEMU needs a Linux kernel; no WSL/Docker/Python locally |
| `gradle`, `python`, `zip` on PATH | **absent** | use the Gradle wrapper (needs network) or the verified no-Gradle path; `jar` from the JDK replaces `zip`; **no tool in this project may require Python** |
| `android.jar` location | `platforms/android-36/android.jar` | **not** in `build-tools/` — a repeated failure in this session's first attempts |

Two corrections to earlier planning assumptions fall out of this:

1. **`libOpenSLES.so` exposes only three symbols** (`slCreateEngine`, `slQuerySupportedEngineInterfaces`,
   `slQueryNumSupportedEngineInterfaces`) and **`libOpenMAXAL.so` is the same shape** (`xaCreateEngine`,
   `xaQuerySupportedEngineInterfaces`, `xaQueryNumSupportedEngineInterfaces`) — measured here, not inferred. Both
   real APIs sit behind opaque object/interface vtables, so a symbol-list-driven proxy generator cannot produce a
   working audio proxy: audio's guest-side surface is **hand-modelled interface tables**, and its estimate is revised
   upward (§9 R-4). By contrast `libjnigraphics.so` (4) and `libmediandk.so` (90) are ordinary flat-symbol APIs and
   generate cleanly, and `libGLESv1_CM`/`libGLESv2`/`libEGL`/`libandroid` are large flat-symbol APIs — so audio is the
   *only* category where the generator approach breaks down.
2. The **decoder-coverage audit and the API-availability table are both doable today, host-only.** They move from
   "device work" to "M0 work", which is what makes M0 worth its cost.

---

## 2. Component inventory

Nineteen components. `Tier` = which execution path uses it (T0 normal path, T1 isolated environment, BOTH).
`M` = milestone where it first has to exist. `↑` = what it depends on.

| # | Component | Namespace | Tier | M | ↑ |
|---|---|---|---|---|---|
| C01 | ARMv7 front end (vendored Dynarmic) | `rb::isa` | BOTH | M0 | — |
| C02 | Executor abstraction: Tier I interpreter, Tier J AArch64 JIT | `rb::exec` | BOTH | M0 (I) | C01 |
| C03 | Guest memory / page model | `rb::mem` | BOTH | M0 | — |
| C04 | Code cache + block cache (instance-tagged) | `rb::codecache` | BOTH | M0 | C03 |
| C05 | Guest ELF loader + relocations | `rb::loader` | BOTH | M0 | C03 |
| C06 | Guest dynamic linker / namespace semantics | `rb::linker` | BOTH | M1 | C05 |
| C07 | Syscall monitor (ARM EABI ⇄ host) | `rb::syscall` | BOTH | M0 | C03 |
| C08 | AAPCS32 call bridge (host→guest, guest→host) | `rb::abi` | BOTH | M0 | C02,C03 |
| C09 | JNI binding: agent lib, native index, guest `JNIEnv` proxy | `rb::jni` | T0 (+T1) | M1 | C08 |
| C10 | Guest VFS: resolver, mount trie, fd capability table | `rb::vfs` | BOTH | M0 (T0-only) | C07 |
| C11 | Guest system libraries (bionic build + generated proxies) | `guest-libs` | BOTH | M1 | C07,C08 |
| C12 | Graphics channel + host executors | `rb::gfx` | BOTH | M1 (GL stubs) | C08 |
| C13 | Audio + input emulation | `rb::audio`, `rb::input` | BOTH | post-M2 | C08,C12 |
| C14 | Profile store (typed, closed lookups) | `rb::profile` | BOTH | M0 | — |
| C15 | App Revival override engine (mechanisms 1+2) | `rb::revival` | BOTH | M1 | C07,C09,C14 |
| C16 | Sandbox subsystem: lifecycle, instance token, fd/service broker | `rb::sandbox` | T1 | M2 | C10,C14 |
| C17 | Diagnostics recorder ("requested vs active") | `rb::diag` | BOTH | M0 | — |
| C18 | Import pipeline: APK parse, ABI gate, native-method index | `rb::import` (Kotlin+JNI) | BOTH | M1 | C05,C09 |
| C19 | UI: library + long-press (AD-17), T1 first-run, badges, settings | Kotlin | BOTH | M1 (minimal) | C14,C16,C18 |

Dependency-critical reading: **C01/C02/C03/C05/C07 + C08 form a closed loop that can be finished and tested without
a phone and without an APK** — as a cross-compiled `rb_core_tests` binary running on the local x86_64 emulator (or on
Linux CI), driving a guest `.so` loaded into its own process with a minimal fake syscall surface. That is the single
most important structural fact in this plan: milestone 0 can reach "executes a real ARMv7 `fib()` and returns 6765"
with no device and no APK. It cannot, however, reach it as a *Windows* executable — see §1.

```
C01 → C02 → C08 ↘
C03 → C05 → C06  C09 → C18 → C19
C07 → C10, C11  ↗        C15 ← C07,C09,C14
C04 ← C02,C03            C16 ← C10,C14 (T1 only)
C12 ← C08                C13 ← C12
```

---

## 3. Interface boundaries

Each component gets: the surface it *provides*, what it *consumes*, and the invariants a reviewer must check
(Phase 3). Signatures are sketches, not final — but the **ownership, direction and invariants are the contract** and
may not change without amending this document.

### C01 `rb::isa` — ARMv7 front end
```cpp
namespace rb::isa {
struct CpuConfig { bool neon = true, vfp_v3_d32 = true, armv7_div = true,
                   big_endian = false; };          // no AArch32-hardware path (C11/C12 of docs/10)
class Frontend {                                   // wraps vendored Dynarmic A32
 public:
  std::optional<IrBlock> Translate(CpuMode mode, GuestAddr pc, const DecoderOpts&);
  const CoverageReport& coverage() const;          // which encodings are implemented
};
}  // namespace rb::isa
```
Consumes: nothing. Provides `IrBlock` to C02.
**Invariants:** the *only* place guest semantics are defined; C02 backends must add none. Vendored tree stays
byte-identical to upstream (AD-14); RetroBug changes live in `patches/` + `rb::isa` wrappers. `CoverageReport` is
never empty at runtime — an unimplemented encoding raises `ExceptionRaised`, it must not silently execute nothing.
**Review focus:** IT-state (T32 conditional blocks) is not representable outside the front end; any backend touching
condition flags is a design violation.

### C02 `rb::exec` — executors
```cpp
namespace rb::exec {
struct MachineState { GuestReg r[16]; uint64_t d[32]; uint32_t fpscr; CpuMode mode;
                      ITState it; uint32_t cpsr; GuestAddr tp; /* TLS */ };
class Executor {                         // Tier I (InterpExecutor) and Tier J (JitExecutor)
 public:
  virtual RunResult Run(MachineState&, unsigned max_instructions) = 0;
  virtual void Invalidate(GuestAddr from, GuestAddr to) = 0;   // SMC
  virtual CodeTier tier() const = 0;
};
struct ExecOpts { bool enable_jit; unsigned jit_threshold; };   // per-instance, from C14/C15
}
```
Consumes `IrBlock` (C01), `GuestMemory` (C03). Provides: `Run`, plus the exception channel to C07 (SVC) and C17.
**Invariants:** Tier I is *always available and always green* — CI runs the full ISA suite under both tiers
(`docs/10` §17 U-13 makes this the fallback path, so an untested Tier I is an unshipped fallback). `MachineState`
contains no host pointer wider than `GuestAddr` (u32) — a 64-bit field in guest state is a bug. Both tiers must
produce bit-identical `MachineState` for the same guest program (this is the *back-end equivalence* test, deliberately
**not** called a correctness test — C8).
**Review focus:** interpreter dispatch must not read Dynarmic host-codegen internals; `tp` (TLS) and `ITState` must
survive every block boundary.

### C03 `rb::mem` — guest memory
```cpp
namespace rb::mem {
class GuestMemory {
 public:
  static std::unique_ptr<GuestMemory> Create(const AddressModel&, size_t size);
  bool  Map(GuestAddr hint, size_t len, Prot, MapFlags, int host_fd, off64_t);  // 4KiB granule
  bool  Protect(GuestAddr, size_t, Prot);
  bool  Unmap(GuestAddr, size_t);
  void* Host(GuestAddr) const;                    // fastmem base + bias; nullptr ⇒ fault path
  bool  Translate(void* ptr, GuestAddr* out) const;  // host→guest, for pointer crossing
  void  NullPte(PageIndex);                       // mixed host-granule ⇒ leave fastmem
  GuestAddr Brk(GuestAddr);  PageShift GuestPageShift() const;  // always 12
};
struct AddressModel { enum Kind { kFlatIdentity, kBiased } kind; GuestBase guest_base; };
}
```
**Invariants:** `RB_GUEST_PAGE_SHIFT == 12` is a compile-time constant; host shift is read once from
`getauxval(AT_PAGESZ)` and never assumed (AD-7). **Every** pointer crossing goes through `Host`/`Translate` — no
`static_cast<T*>(guest_ptr)` anywhere in the tree (AD-6/C16: a `guest_base == 0` shortcut is a latent iOS-only bug,
so grep-forbidden in CI). OOB fastmem accesses are attributed and delivered as guest `SIGSEGV`, never host crashes.
**Review focus:** `MAP_SHARED` onto host files is C10's problem, not this one — C03 must not accept an fd it did not
obtain through C10.

### C04 `rb::codecache`
```cpp
namespace rb::codecache {
class CodeCache {                        // AD-16 seam; three backends
 public:
  static std::unique_ptr<CodeCache> Create(CodeCacheBackend, const Caps&);  // kAnonRwx|kMemfdDual|kInterpreted
  BlockSlot Acquire(const BlockKey&);   // BlockKey = {instance_id, image_id, GuestAddr pc, CpuMode}
  void      Commit(BlockSlot, const IrBlock&);
  void      FlushInstance(InstanceId);   // T1 teardown, and patch disable paths
};
}
```
**Invariants:** a `BlockKey` **always** carries `instance_id` — a cache lookup by guest PC alone is the cross-instance
aliasing hole named in `docs/research/05` §2.9. `kInterpreted` returns a slot that marks "no host code" rather than
failing, so Tier I needs no cache at all (iOS, and the U-13 fallback). Backend choice is recorded via C17, never
silently downgraded.
**Review focus:** `FlushInstance` must be provably called on every T1 exit path; executable memory is never handed to
the guest as writable-and-executable at the same time once a block is committed.

### C05 `rb::loader` — guest ELF
```cpp
namespace rb::loader {
struct GuestModule { GuestAddr base; uint64_t size; std::string soname;
                     BuildId build_id;                    // sha256, C15's match key
                     SymbolIndex exports; ExIdxTable unwinding;
                     std::vector<Needed> needed; };
LoadResult Load(const std::vector<uint8_t>& image, C03::GuestMemory&, LinkerHooks&);
}
```
**Invariants:** `build_id` is computed at load, never trusted from a filename. `R_ARM_THM_CALL`/`R_ARM_ABS32`/
`R_ARM_JUMP_SLOT` handled; a load failure is a named diagnostic stage, not a `bool`. The loader must be usable on
**host memory before any execution exists** (that is what makes M0 testable without a syscall layer).
**Review focus:** Thumb-bit correctness on entry (`|1`), `.ARM.exidx` preservation for unwinding, and that `PT_GNU_STACK`
/`PT_ARM.exidx` are not ignored.

**As built** (`30` §Step 5e — recording the deltas rather than editing the sketch above):

| Sketch | Built | Why it moved |
|---|---|---|
| `LoadResult Load(image, GuestMemory&, LinkerHooks&)` | `LoadFromFd` / `LoadFromBytes` → `std::optional<GuestModule>` + `LoadOptions{hint, apply_relocations, enforce_relro, resolve}` | `LinkerHooks` was C06's placeholder for what is really one question — "what address does this imported name get?" — so it is a `SymbolResolver` and C06 stays unbuilt until M1 needs it. The named-stage diagnostic survived as `err`, which is what makes a phone-only failure readable |
| `GuestAddr base; uint64_t size;` | `std::uint32_t base, extent` | Guest addresses are 32-bit by definition here, and a wider type only gives a load bias somewhere to grow that the guest cannot represent |
| `SymbolIndex exports` | `std::vector<Symbol> exports` + `FindExport` | A map is an optimisation over a table that is eight entries in the fixture; revisit against the first corpus `.so` rather than guessing the size now |
| `BuildId build_id` (*"sha256, C15's match key"*) | `std::vector<uint8_t> build_id`, read from `PT_NOTE`, **no hash computed** | The NDK fixture has no build-id at all, so a hash of the content is the only key that works for every image. That belongs with tier-3 patching, which is deliberately held — the gap is named here so it is not rediscovered as a surprise in C15 |
| `entry` as an address | `CodeAddress{addr, thumb}`, on `entry`/`init`/`fini` and on `FindExport`; `Symbol::value` keeps the raw tagged value | The review focus above. Masking bit 0 loses the ARM/Thumb choice, which is unrecoverable downstream and does not fault — it executes different instructions. Data relocations need the tagged value, execution needs it split out, so both are exposed |
| `ExIdxTable unwinding` | **not implemented** | `PT_ARM_EXIDX` is recognised and discarded. Needed before a guest can unwind a C++ exception across a `.so` boundary, which is a corpus-app behaviour, so it is M1 work — not silently dropped: `R_ARM_THM_CALL`/`R_ARM_COPY` and `PT_GNU_STACK` are likewise unhandled and land in `unsupported_relocs` or are ignored |

### C06 `rb::linker`
```cpp
namespace rb::linker {
class GuestLinker {                  // emulated guest linker, or AOSP arm32 linker under emulation
 public:
  GuestHandle DlopenGuest(const std::string& name, int flags, NamespaceId);
  GuestAddr   DlsymGuest(GuestHandle, const char* symbol);
  bool        LinkInto(GuestModule&, const Dtg& search_path);
};
}
```
**Decision recorded in `docs/10` §8 stands as an open choice for M1**: run AOSP's arm32 `linker` under emulation
(faithful, needs C07 complete) versus RetroBug's own loader (simpler, must reimplement namespace + TLS
semantics). **M0/M1 resolve this by trying the own-loader path first** and upgrading to guest-`linker` only if a
corpus app needs namespace behaviour we cannot reproduce. Flag: this is the plan's largest *unknown-effort* item.

### C07 `rb::syscall`
```cpp
namespace rb::syscall {
struct SyscallFrame { uint32_t nr; uint32_t arg[6]; };   // ARM EABI: r7 = nr
enum class Verdict { Handled, DenyGuest /*errno to guest*/, ForwardHost, SignalGuest };
class SyscallMonitor {
 public:
  Verdict Dispatch(const Instance&, SyscallFrame&, MachineState&);
  void    InstallGuestSignal(int sig, GuestAddr handler);   // ucontext 32-bit layout
  void    DeliverPending(MachineState&);
  // ARM-specific and NOT optional:
  void    CacheFlush(GuestAddr lo, GuestAddr hi);           // __ARM_NR_cacheflush 0xf0002
  bool    KuserHelper(GuestAddr pc, MachineState&);         // synthesised 0xffff0000 page
};
}
```
**Invariants:** guest-visible `errno` values are ARM-EABI numbers, not host ones (map is data, generated from bionic's
`errno.h`); `sigaction`/`rt_sigreturn` use the 32-bit `ucontext` layout; **no syscall reaches the host kernel except
through an explicit allowlist here**, because on T1 that same function *is* the network kill-switch for guest code
(`docs/10` §10.2). `DenyGuest` is the revival mechanism-2 hook (§6).
**Review focus:** `clone`/`futex` → real host threads with guest TLS (`tp`) set per thread; `mmap` returning an
address inside the guest window only; `execve`/`fork` refused with the *same* errno a real 32-bit Android would give
(§17 of docs/10 lists which).

### C08 `rb::abi`
```cpp
namespace rb::abi {
struct Args32 { std::vector<Value32>; };   // Value32: i32/i64/f32/f64/ptr32
Ret32 CallGuest(Executor&, MachineState&, GuestAddr fn, const Args32&, const Shorty&);
JValue CallGuestJ(Executor&, GuestAddr fn, const char* shorty, va_list);   // JNI dispatch
}
```
**Invariants:** soft-float ABI (`-mfloat-abi=softfp`) ⇒ scalars in `d0`–`d7`/`s0`–`s15` per AAPCS **and** the
Android-VFP variant rules; 8-byte stack alignment on entry, 16-byte before a `bl`; `r9`/`tp` preserved; structs
by-value copied into the guest window (never a host pointer). `Shorty` is the same descriptor ART uses, so C09 has one
encoding, not two.
**Review focus:** this file is where the ABI-correctness review pass concentrates (brief Phase 3 names it);
`jvalue`/`va_arg` marshalling and 64-bit-pair alignment for `jlong` are the classic failures.

### C09 `rb::jni`
```cpp
namespace rb::jni {
struct NativeMethod { std::string cls, name, shorty; GuestAddr guest_fn; };
class Binding {                       // one per guest instance
 public:
  static void PublishToArt(InstanceId, const std::vector<NativeMethod>&);  // host RegisterNatives
  void RunGuestJNIOnLoad(InstanceId, GuestAddr on_load, JavaVM32*, void* reserve);
  JNIEnvProxy* ForThread(HostThread*);   // 32-bit-shaped JNIEnv handed to guest code
};
}
```
Consumes C08; provides to C18/C19. **Invariants (AD-5):** **no code file is ever written or generated at runtime**;
the only host library is the one pre-shipped in RetroBug's own APK. Every `JNIEnv` function the guest calls is a
32-bit-shaped table entry implemented host-side. `FindClass` must honour the class-loader override ART applies during
`JNI_OnLoad` (`java_vm_ext.cc:1125-1142`), and `System.load(absolutePath)` resolves to the same binding as
`loadLibrary` (the case `docs/10` §7 adds).
**Review focus:** local/global reference accounting under emulation (leaked refs are invisible until they aren't),
`GetPrimitiveArrayCritical` handing out a *guest-visible* address, and ExceptionCheck after every guest→host return.

### C10 `rb::vfs`
```cpp
namespace rb::vfs {
class Resolver {                       // docs/research/05 §1.5, verbatim design
 public:
  ResolveResult Resolve(const Instance&, std::string_view guest_path, Follow, ResolveFlags);
  FdCapability Open(const Instance&, ...);            // returns entry, never a host path string
  bool         AllowSharedMap(Fd, Prot);
};
}
```
**Invariants:** resolution is component-at-a-time with `openat(O_PATH|O_NOFOLLOW)` + `fstat`; **no host path string is
ever returned to, or constructed for, the guest**; `..` clamps at the instance root; RetroBug's own fds live below
`FD_BASE` and are absent from the capability table; every host `open` uses `O_CLOEXEC`. `openat2` is emulated
(its `RESOLVE_*` flags implemented by this resolver) but never *delegated to* (unavailable on Android — `docs/10`
§10.2). On T0 this layer is advisory-by-construction and must be labelled so in C19 (§10.5 tier wording).
**Review focus:** symlink/hardlink/`/proc/self/fd`/`MAP_SHARED`/`SCM_RIGHTS` (the five the research doc ranks as the
real escapes), and that synthetic `/proc` returns **guest** paths from `readlink`.

### C11 `guest-libs`
Build-time artefacts, not runtime codegen. Two kinds: **built from AOSP source** (`libc.so`, `libm`, `libdl`,
`libc++` — BSD-3/Apache-2.0, AD-10) and **generated proxies** (`libEGL`, `libGLESv1_CM`, `libGLESv2`, `libandroid`,
`liblog`, `libz`, `libjnigraphics`, `libmediandk`).
```
generate:  NDK stub .so (arm-linux-androideabi/<api>/)  ─┐
           AOSP native_bridge_support/android_api        ─┼→ guest-libs/*.c  → clang armv7 → .so
           api-availability diff over dirs 21..35        ─┘
```
**Invariants:** symbol lists come from the NDK stubs (verified present, §1) so the ABI surface is *derived*, never
typed by hand; **nothing is ever extracted from a device image**. Fallback if U-7's generator does not build for
arm32: generate the ~200 needed declarations ourselves from stubs + our own `.map` reconstruction.
**Review focus:** OpenSL ES/OpenMAX AL are **not** symbol-generatable (§1) — their interface vtables are
hand-written and must be reviewed against the Khronos headers, not against a symbol list.

### C12 `rb::gfx` — and the T1-critical seam
```cpp
namespace rb::gfx {
class Channel {                 // AD-9: two impls from day one, no third
 public:
  virtual GlHandle CreateBuffer(BufferDesc&) = 0;      // 32-bit guest handle ← host id
  virtual void Submit(const CommandBatch&) = 0;        // kInProcess: direct;  kRing: serialised
  virtual bool Swap(InstanceId, HostSurface&, Fence*) = 0;
};
enum class Executor { kHostNativeGLES3, kHostNativeGLES1, kAngleGLES1, kSwANGLE, kAngleMetal };
}
```
**Invariants:** guest GL proxy code is executor-agnostic and links against `Channel` only, so T1's software path and
the CI oracle are the same binary as T0's; a per-instance executor choice comes from C14/C15 and is echoed "requested
vs active" through C17 (never silently substituted — `docs/research/08`'s most expensive lesson).

### C13 `rb::audio` / `rb::input`
Same channel discipline. Audio's guest interface tables hand-modelled (§1). Input's `MotionEvent` is a **real 32-bit
mirror object**, not a pointer to the host C++ object (`docs/research/03`). Deferred after M2; interface fixed now so
M1 doesn't grow a second input path.

### C14 `rb::profile`
```cpp
namespace rb::profile {
class NormalKey   { std::string guest_app_id; };                    // constructible only on T0
class IsolatedKey { std::string guest_app_id, instance_id; };        // constructible only inside a T1 instance
class Store {   // one store, one schema (AD-12)
 public:
  template <class K> std::optional<Value> Get(const K&, Domain, Name);   // closed: no ambient fallback
  ProbeSnapshot DeriveDefaults(const ImportedApp&);                       // probes, never a package table
};
}
```
**Invariants:** there is no overload accepting an un-namespaced key; defaults come from probes and a compiled
versioned table; `tier` and `patches` are separate fields and neither may reference the other in the schema
(AD-18, §12.1) — asserted by a schema test, not by convention. A T1 row is created from probes, **never copied from**
a T0 row.

### C15 `rb::revival` — see §6 for the v1 scope decision.
```cpp
namespace rb::revival {
struct Override { Intent intent;              // e.g. kReportNetworkAvailable
                  std::vector<Layer> apply;   // kArtService, kSyscall, kProperty, kGuestFile
                  EffectNote user_visible; }; // wording rule from docs/10 §12.1
class Engine {                                // mechanisms 1+2 ONLY in v1
 public:
  OverrideSet Resolve(const ProfileKey&);                 // per (guestAppId, instanceId)
  void RecordLayerConsulted(Intent, Layer, bool answered); // ← the tier-3 trigger, §6.3
};
}
```

### C16 `rb::sandbox` — T1 lifecycle
```cpp
namespace rb::sandbox {
enum class State { kNone, kCreating, kReady, kRunning, kReaped, kTearingDown, kFailed };
class Manager {
 public:
  Instance Handle Create(const IsolatedKey&, const ProbeSnapshot&);
  EnterResult Enter(const IsolatedKey&);      // binds service, runs SELF-TEST, then and only then reports "isolated"
  void        Exit(InstanceId);  void Teardown(InstanceId);   // asserts capability table + cache empty
};
struct SelfTest { bool uid_in_band, cannot_open_own_files, cannot_create_inet_socket,
                         cannot_see_other_instance, dns_brokered; };   // docs/10 §10.4
}
```
**Invariants:** `Enter` **fails visibly**; it never falls back to in-process launch, and the guest loader is not
instantiable in the host process for a T1 entry (test T-37). `kReaped` is a real state (the system kills isolated
processes eagerly), so nothing may be process-resident-only. `SelfTest` gates the word "isolated" in all UI strings.

### C17 `rb::diag`
Stage recorder with the `>>> INITIALIZATION FAILED AT: <stage> <<<` shape and mandatory requested-vs-active echo;
serialised into the instance dir and copyable from the UI. Every other component's "must be reviewed" line above is
expressed as a `diag` field, because that is what makes review findings mechanical rather than aspirational.

### C18 `rb::import` / C19 UI
Import: read manifest, gate on `native-code: 'armeabi-v7a'`/ABI presence, parse DEX for `native` methods,
index guest `.so` exports and build-ids → `ImportedApp`. Nothing may require user input here (f).
UI: library list with the AD-17 long-press **and** its accessible overflow alternative, T1 first-run contract screen,
persistent tier badge, advanced settings listing probe results and active overrides with plain-language effect
wording. Single tap ⇒ T0. Always.

---

## 4. Repository layout and build targets

Extends the Gradle+NDK shape the existing `.gitignore` already implies; the dual Gradle/Xcode CMake layout is
adopted from the developer's proven `XenDroid` arrangement.

```
core/                     rb::isa exec mem codecache loader linker syscall abi jni vfs revival profile diag
  CMakeLists.txt          → librb-core.a (host-testable, no Android headers)
external/dynarmic/        vendored, byte-identical (AD-14)
patches/dynarmic/*.patch  RetroBug-side changes only
guest-libs/               build + generation recipes (C11); no .so committed until M0 decides
platform/android/         app/ (Gradle), agent .so, RbCodeCache AnonRwx/MemfdDual, RbGfxChannel impls,
                          sandbox service declarations, isolatedProcess host/broker
platform/ios/             Tier I default, RbCodeCache Interpreted, ANGLE-metal, in-process monitor
tests/isa/                ARM-ARM-derived vectors; runner on host; both tiers; qemu-arm oracle in CI
tests/corpus/             no-Gradle build scripts for fixture APKs (verified path, §1)
tests/leak/               device leak-probe fixtures (M2 gate)
docs/                     00 current state, 10 architecture, 20 this plan, research/*
NOTICE.md  LICENSES/  REUSE.toml                  (AD-16, from the first commit)
```

Build targets, in the order a developer needs them: `rb_core_host_tests` (desktop, no device),
`rb_isa_coverage` (desktop: corpus disassembly vs `CoverageReport`), `librb_agent_android` (arm64 `.so` in the APK),
`retrobug.apk` via Gradle wrapper **or** the verified no-Gradle script, and `rb_leak_probe.apk` for M2.

---

## 5. Milestones

`M0` exists for one reason: the device is the scarcest dependency, so everything not requiring one is pulled
forward and made green first.

### M0 — emulation core bring-up: no phone, no APK *(fully local)*

Test target is **x86_64 Android binaries cross-compiled by the NDK and run in the local emulator**
(`rb_core_tests` pushed with `adb push`, executed over `adb shell`) — not a Windows host build, which this machine
cannot produce (§1). Dynarmic's existing **x64 backend for the A32 front end** makes this more than a stand-in:
Tier J is genuinely exercised here, and `docs/research/07` notes that same backend is what its CI differentially
tests against. Linux CI carries the `qemu-arm` oracle and the arm64 build.
| Deliverable | Acceptance test |
|---|---|
| Dynarmic vendored + patched; Tier I and Tier J over one IR | same guest program bit-identical in `MachineState` under both tiers (back-end equivalence, not semantics) |
| Semantics vs external oracle | `qemu-arm` linux-user on CI, ≥ 1 000 instruction-vector programs, **0 mismatches**; ARMv7 barriers/LDREX/IT-block/NEON each explicitly covered |
| Decoder coverage audit | `llvm-objdump` over corpus `.so`s ⇒ every mnemonic in `CoverageReport` or a triaged gap list. **This closes U-8 host-side** |
| `rb::mem` + address models | 4 KiB guest / 16 KiB host mixed-granule tests (the local image is a `ps16k` build — confirm `getconf PAGESIZE`, §1); `guest_base != 0` exercised under x86_64 Android so no identity-only code path survives |
| `rb::loader` | loads a real `armeabi-v7a` `.so` (the one built in §1) into a host process, applies relocations, resolves `libc.so` symbols to our own implementations |
| `fib(20) == 6765` | **the M0 exit test**: call the loaded `Java_..._stringFromJNI` through `rb::abi` with a fake `JNIEnv`, under Tier I and Tier J, assert identical result |
| Audio proxy strategy locked | one stub-lib sweep confirms §1's finding for every remaining guest library, so no interface-vtable API is ever assumed symbol-generatable |

M0 is the plan's risk-killer: it validates AD-1 (Dynarmic fit), U-8 (ARMv7 coverage), C8's oracle strategy, and
AD-6/AD-7 (address/page model) before a single line of Android UI or JNI code exists.

### M1 — one simple 32-bit APK launches end-to-end on the **normal (non-VM) path**, Android
`hello-jni`-class fixture (own NDK-built APK from §1, `armeabi-v7a`-only) plus one third-party FOSS app from
`docs/research/10`.

Required to exist: C06, C07, C09, C11, C12 (GL can be a stub that draws nothing), C14, C15 (mechanisms 1+2, §6),
C17, C18, and C19 at "list, single-tap launch, long-press menu present" fidelity.
Exit criteria: import → select → launch with zero configuration; guest native code executes **on the emulated CPU**
and the guest's text appears on the host-rendered screen; `rb::diag` shows active tier + address model + code-cache
backend; the T1 option is present via long-press but **does nothing yet**. `U-2` (4 GiB reservation) and `U-11`
(hidden-API hosting) are answered on real hardware here, and their fallbacks are already designed (§10.7, `docs/10` §6).

**M1's device step is defined, not deferred** — see [`21-m1-device-acceptance.md`](21-m1-device-acceptance.md).
Agreed device: **OnePlus 12 / Snapdragon 8 Gen 3, confirmed to have no 32-bit ARM support at all**, which makes it
the target class exactly (ARMv9.2 cores drop AArch32 at EL0) and lets the acceptance test rest on an *OS-level*
negative control: the platform itself cannot install the guest APK. The runbook's governing rule is that at least one
piece of evidence must come from something other than RetroBug's own logs, since a buggy build could otherwise
report a tier it was not running. Two gaps are recorded rather than papered over: this device has 4 KiB pages, so
AD-7's mixed-granule path stays host-tested-only, and one OEM build is device #1 of the U-13 matrix, not the matrix.

### M2 — one app inside the isolated environment, **isolation verified not assumed**
Required: C16, `RingChannel` (C12), SwANGLE in-sandbox executor, self-test battery, T1 UI (badge, first-run
contract, exit affordance), leak-probe fixtures.
Exit criteria = `docs/research/05`'s test matrix green on a device: one app runs in T1 end-to-end; a second app in
its own T1 cannot see the first's files, sockets, or profile; T1 cannot create an `AF_INET` socket at the syscall
**and** DNS levels; T1 config never appears in T0 or in the other instance; `Teardown` asserts empty capability table;
any unachievable claim is written into the disclosure list rather than quietly omitted.

### M3 — breadth, after the two brief-mandated milestones
Graphics fidelity (GLES1 executor selection, ANGLE/SwANGLE tuning), audio/input (C13), the second and third tiers of
the App Revival corpus, iOS platform layer (Tier I default, `Interpreted` code cache, ANGLE-metal channel, biased
address model), and the isolated-environment *hardware* graphics ring as an opt-in.

---

## 6. App Revival v1 — mechanisms 1 and 2

### 6.1 Scope
Implemented now: **mechanism 1** (API/service-level override on the host-ART side) and **mechanism 2**
(syscall/VFS/property-level virtualisation). Held: **mechanism 3** (hash-gated binary patching + guest-code
emitter). Holding it costs nothing structurally, because both mechanisms and the held one consume the *same*
`Override` object (§C15) — adding tier 3 later adds a `Layer`, not a redesign.

### 6.2 One intent, applied at every layer the guest might read
The design point worth stating explicitly: an override is an **intent**, not a hook at one layer. `kReportNetworkAvailable`
resolves to a set of layers, and RetroBug answers at all of them simultaneously:

| Layer | What it virtualises for that intent |
|---|---|
| `kArtService` | the guest's `ConnectivityManager`/`NetworkInfo` view through RetroBug's own `Context` (§AD-4) |
| `kSyscall` | `ioctl(SIOCGIFFLAGS/SIOCGIFADDR)`, `getifaddrs` backing calls, socket-family queries |
| `kGuestFile` | synthetic `/sys/class/net/wlan0/operstate`, `/proc/net/dev`, `/proc/net/if_inet6` (C10 already serves a synthetic `/proc`) |
| `kProperty` | `net.dns*`, `net.<iface>.*`, `ro.telephony.*` reads — which **issue no syscall**, so a syscall-only filter provably cannot implement this override; that is why mechanism 2 owns properties too |

This kills the whack-a-mole failure mode: an app whose check reads `operstate` instead of the API still gets the
answer, and we don't have to know which it reads in advance.

### 6.3 What counts as the "real detected case" for tier 3
`Engine::RecordLayerConsulted(intent, layer, answered)` writes to `rb::diag`. Tier 3 becomes justified **only** when,
for a specific app, all of:
1. the override is active and its layers all report "answered",
2. the app nevertheless fails the same check (crash or exit at the same diagnostic stage twice in a row), and
3. the corpus disassembly shows the guard reading **no** interceptable surface — i.e. the value is computed and
   latched in native code.

Those three lines are emitted as one `revival.tier3_candidate` diagnostic with the app id, the library name and the
candidate site. Until such a record exists, mechanism 3 stays unbuilt; if it does, it is a *data* addition (§C15)
plus the emitter, not a new subsystem. This is the mechanism that keeps "hold tier-3" from silently becoming
"never build tier-3".

### 6.4 Independence restated as a test
A profile for a **T0** app carrying `kReportNetworkAvailable` must work with no T1 record in existence; a T1 app with
**no** overrides must work. Both directions are schema tests in `tests/`, per AD-18.

---

## 7. Phase 3 verification protocol (applies per component as it lands)

1. **Implementation commit, then a separate review pass** — a sub-agent re-reads the code against `docs/10` + the
   relevant research doc, never against my summary of it. Fixed review targets per component are the "Review focus"
   lines in §3 (ABI/`rb::abi`; IT-state and flags in `rb::isa`; escape routes in `rb::vfs`; reference accounting and
   class-loader override in `rb::jni`; instance tagging in `rb::codecache`).
2. **Mandatory edge-case re-analysis** before any "done": native `.so`-only apps, JNI-heavy apps (callbacks into guest
   code, `RegisterNatives`, `GetPrimitiveArrayCritical`), apps pinned to odd `targetSdkVersion`, self-modifying/JIT-ish
   guest code (→ `CacheFlush` + write-tracking), `libstdc++` vs `libc++_shared` bundles, and 16 KiB-host mixed
   granules.
3. **Shims must match real Android behaviour, not assumed behaviour**: every proxy function's semantics are checked
   against AOSP/bionic source or the Khronos/NDK headers, and each mismatch becomes a `docs/` note, not a code comment.
4. Nothing is marked done on first pass; uncertainty is written into `docs/10` §17's table with its experiment.

---

## 8. Risks and gates

| # | Risk | Gate / mitigation |
|---|---|---|
| R-1 | `rb::linker` effort unknown (own loader vs emulated AOSP linker, C06) | decide empirically inside M1 on the first corpus app that needs namespaces; time-box the own-loader attempt |
| R-2 | U-2 reservation failure ⇒ biased mode on Android | `AddressModel` already parameterised; M0 runs `guest_base != 0` on x86_64 so no identity-only path survives |
| R-3 | U-10 (DEX load inside `isolated_app`) fails ⇒ T1 = native-only | M2 gate; disclosure wording pre-drafted (`docs/10` §10.5); does not block M1 |
| R-4 | audio proxy surface larger than the 3-symbol stub suggests (§1) | re-estimate when C13 is scheduled; explicitly deferred out of M1/M2 |
| R-5 | Tier I speed unknown (U-9) | M0 measures IR-ops-per-guest-instruction on a real `.so`; **no performance claim in any user-facing text before M0 numbers exist** |
| R-6 | Dynarmic maintenance burden | `CoverageReport` + oracle green in CI before any core change merges |
| R-7 | Play policy on APK-running apps (U-12) | ships zero APKs, no downloader; sideload channel kept as fallback; unresolved but non-blocking |

---

## 9. M0 task breakdown (the immediate next work, all local)

Progress and the findings that change this plan are tracked in
[`30-m0-notes.md`](30-m0-notes.md). Status as of `30` §Step 5e:

1. `core/` + `external/dynarmic` vendoring with `patches/` layout and `NOTICE.md`/`LICENSES/`/`REUSE.toml` in the
   same commit (AD-16: obligations start now). **Done** (`419fe9f`), with the one mechanical deviation recorded in
   `external/PROVENANCE.md`.
2. `rb_core_tests` target, cross-compiled for x86_64 Android and run in the local emulator via `adb shell`
   (no Windows host build is possible here — §1), plus a Linux CI job carrying the `qemu-arm` oracle.
   **Done except the CI job.** The tests were re-homed to run on arm64 **phones** as well, which turned out to be
   the better lane: three of this step's findings (Step 5c, 5e) only reproduce on 4 KiB hardware.
3. `rb::isa` wrapper + `CoverageReport`; corpus disassembly job → coverage gap list (**closes U-8**).
   **Half done, and the halves are unrelated.** The corpus measurement landed as `docs/32-isa-coverage.md` and
   settled the demand question; the wrapper and its mnemonic→handler coverage tool are unwritten, so the
   "what is still unimplemented" side is still open.
4. `rb::mem` with both address models + mixed-granule tests. **Done** — 131 checks, 0 failures, executed on a
   OnePlus 12. The injected 16 KiB cases run on any host; the *native* 16 KiB ones still need the emulator.
5. `rb::loader` → load the §1 `.so`. **Done** — 71 checks, 0 failures on hardware, with `llvm-readelf` as the
   external oracle rather than the loader's own parser (`30` §Step 5e). Three bugs found and fixed in getting
   there; one of them was in `rb::mem` and had been silently corrupting every load.
6. `rb::exec` Tier I; then Tier J behind `RbCodeCache`.
7. `rb::abi` `CallGuest` + fake `JNIEnv`; **M0 exit test: `fib(20) == 6765`, both tiers, identical state**.
8. `docs/research/01` corrections (stale `MemoryProcessor` API; no interpreter upstream) and `docs/00` §2 note that
   the no-Gradle build path is verified.

Sequencing constraint the owner should know before M1: **M1's end-to-end acceptance step cannot be verified on this
machine at all** — it needs a physical 64-bit-only arm64 phone. Everything in §9 and most of M1's code can be written
and tested without one, so the device gates only the final launch assertion, not the work.
