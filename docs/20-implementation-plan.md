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
   upward (§9 R-4).
   > **Corrected by [`docs/33`](33-guest-library-inventory.md), which ran the sweep this paragraph should have run
   > before generalising.** Every measurement above is right and re-confirmed; the conclusion drawn from them is not.
   > Audio is **not** the *only* category where the generator approach breaks down — `libandroid`
   > (`ANativeActivityCallbacks`, 15 function pointers in a struct the guest calls through), `libmediandk`
   > (`AMediaCodec_setAsyncNotifyCallback` takes a **struct of 4 function pointers by value** — `NdkMediaCodec.h:150-154,499-502`,
   > verified here **[1L]**, so 4 guest words vs 4 host words is not the same AAPCS expansion), `libz`
   > (`z_stream` carries `zalloc`/`zfree`, is laid out *inline* by the guest, and `deflateInit_`/`inflateInit_` pass
   > `(int)sizeof(z_stream)` as a runtime argument the callee compares against its own — `zlib.h:124-143,1856-1864`
   > **[1L]**), `libdl`/`libEGL`/`libvulkan` (pointer-returning entry points: `dlsym`, `eglGetProcAddress`,
   > `vkGetInstanceProcAddr`), `libaaudio`, `libcamera2ndk` and `libbinder_ndk` each break it too. And
   > `libjnigraphics`/`libmediandk` are *not* "ordinary flat-symbol APIs": `libmediandk` is 161 FUNC against 143
   > `AMEDIAFORMAT_KEY_*` OBJECT symbols. **The discriminator is not "audio vs not-audio"**, it is whether any value
   > crossing the boundary is a guest-callable pointer or a layout both sides must agree on — `docs/33` §6 states it
   > as a rule and §4 lists what must be hand-modelled before C12/C13 can claim coverage. Also from that sweep:
   > `libc++.so` in the stub dirs is a 19-byte `INPUT(-lc++_shared)` linker script, not a stub, so the C++ ABI
   > surface has no stub table and belongs to `libc++_shared.so`/AOSP; and a generator filtered on `FUNC` silently
   > emits a half-complete library (`libOpenSLES` 3 of 56, `libmediandk` 161 of 304).
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

**As built** for Tier I (`30` §Step 6d) — two constraints on this sketch that only showing up in the IR revealed:

- `MachineState` as written is complete, deliberately: CPSR's Q (bit 27) and GE (bits 19:16) live *inside*
  `cpsr` rather than as fields. The back ends keep `cpsr_nzcv`/`cpsr_q`/`cpsr_ge` apart, but that is a host
  layout choice, and splitting an architectural register into three fields would make the bit-identical-state
  assertion above check the wrong thing.
- **The flag contract is per-instruction, not per-machine.** `Add32`/`Sub32` and the shift family carry a
  `carry_in` operand (not a set-flags bit), and flags are consumed by pseudo-operations that name their
  producer — `GetNZCVFromOp`, `GetNZFromOp`, `GetCarryFromOp`, `GetOverflowFromOp` — falling back to
  `TST v, v` (which clears C and V) when the producer has no flag-form. Any future tier, including Tier J's
  C04 harness and anything that tries to share code between the two, has to model that as a map keyed by
  IR instruction. One mutable `Flags` in `MachineState`-adjacent state is how `push` came to set `C` and
  every recursive `fib` call returned an arithmetic series instead.

**As built** for Tier J (`30` §Step 9) — the sketch's `Executor` held, and four things became visible only
when a second tier had to be compared with the first:

- The config is pinned for accuracy: `arch_version = v7` (Dynarmic's default is **v8**), forced
  little-endian, `check_halt_on_memory_access`, and **no `fastmem_pointer` and no page table** — so compiled
  code reaches the same `GuestMemory` the interpreter walks, and a disagreement can only be a back-end
  disagreement. Fastmem is the speed story and belongs with C03/C04, not with the equivalence test.
- **Dynarmic caches the block it builds for an unreadable address**, so an unmapped fetch reaches
  `MemoryReadCode` exactly once and the second visit yields no callback at all. The fault reason therefore
  has to be surfaced from `ExceptionRaised(NoExecuteFault)` as well, and the first reason recorded wins.
- `instructions_executed` is a count **in the tier's own unit** today — IR ops in Tier I, guest instructions
  in Tier J — which is documented on the field and is C02 work to unify *before* C14/C15 put a per-app
  budget on it. `Run(state, max_instructions)` reading as one number per tier is the wrong assumption.
- What Tier J still cannot do: read or write `tp` (`tpidrurw` needs a `Coprocessor` hook; Dynarmic models it
  nowhere), reach `GetGEFromOp`/SIMD32 for the same reason Tier I cannot, or run under C04's cache. None of
  these is a *difference* between the tiers, which is the point of listing them here rather than in U-13.

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

**As built, policy half** (`30` §Step 13, 86 checks): the sketch's `CodeCache` exists as
`rb::codecache::Cache`, and the split that fell out of building it changes what C04 is *for*.

- **The memory is injected, so the three backends are M1's, not this component's.** `Reserve` hands out
  writable space, `Commit` makes a prefix of it executable and *not* writable, `Release` takes the whole
  reservation back — and that ordering is asserted against a fake allocator, so W^X is checked with no
  `mmap` in `core/` at all. `AnonRwx` and `MemfdDualAlias` land where the U-13 probe result is consumed,
  which is also where their real constraints (SELinux, a memfd's dual mapping, iOS's outright refusal) live.
- **`FlushInstance`/`Forget` return the guest ranges to invalidate; they do not invalidate.** Step 9
  settled that host-cache maintenance belongs to whichever back end emitted the code — Dynarmic today — so
  C04 owns *what to drop, when, and for whom*. `Instances()` gives the T1 teardown the list that makes
  "provably called on every exit path" checkable; the proving itself is C16's and is not done.
- **`kInterpreted` returns a slot with no range rather than failing,** which is what lets Tier I run with
  no cache on iOS or on an OEM whose probe says no. A cache that answered `kNoRoom` there would be a
  fallback that cannot be taken.
- **An entry is neither findable nor evictable until it is committed.** Two tests were written before the
  code got this right and failed, which is the only reason it is now correct: an uncommitted block is a
  back end mid-write, and evicting one releases memory from under someone's emitter.
- `Kind`, per-instance `Limits` (bytes and blocks), LRU eviction *within one instance only*, and the
  requested-vs-active echo into C17's `code-cache-backend`, recorded `inert` when a stronger backend was
  asked for and a weaker one is in use.
- **The Tier J wiring was investigated and the answer is a boundary, not a to-do** (`30` §Step 18): this
  vendored Dynarmic allocates its own executable memory and exposes no injection point, so the
  `Allocator` seam has no consumer on the JIT path. What C04 governs there is exactly one number —
  `UserConfig::code_cache_size` per `Jit`, hence per instance — plus `kInterpreted` and its echo, and the
  guest-range algebra both tiers use when invalidating. Three consequences that have to travel with the
  component: eviction on that path is **the whole cache at once** below 1 MiB remaining
  (`backend/arm64/address_space.cpp:110-113`), so a per-instance quota is a latency cliff rather than an
  LRU and has a **floor of about 1 MiB + ε** (C04's default is 2 MiB, now stated at the struct);
  `code_cache_size` is hard-capped at 128 MiB by an assert; and **fill level is not observable**, so the
  C17 field can report budget and backend, never occupancy. Cross-instance isolation, the property
  `docs/research/05` §2.9 demanded, holds by construction — one `Jit` each — which is worth saying
  plainly rather than attributing to the key. Getting a cache RetroBug actually owns (memfd dual-alias, no
  RWX window, per-block LRU) means injecting an allocator into `AddressSpace`: ~2 lines of
  `external/dynarmic`, permitted by its 0BSD licence, forbidden by the no-patches posture so far held for
  18 steps. Nothing in M1 needs it — the app-domain probe measured anon RWX *and* memfd execution as
  available — so the decision belongs to iOS/M3, where the first step is measuring whether the back end's
  existing `ProtectCodeMemory()` (which already keys on `__APPLE__`) is sufficient before paying for the
  patch at all.
- **`BlockKey` was missing a field and now has it:** `itstate`. Instance, image, pc and mode were all there,
  but `isa::TranslateOne` documents that pc, mode *and* IT state decide what a word of guest bytes
  decode to, and Dynarmic keys its own cache on the whole location descriptor. Without it, a block
  translated inside one predicated sequence is served to a fetch inside another — a wrong-answer alias,
  the same class as dropping the mode, and the reason the key is tested field by field.
- Still open: thread safety (one cache per instance *and* per thread until C16's threading story exists),
  and any measurement behind the eviction policy or the sizing — both are `[INFER]` until a real app's
  block trace says otherwise, and note that on the Tier J path such a trace cannot be taken at all from
  outside the back end.

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
| `ExIdxTable unwinding` | **not implemented** | `PT_ARM_EXIDX` is recognised and discarded. Needed before a guest can unwind a C++ exception across a `.so` boundary, which is a corpus-app behaviour, so it is M1 work — not silently dropped: `R_ARM_COPY` and the TLS kinds are unhandled and now refuse the load, and `PT_GNU_STACK` is ignored |
| (no such field) | `LoadOptions::tolerate_unresolved_relocs`, **default false**; `GuestModule::ifunc_slots`, `unsupported_relocs`, `unsupported_reloc_types`, `Symbol::ifunc` | The three TLS slots in a real `libc.so` made the old record-and-continue default a silent-wrong-value machine: bionic computes those offsets into addresses at load, so a skipped one is read as a live pointer with nothing to indicate it was never filled. Refusing by default, with every slot named, is the owner's call (`30` §Step 27). IFUNC is the one case the caller *can* finish itself, so it is recorded rather than refused — and `30` §Step 27 measured that its slot is **read-only by the time the load returns**, which puts the resolution call inside `LoadFromBytes`, before the RELRO pass |

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

**As built, first slice** (`30` §Step 11; `core/syscall/monitor.cpp`, 77 checks) — one invariant above was
stated as a fact and turned out to be only half right, so it is corrected here rather than left to be
re-discovered:

- **Syscall *numbers*: as claimed, and worse than expected.** Measured from both ABIs' own headers on the
  SM-X710: **every** name shared between the guest's ARM EABI table and the host's AArch64 table differs
  (`read` 3↔63, `write` 4↔64, `gettid` 224↔178, `openat` 322↔56, fourteen of fourteen), and five guest
  numbers have no host counterpart at all (`mmap2`, `sigaction`, and the three `__ARM_NR_*`). A forwarded
  call that reuses a number rather than translating by name is wrong in every case, not some.
- **Errno *values*: identity today, measured.** The classic claim is that ARM EABI errnos differ from the
  generic ones (`ENAMETOOLONG` 62 vs 36, `EOVERFLOW` 75 vs …), so all 47 sampled names were printed from
  both ABIs: they agree on every one, and a real `openat` of an over-long name from a 32-bit process
  returned **-36**, i.e. the kernel this runs on already speaks bionic's numbers. So the invariant's
  *conclusion* holds — resolve errnos by name, never copy a number across — but its premise ("not host
  ones") is false for this ABI pair. `err::ToGuest` is therefore an identity function that exists as a
  place, and the test asserts the numbers came from a measurement rather than from the history book.
- **The return convention is not the textbook one.** The usual statement — "negative errno in r0 with
  CPSR.C set, clear on success" — is wrong on the measured device: C is set after *every* return tried,
  including `sched_yield` succeeding with 0. The discriminator is the `-4095..-1` range, which is what
  `LooksLikeError` implements and what `ApplyReturn` relies on, leaving CPSR alone. Synthesising the flag
  would be a silent behavioural difference from every real 32-bit Android process.
- **Nothing is forwarded.** `Verdict::kForward` exists with no allowlist row behind it, so the posture is
  data-shaped rather than absent-shaped, and `Monitor::Echo` records it as C17's `syscall-policy` field
  ("requested: no forwarding; active: nothing reached the host kernel — the guest asked for getpid x1,
  openat x2"). Refusals name the component that is missing (`openat … C10 lands first`), because a bare
  ENOSYS in a support report describes a gap nobody can triage.
- **`execve`'s refusal errno is measured, and it is not `ENOSYS`.** A 32-bit app process on the SM-X710
  executing a file it had written itself into its own `cache/` got **`-EACCES`** (`30` §Step 12), which is
  the SELinux `app_data_file:file execute_no_trans` rule. The monitor answers `Verdict::kDenied` with 13
  and names the measurement in the `detail`, because `ENOSYS` and `EACCES` tell an app developer two
  different stories and only one of them is true here. `fork`/`vfork`/`clone` remain `ENOSYS`-refused and
  say they are unmeasured: asking them means forking inside the probe, and the last time that was done the
  child's inherited stdio buffer duplicated half the report.
- **The seccomp half of this component's premise changed.** `docs/10` §10.2's filter is now justified by
  measurement rather than by policy reading: none of twelve ARM EABI numbers trapped in the app domain on
  this build, so nothing the platform does can be relied on to refuse anything for a guest.
- **`__ARM_NR_set_tls`/`get_tls` work now**, through `MachineState::tp`. That is the syscall half of TLS;
  the `mrc` read of `tpidrurw` still needs the `Coprocessor` hook (`30` §Step 9), and guests from different
  toolchain generations use one path or the other, so both have to exist before "TLS works" is true.
- **The loop exists** (`30` §Step 11d): `AbiContext::CallGuest` services a syscall the way it services a
  host call, `exit_group` ends the call with `StopReason::Exited` (appended to that enum on purpose: its
  numbers appear in differential logs), and a guest with no monitor attached fails rather than continuing
  past an unanswered SVC. `arm_cacheflush` reaching the tier's `Invalidate` is now proven with the
  *guest's* instruction — including that a flush of an unrelated address leaves the cache stale, which is
  the assertion that makes the range meaningful instead of decorative.

### C08 `rb::abi`
```cpp
namespace rb::abi {
struct Args32 { std::vector<Value32>; };   // Value32: i32/i64/f32/f64/ptr32
Ret32 CallGuest(Executor&, MachineState&, GuestAddr fn, const Args32&, const Shorty&);
JValue CallGuestJ(Executor&, GuestAddr fn, const char* shorty, va_list);   // JNI dispatch
}
```
**Invariants:** soft-float ABI (`-mfloat-abi=softfp`) ⇒ **every argument and every result in the core bank**:
a scalar takes one of `r0`-`r3`, an 8-byte one (integer *or* `double`) an even/odd pair, a `jdouble` result
comes back in `r0:r1`, and the argument block starts at the callee's entry SP with the 16-byte
intra-procedure area *below* it. Nothing is allocated from `d0`-`d7` on either side of the boundary — that is
the *hard*-float convention (`-mfloat-abi=hard`), which the NDK refuses for `armv7-none-linux-android`, so no
guest RetroBug can build uses it. `30` §Step 25 has the disassembly table this rule is taken from; §Step 7a
carried the opposite rule as an invariant until §Step 25, and both directions were separately wrong (§Step 24
the results, §Step 25 the arguments). Also: 8-byte stack alignment on entry, 16-byte before a `bl`; `r9`/`tp`
preserved; structs by-value copied into the guest window (never a host pointer). `Shorty` is the same
descriptor ART uses, so C09 has one encoding, not two.
**Review focus:** this file is where the ABI-correctness review pass concentrates (brief Phase 3 names it);
`jvalue`/`va_arg` marshalling, 64-bit-pair alignment for `jlong`, **and the float kinds** are the classic
failures — the third is the one that actually shipped here, twice, and the reason it is not enough to check
that the integer rows pass.

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
> **Three corrections to those buckets, so read them as labelled rather than as the plan.**
> **(1) `liblog` is not a proxy.** AOSP's own guest `libGLESv1_CM` declares `shared_libs: ["libcutils", "liblog",
> "libnative_bridge_guest_libEGL"]` — `liblog` is a *dependency* of the guest build, i.e. AOSP links against it
> rather than generating it **[1L]**, and the directory listing of `native_bridge_support/android_api/` shows
> no `liblog`, no `libz`, no `libm` and no `libc++` at all **[1L]**. It belongs
> with `libm`: a small, stable export surface (18 `FUNC` in the API-34 32-bit stub, counted here **[1L]**) that
> forwards to the host's logging.
> **(2) `libz` is neither a proxy nor a problem.** `android_api/` has no `libz` — AOSP's answer to "proxy libz?"
> is *no* **[1L]** — and the NDK ships a real compiled 32-bit `libz.a` that relinks to a genuine `libz.so`
> (227,544 B, 93/93 parity with the stub **[1A]**, and the same relink was reproduced here for `libm` **[1L]**).
> So it moves to the "already built, just link it" bucket with `libm`, which is where the struct-by-value
> `z_stream` problem in the next quote never arises.
> **(3) `libdl` cannot come from the NDK.** Its `libdl.a` is one 4,908-byte member **[1A]**, and the guest
> `libdl.so`'s real content is the *guest linker's* — so it depends on the C12 / §8 `linker` decision, not on a
> library build. And "BSD-3" above is the wrong granularity for attribution: bionic has no root `LICENSE`, only
> per-directory `MODULE_LICENSE_*` markers (§8's table, AD-10).
> **What the generated-proxy bucket actually is, from that same listing [1L]:** `android_api/` ships
> `libEGL`, `libGLESv1_CM`, `libGLESv2`, `libGLESv3`, `libandroid`, `libaaudio`, `libamidi`, `libbinder_ndk`,
> `libcamera2ndk`, `libc`, `libjnigraphics`, `libmediandk`, `libnativehelper`, `libnativewindow`,
> `libneuralnetworks`, `libOpenMAXAL`, `libOpenSLES`, `libvulkan`, `libwebviewchromium_plat_support`,
> `app_process`, `linker`, `vdso` — so C11's proxy scope should be cut against that list rather than against a
> guess, and the four absences above (`liblog`, `libz`, `libm`, `libc++`) are as much of the answer as the
> presences.
> The two original bucket errors stand as written: `libc++.so` in the NDK stub dirs is a
> 19-byte `INPUT(-lc++_shared)` linker script with no `.dynsym`, so it is a symbol authority for nothing and the
> C++ ABI surface has to come from `libc++_shared.so`'s own `.dynsym` or from AOSP **[1L]**; and `libz` and
> `libmediandk` are not generatable from an export list at all — `z_stream` is laid out by the guest, carries
> `zalloc`/`zfree`, and is *size-asserted* at runtime through `deflateInit_(strm, level, ZLIB_VERSION,
> (int)sizeof(z_stream))`, while `AMediaCodec_setAsyncNotifyCallback` takes a struct of four function pointers
> **by value** **[1L]**. Both move to `libandroid`'s mixed bucket: flat exports that generate, entry points that
> must be hand-modelled. The third correction is to the generator itself: filtering `--dyn-syms` on `FUNC`
> silently emits half a library (`libOpenSLES` 3 of 56, `libmediandk` 161 of 304), so `OBJECT` data symbols —
> the `SL_IID_*` interface IDs the vtable lookup takes as *input* — have to be emitted too.
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
vs active" through C17 (never silently substituted — `docs/research/08` §"silent-ignore interfaces
everywhere", which lists six such interfaces in that codebase and says of them "every one cost a debug
session"; the executor-substitution case is one instance of that class, not the whole of it).

### C13 `rb::audio` / `rb::input`
Same channel discipline. Audio's guest interface tables hand-modelled (§1). Input's `MotionEvent` is a **real 32-bit
mirror object**, not a pointer to the host C++ object (`docs/research/03`). Deferred after M2; interface fixed now so
M1 doesn't grow a second input path.

**Scope widened by [`docs/33`](33-guest-library-inventory.md)** (the stub sweep §1's audio finding was generalised
from, which that sweep then falsified — see §1's correction). The hand-modelled surface is not audio's alone, and the
list C13/C11/C12 must cover is in `docs/33` §4: the **vtable machinery built once** as a C11/C08 seam (six customers
besides audio — `libmediandk`'s by-value callback struct, `libandroid`'s `ANativeActivityCallbacks`, `libaaudio`,
`libcamera2ndk`, `libbinder_ndk`, `libvulkan` — and `docs/30` §Step 7b's fake `JNIEnv` is already that shape, so it is
a generalisation of something green rather than new machinery); **pointer-returning entry points** as a distinct
generated-code case (`dlsym`, `dlvsym`, `eglGetProcAddress`, `vk*GetInstanceProcAddr` — five symbols across four
libraries, all invisible in a FUNC-count sweep); **host→guest callbacks on threads RetroBug does not own**, which
extends `docs/research/03` §4.2 item 3's attach/real-time consequences to every one of them, not only to audio; and
**`OBJECT` data symbols**, since the 53 `SL_IID_*` are *inputs* to the interface lookup — item 1 cannot be tested
until the generator emits them.

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

**As built** (`30` §Step 15, 133 checks green on both devices). Three deltas from the sketch, each of which made an
invariant harder to violate rather than softer:
`Domain` is not a parameter — it is *in the key type*, so `NormalKey`/`IsolatedKey` carry their domain in the row
string they build and a wrong `Domain` argument cannot be written (a runtime parameter is a thing you can pass
wrong; a type is not). `DeriveDefaults(const ImportedApp&)` became `ApplyProbes(vector<ProbeResult>)`, because C05/C06
do not exist yet and the probe results are held by the launcher anyway — the rule the sketch is protecting ("probes,
never a package table") is unchanged and is enforced per name by `Schema::probe_supplied`. And `Value` is
`string | string[]` only: the four sketched kinds reduced to two once `tier` became a word from a closed set rather
than an int someone could pass 7 to, and a store that can hold an unvalidated number is a store with a hole in it.
**The rule that the closed lookup is a compile-time fact needed a `concept` to be true**: a `static_assert` inside the
template body only fires on instantiation, and a `requires`-expression does not instantiate — so the first draft's
`!CanLookup<std::string, Name>` assertion was false, i.e. the hole it was testing for was open. `template <ProfileKey K>`
fixes that, and every public method is now a key-taking forwarder to a private row-string half, because a `Get` that
is closed next to a public `Effective(row, name)` is a closed lookup with the door off its hinges.
`tier`'s schema row is `probe_supplied = false` and defaults to `normal`: **a probe that could choose T1 would be
the silent fallback AD-18 forbids**, and it would arrive through the most plausible channel in the system. That makes
`ApplyProbes`' refusal the mechanism enforcing the isolation-entry rule at the data layer, with the launcher's gesture
still the only way in.

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

**As built** (`30` §Step 10, 108 checks green on both devices). Three properties make the sentence above
enforceable rather than aspirational, and each has a test that fails if it is removed: `Record` **cannot
discard** a malformed echo (it stores a failure naming the field instead, which is the only version that is
not itself a silent-ignore interface); a `kFailed` entry does **not** satisfy a required field, so a field
cannot be closed out by its own complaint; and entering a stage is itself an append, which is what makes the
rewrite-in-full survive a hang. Required fields are a small cross-platform set plus whatever `Require()`
adds at run time — the Android-specific probes are demanded by the glue, not by `core/`, because `core/` may
not encode one platform's answer set as universal. `Field` is the current list of "must be reviewed" lines
that have a component to review them from; adding a component with a review requirement means adding a
field, and the name table is checked against the enum by test rather than by care.

**Still open in C17, and it is glue work not core work:** nothing populates a recorder yet, and no doc says
what must *not* appear in a report that a user can copy out of a UI — that decision belongs before M1's
export button, not after.

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
| Semantics vs external oracle | `qemu-arm` linux-user on CI, ≥ 1 000 instruction-vector programs, **0 mismatches**; ARMv7 barriers/LDREX/IT-block/NEON each explicitly covered. **A second, better oracle exists for part of this**: an AArch32-capable arm64 device executes the same vectors natively, is real silicon rather than someone's model of it, and carries no GPL adjacency at all (`tools/run_armv7_oracle.sh`, `30` §Step 5g). It cannot replace `qemu-arm` — CI has no phone attached, and it can only run what the hardware implements — so it supplements the differential set rather than joining it |
| Decoder coverage audit | `llvm-objdump` over corpus `.so`s ⇒ every mnemonic in `CoverageReport` or a triaged gap list. **This closes U-8 host-side** |
| `rb::mem` + address models | 4 KiB guest / 16 KiB host mixed-granule tests (the local image is a `ps16k` build — confirm `getconf PAGESIZE`, §1); `guest_base != 0` exercised under x86_64 Android so no identity-only code path survives |
| `rb::loader` | loads a real `armeabi-v7a` `.so` (the one built in §1) into a host process, applies relocations, resolves `libc.so` symbols to our own implementations. **First half done and hardware-verified** (`30` §Step 5e: 71 checks, expectations cross-checked against `llvm-readelf`); the second half — binding imports to RetroBug's own `libc` — belongs to C06 and C11 rather than the loader, and its test has only ever used a synthetic resolver |
| `fib(20) == 6765` | **the M0 exit test**: call the loaded `Java_..._stringFromJNI` through `rb::abi` with a fake `JNIEnv`, under Tier I and Tier J, assert identical result. Expected values are **measured, not assumed** — `tools/run_armv7_oracle.sh` runs the fixture's own `fib`/`wide_probe` as native Thumb-2 on an AArch32-capable device and reports `fib(0)=0 fib(1)=1 fib(2)=1 fib(20)=6765`, the short curve `fib(0..12) = 0 1 1 2 3 5 8 13 21 34 55 89 144`, `wide(0)=52 wide(2)=50 wide(-1)=55`, string `fib(20)=6765 wide=50`. Tier I already reproduces all of it (`30` §Step 6d); the curve is there so a recursion defect localises at n=4 rather than at n=20. **Both tiers now do** (`30` §Step 9): 15 call shapes × every field of `MachineState`, identical on two devices, plus the SMC half — a one-byte patch whose new value hardware also measured, answered correctly only after `Invalidate`. The row's `stringFromJNI` remains M1's, not this lane's: `fib`/`wide`/`it_probe`/`patch_probe` are the JNI-only exports the tiers drive, because a `jstring` return needs the bridge to be real rather than fake |
| Audio proxy strategy locked | one stub-lib sweep confirms §1's finding for every remaining guest library, so no interface-vtable API is ever assumed symbol-generatable. **Done — and it falsified §1's generalisation instead of confirming it** (`docs/33`, `30` §Step 14): 27 libraries enumerated from the NDK's AArch32 stub dirs at API 21→35 with per-library FUNC/OBJECT counts, classified against headers with file:line citations. Nine non-audio libraries break the generator approach the way audio does, `libc++.so` turns out not to be a stub at all, and a FUNC-filtered generator emits half-complete libraries. §1's correction note carries the outcome; the discriminator is now stated as a rule (`docs/33` §6), not as a category |

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
| R-2 | U-2 reservation failure ⇒ biased mode on Android | **TRIGGERED — measured, not hypothetical** (`30` §Step 8): no contiguous 4 GiB low window exists in an app process on either Android 16 device tested; largest obtainable is 1856 MiB, because ART and the app's own mappings occupy the low range. AD-6 amended to biased-by-default on Android as well as iOS. `AddressModel` was already parameterised and M0 already runs `guest_base != 0`, so no identity-only path survives — the mitigation was built before the risk landed, which is what it was for |
| R-3 | U-10 (DEX load inside `isolated_app`) fails ⇒ T1 = native-only | M2 gate; disclosure wording pre-drafted (`docs/10` §10.5); does not block M1 |
| R-4 | audio proxy surface larger than the 3-symbol stub suggests (§1) | re-estimate when C13 is scheduled; explicitly deferred out of M1/M2 |
| R-5 | Tier I speed unknown (U-9) | M0 measures IR-ops-per-guest-instruction on a real `.so`; **no performance claim in any user-facing text before M0 numbers exist** |
| R-6 | Dynarmic maintenance burden | `CoverageReport` + oracle green in CI before any core change merges |
| R-7 | Play policy on APK-running apps (U-12) | ships zero APKs, no downloader; sideload channel kept as fallback; unresolved but non-blocking |

---

## 9. M0 task breakdown (the immediate next work, all local)

Progress and the findings that change this plan are tracked in
[`30-m0-notes.md`](30-m0-notes.md). Status as of `30` §Step 20:

1. `core/` + `external/dynarmic` vendoring with `patches/` layout and `NOTICE.md`/`LICENSES/`/`REUSE.toml` in the
   same commit (AD-16: obligations start now). **Done** (`419fe9f`), with the one mechanical deviation recorded in
   `external/PROVENANCE.md`.
2. `rb_core_tests` target, cross-compiled for x86_64 Android and run in the local emulator via `adb shell`
   (no Windows host build is possible here — §1), plus a Linux CI job carrying the `qemu-arm` oracle.
   **Done except the CI job.** The tests were re-homed to run on arm64 **phones** as well, which turned out to be
   the better lane: three of this step's findings (Step 5c, 5e) only reproduce on 4 KiB hardware.
3. `rb::isa` wrapper + `CoverageReport`; corpus disassembly job → coverage gap list (**closes U-8's
   presence half**). **Done, and the halves turned out to be unrelated work.** The corpus measurement
   landed as `docs/32-isa-coverage.md`; the `rb::isa` wrapper is built and hardware-tested (`30` §Step 6a:
   101 checks, and it also caught a latent load-bias bug in C05); the mnemonic→handler query is
   `tools/mnemonic_handlers.sh` and answers it — **757 of 759 demanded mnemonics have an implemented
   translator, 100.00% of instructions** (`docs/32` §5.2). Building the two tools also corrected the
   survey's own totals (`325,541`/`759`, not `338,034`/`855`) and removed one claim that had never been
   measured; see `docs/32` §3.3–3.4 before reusing any figure older than that note.
   What U-8 still means: **semantic** correctness against a reference, which is item 4's differential lane.
4. `rb::mem` with both address models + mixed-granule tests. **Done** — 131 checks, 0 failures, executed on a
   OnePlus 12. The injected 16 KiB cases run on any host; the *native* 16 KiB ones still need the emulator.
5. `rb::loader` → load the §1 `.so`. **Done** — 71 checks, 0 failures on hardware, with `llvm-readelf` as the
   external oracle rather than the loader's own parser (`30` §Step 5e). Three bugs found and fixed in getting
   there; one of them was in `rb::mem` and had been silently corrupting every load.
6. `rb::exec` Tier I; then Tier J behind `RbCodeCache`. **Tier I done** (`30` §Step 6d): the interpreter
   computes the fixture's whole `fib` curve and all three `wide_probe` values exactly as the SM-X710
   hardware oracle reported them, 80 checks, 0 failures. Getting there corrected three beliefs about the
   IR — `Add32`/`Sub32`/shift operand 2 is a **carry-in**, not a set-flags bit; flags are read back through
   pseudo-operations naming their producer, so they cannot live in one mutable copy; and a shift's
   carry-in never reaches its result. **Tier J is built and green** (`30` §Step 9): `JitExecutor` over
   `Dynarmic::A32::Jit`, no fastmem and no page table so both tiers share one `GuestMemory`, and the
   field-by-field differential agrees with hardware on all 15 shapes on two devices. What is *still* open
   is the `RbCodeCache` half — Tier J runs on Dynarmic's own cache, so C04 (instance tagging, eviction,
   per-app limits) remains unbuilt and is now the last thing between this and a shippable JIT.
7. `rb::abi` `CallGuest` + fake `JNIEnv`; **M0 exit test: `fib(20) == 6765`, both tiers, identical state**.
   **Done** (`30` §Step 7a/7b/9). Bridge and `JNIEnv` were 7a/7b — guest calls through AAPCS32 match the
   hardware values and the fake table is built from indices measured twice over (NDK `offsetof`, and the
   fixture's own `ldr.w r2, [r2, #0x29c]`); building them found Tier I's IT-block defect (`7c`), now fixed
   against six measured values. The second half landed with Tier J: 65 checks comparing **every** field of
   `MachineState` across both tiers on 15 call shapes whose expected values are SM-X710 measurements, plus
   self-modifying-code invalidation proved against a one-byte patch whose new value hardware also produced.
   Asserted against silicon *and* against a second tier, which is what C02's invariant asked for.
   One dependency note from Tier I: those exports are JNI methods, so `n` arrives in **r2** behind `env`
   and `clazz` — a call bridge that puts the first C argument in r0 will get `fib(0)` and look correct.
   **Read "Done" with two caveats, both since repaired and both in the same direction:** this item's
   evidence was integer-only, and the component's float path was wrong in each direction separately — the
   results first (`30` §Step 24) and the arguments after (§Step 25, item 15 below). An integer suite green
   across a call bridge says nothing about the bridge's float placement, which is the general lesson and the
   reason item 15's rows are hardware-shaped rather than register-inspection only.
8. `docs/research/01` corrections (stale `MemoryProcessor` API; no interpreter upstream) and `docs/00` §2 note that
   the no-Gradle build path is verified.
9. `rb::diag` (C17) — the recorder the "echo requested vs active" invariants all hang off. **Core is done**
   (`30` §Step 10): 108 checks, and M1's "shows active tier + address model + code-cache backend" is now
   `MissingRequired().empty()` rather than a review note. Populating it is M1's launch-path work, listed
   there rather than assumed here.
10. `rb::syscall` (C07) — first slice **done** (`30` §Step 11): the SVC channel, identical in both tiers and
    proved with guest arithmetic rather than a log line; a monitor whose forwarding allowlist is empty by
    design and echoes itself through C17; and the loop that services a syscall inside a JNI call, with
    `exit_group` ending it and `arm_cacheflush` reaching the tier that owns the cache. Its numbers, errno
    values and return convention are measured on hardware per ABI (`tools/syscall_abi_probe.sh`), which
    corrected two things this plan had stated as fact — see C07's "As built" above. Signals, threads and
    forwarding are not.
11. `rb::codecache` (C04) — **policy half done** (`30` §Step 13): instance-tagged keys, per-instance limits
    with eviction that cannot cross an instance, W^X enforced through an injected allocator, `kInterpreted`
    as a working configuration rather than a failure, and the backend choice echoed through C17. The
    `AnonRwx`/`MemfdDualAlias` allocators themselves are M1's, next to the probe that selects them, and
    nothing is wired into Tier J yet — which still runs on Dynarmic's cache.
12. **FP on the interpreted path: scalar VFP done, NEON not.** `30` §Step 16 measured the boundary and
    §Step 17 moved it. 40 VFP/ASIMD rows carry values captured on the SM-X710; Tier J matches hardware on
    all 40, and Tier I — which refused all 35 it was first shown — now matches on the 35 scalar rows
    (conversions, `f64`/`f32` arithmetic with `vsqrt` and `ddiv`, `dcmp`+`vmrs`, `vldr`/`vstr`, the
    callee-saved `d8-d15` bank, an eight-double call through `d0`-`d7`, NaN comparison semantics) and
    refuses only ASIMD, at `VectorBroadcast64`.
    **What this changes:** `Kind::Interpreted` can run `double`-using guest code, so the iOS/M3 case and a
    FP-using app are no longer blocked; it still cannot run NEON, so an ASIMD-heavy guest on an
    `execmem`-denied host needs either the vector group implemented or a visible per-app gate in C17 — the
    gate is a day, the vector group is M1-sized, and `vmov`-only ASIMD (a `vmul.i32` in a codec's setup
    path) is the part most likely to be reached for first.
    **Closed by §Step 19, which is item 13 below.** The "still cannot run NEON" half no longer holds, and
    the part "most likely to be reached for" turned out not to be `vmul.i32` but `CBZ`.
    **Verified rather than assumed, and the way it was verified matters:** the FPSCR compare field both
    tiers produce (`0x8` less, `0x6` equal, `0x2` greater, `0x3` unordered, as NV:Z:C:GE) was first
    written from a recalled manual table, disagreed with hardware on three of four, and was documented as
    a suspected ARMv7-vs-ARMv8 divergence. It is not one: the measurement matches Arm's published VFP
    comparison table exactly, so the recalled table was the defect and the `[UNVERIFIED]` label on it is
    what caught that. Recorded in `docs/30` §Step 17 as the same lesson this project keeps being given —
    a table remembered from an architecture document is a claim, not a citation.
    **Related, and cheap:** the guest's cumulative FP flags are the *host's* on both tiers (native
    `FDIV`/`FMADD` raise them; Tier I folds the same `<cfenv>` state in), so an app that branches on
    FPSCR's exception bits inherits its host. `FPDoubleToSingle` honours round-to-nearest only.

13. **Integer ASIMD on the interpreted path: done, and it closed item 12.** `30` §Step 19. The differential
    is now **50 hardware-anchored rows with Tier I refusing none**, the lane group being one loop over a
    16-byte view at 8/16/32/64-bit lanes rather than four per-width copies, plus the width-independent
    logical group and lane get/set. Ten new rows, one per lane width and per logical operation, each a fold
    over all 16 result bytes so a wrong lane width carries into its neighbour and moves the number; the
    negative control (lane loop stepping in 16s, `vbic` as `~A & B`) failed all ten.
    **What this changes:** `Kind::Interpreted` is now a configuration that can run ordinary compiled ARMv7
    code, not only scalar code — integer NEON is what codecs, graphics blits and string routines compile to
    on `armeabi-v7a`, so the `execmem`-denied fallback and the iOS/M3 case both stop being "scalar only".
    Chasing the refusals also landed two things that were missing for less obvious reasons: `IsZero32`,
    which is Thumb-16 `CBZ`/`CBNZ` and whose absence meant Tier I could not execute a compare-and-branch on
    zero at all; and `LogicalShiftLeft64`/`Or64`, which an AArch32 guest reaches only through the ASIMD
    structure paths, with a **two**-operand shift form that publishes no shifter carry and zeroes at an
    amount ≥ 64 rather than wrapping like the 32-bit family.
    **Named, not hidden:** `VectorMultiply64` and `VectorAbs64` have no ARMv7 encoding and no hardware row,
    and `VectorBroadcast64` is reached only through `VMOV (immediate)` whose `.i64` form permits 0 and
    all-ones. The still-unimplemented vector groups are floating-point (`FPVectorAdd` and friends —
    `vadd.f32` refuses), the widening/narrowing and saturating forms, register shifts, and table lookups.
14. **`Term::Interpret` no longer aborts the process on an ARM64 host — Done** (`30` §Step 21). What
    follows is the finding as it was measured; the paragraph after it is the fix that closed it.
    It corrects §5 AD-2 consequence 3. Six ARM-state bodies (`arm_LDM_usr`, `arm_LDM_eret`, `arm_STM_usr`,
    `arm_CPS`, `arm_RFE`, `arm_SRS`) end in `InterpretThisInstruction()`; on the AArch64 back end that
    terminal is `ASSERT_FALSE("Interpret should never be emitted.")`, mcl's `ASSERT` is not `NDEBUG`-gated,
    and it was **measured on the SM-X710**: `--interpret-probe jit` dies with SIGABRT, rc=134, inside
    `jit->Run()` — no `StopReason`, no C17 record, nothing the caller can catch. Tier I now refuses the same
    seven encodings by name with their pc (`docs/32` §7's enumeration, re-verified here).
    **Severity is low and consequence is high**, and both halves belong in the record: zero occurrences in
    the 325,541-instruction corpus, because all six are privileged-mode instructions and the guest is EL0 —
    but the NDK assembler accepts every one, so hand-written assembly in an app's `.so` can ship it, and the
    failure mode is a crash rather than a diagnostic.
    **The fix has a seam and does not need a patch:** `JitExecutor` *is* a `Dynarmic::A32::UserCallbacks`,
    which derives from `TranslateCallbacks`, so `PreCodeReadHook` can veto those encodings before
    translation and end the block as a halt. Deliberately not done as part of §Step 20: the hook makes the
    callee responsible for the terminal, and mis-setting it converts a loud abort into a silent *skip* —
    which is worse. Needs its own test asserting Tier J returns a reason naming the pc.
    **The mechanism, found but not yet built:** `Dynarmic::A32::Jit::HaltExecution(HaltReason)` is
    embedder-facing and documented "Can be called at any time. Halts execution if called within a callback"
    (`interface/A32/a32.h:59`, with `ClearHalt` at :65), and `PreCodeReadHook` *is* such a callback. So the
    veto is four moves inside it, in this order: match the word at `pc` against the six decoder masks
    (`decoder/arm.inc:184,185,190,310,315,316` — ARM state only, since all six are `arm_*` bodies);
    `HaltExecution()`; `ir.SetTerm(Term::CheckHalt{Term::ReturnToDispatch{}})`, which is what makes the
    emitted code *observe* the halt rather than run past it; then return false. `CheckHalt` before
    `ReturnToDispatch` is the part that must not be omitted — returning to dispatch alone re-translates the
    same pc, which is the §Step 20 hang on the other tier. `Run` then maps the `HaltReason` to
    `StopReason::Unimplemented` carrying the pc, and the existing `--interpret-probe jit` becomes an
    assertion instead of a measurement.
    **Built, `30` §Step 21.** Exactly that, in `JitExecutor::PreCodeReadHook`, with the six patterns copied
    verbatim out of `decoder/arm.inc` into a `constexpr` conversion rather than transcribed to hex by hand.
    Same binary minus the hook, on the SM-X710: `rc=134` → `rc=0` with `reason=Unimplemented` naming the pc
    and the body, and **`steps == 0` asserted on both tiers**, which is what stops "refused" from quietly
    meaning "skipped it and carried on". Because the hook is in `translate_arm.cpp`, which every back end
    shares, the one veto also pre-empts the x86_64 emulator lane's variant of the bug. No over-fire: the
    lane carries an ordinary ARM `mov`+`bx lr` control that must still execute to 42, and every other suite
    in the 1,402-check engine lane now runs real ARMv7 code through the hook on every instruction, on both
    devices.
    **One limit on the new diagnostic, stated because it is not observable:** which of the six bodies an
    instruction "was" cannot be seen from the embedder side — all six end in the same terminal — so the
    name in the message is an inference from this table. dynarmic ranks matches by `count_ones` of the
    **mask**, and `arm_LDM_usr` and `arm_LDM_eret` both pin six bits, so they tie and declaration order
    decides; the first draft of the lane asserted the wrong one for an encoding where both matched, the new
    assertion caught it, and the row is now `ldm r0!, {r4, r5, pc}^` — an encoding where exactly one pattern
    matches, so the name is forced rather than inferred.

15. **`rb::abi`'s argument path was hard-float, and fixing it exposed a second, older defect — Done**
    (`30` §Step 25). §Step 24 corrected `ReturnValue` and left `PlaceArgs` allocating `F32`/`F64` from
    `s0..s15`/`d0..d7`, which is the `-mfloat-abi=hard` convention; no test called a JNI method that *takes*
    a floating parameter, so nothing could fail. Five new JNI entry points in the real probe library —
    one float, one double, a double that cannot find a pair, a second double that overflows the bank, and a
    double after an odd-sized argument block — each close a *different* placement rule, and each is checked
    against bits captured on the SM-X710 and then reproduced in node. Two things came out of doing it.
    The softfp rule was **measured** rather than recalled, from the probe's own disassembly
    (`vmov s4, r2`, `vmov d18, r2, r3`, `vldr d17, [sp]`, `vldr d17, [sp, #8]`), and with it the discovery
    that clang gives an internal-linkage function the VFP argument ABI under the *same* flags — so a claim
    about softfp is a claim about the boundary, not about a disassembly, and four doc sites plus two probe
    comments had been conflating them. And the argument **block's address** was wrong for every kind, not
    just floats: it was written 16 bytes above the callee's entry SP, where the intra-procedure region is
    *below* it, so a guest's `vldr [sp]` read somebody else's bytes. Six pre-existing unit rows covered
    spilled arguments and passed throughout, because the helper that read the spill area added the same 16
    — an assertion that mirrors the implementation it is checking, which is now the standing reason this
    component's spill rows also exist in hardware-shaped form. `rb_abi_call_tests` 95 → 105 checks 0
    failures, `rb_guest_apk_tests` 92 checks with the 18 float rows green on both tiers and only §Step 24's
    deferred Tier I libm defect left failing, and the 1,402-check suite still exits 0 on the tablet.
    **What this changes:** C09's `'F'`/`'D'` shorties are now correct in both directions, which is what
    lets a real app's `Canvas.rotate(float)`-class surface be called at all rather than entered with a
    garbage argument — and it is a precondition for #29, because bionic's own functions take stack
    arguments constantly (`printf` more than most).

16. **A real bionic `libc.so` loaded and called — Done as a measurement, and it produced the work list**
    (`30` §Step 26). New lane `rb_guest_libc_tests` loads the SM-X710's own
    `/apex/com.android.runtime/lib/bionic/libc.so` (1,267,328 bytes, ELF32 ARM DYN, Android 16) and calls
    into it, with the expected values from `tests/guest/oracle_libc.c` run natively on the same file on the
    same device. Two results, one of them a defect that was about to be structural.
    **The defect:** bionic's `errno` is a **coprocessor read** — `__errno()` starts
    `mrc p15, #0, r0, c13, c0, #3`, and 98 of the library's coprocessor accesses are that one register — while
    Dynarmic's AArch64 back end answers an unregistered coprocessor with a non-`NDEBUG` `ASSERT_FALSE`. So
    `atoi` on an overflowing string **terminated the emulator process**, and every guest that reports an
    error would have. Fixed on both tiers: TPIDRURW reads and writes go to `MachineState::tp` (proved by a
    row that seeds a TLS block and asks libc's own `__errno` for the slot address, expecting the `+0x29c` out
    of its disassembly — both tiers return it exactly), and any other coprocessor access now stops with
    `Unimplemented` naming the encoding and the address instead of returning a value the guest would
    dereference. §Step 21's lesson, generalised: **an unimplemented thing must be a reason, never an abort**
    — and both instances so far were found by loading code nobody wrote here.
    **The measurement:** the asserted rows that depend on nothing missing all match hardware — string
    scanning, `atoi`/`abs`/`strtol` with its `endptr` written back through a guest pointer, `strtok_r` across
    four calls on a mutating buffer, the thread pointer, and `qsort` calling a comparator in a *different*
    guest library, which is #27's ISA tag surviving a `blx` issued by emulated bionic. The lane is left
    **74 checks, 13 failures red on purpose**, because each failure is a named gap rather than a mystery: IFUNC resolution at load (9 rows; and the resolvers
    themselves already execute — calling `memcpy` reaches the resolver and it *returns* an implementation
    pointer in 4 steps, so only the load-time call is missing), four Tier I opcodes (`Mul64`,
    `ByteReverseWord`, `ConditionalSelect64`, `CountLeadingZeros32` — the list that stands between the
    interpreted tier and real system code), two unexplained Tier J `undefined instruction encoding` refusals
    inside `strtod` and `malloc`, three unfilled TLS slots whose silent-corruption semantics are an owner
    decision (§Step 26 item 4), and `DT_INIT_ARRAY` parsed but never run.
    **And the number that changes a plan:** the host-provided surface a real `libc.so` needs is **nine
    names** — `dlopen`, `dlsym`, `dlclose`, `dladdr`, `dlerror`, `dl_unwind_find_exidx`, `android_dlopen_ext`,
    `android_get_application_target_sdk_version`, `__loader_shared_globals` — not the ~1,300 it exports. C11's
    scope, measured off an image instead of estimated. Suite otherwise unchanged: 1,418 checks, 0 failures,
    plus dynarmic's 200,561 assertions.
    **Followed immediately by §Step 27**, which took the owner's decision on the TLS question — refuse the
    load, name every slot, `tolerate_unresolved_relocs` as the explicit opt-in — and in doing it found the
    constraint that sizes IFUNC: a real `libc.so` has **eight** slots needing a resolver call, three of the
    eleven resolvers return an implementation pointer in four steps with no syscall, five stop at the
    `/dev/cpu_variant:arm` probe, and **every one of the eight writes is refused**, because RELRO has made
    that memory read-only before the load returns. So resolution cannot be a post-load step: it belongs
    *inside* `LoadFromBytes`, between relocation and protection, which is where bionic puts it too
    (**[1A]**). That also corrected something §Step 26 had written down from a `readelf` column: the
    "nameless import" was really this loader asking the resolver to resolve `""`, because `R_ARM_IRELATIVE`
    and the `r_sym == 0` TLS kinds have no symbol and used to be looked up anyway.

Sequencing constraint the owner should know before M1: **M1's end-to-end acceptance step cannot be verified on this
machine at all** — it needs a physical 64-bit-only arm64 phone. Everything in §9 and most of M1's code can be written
and tested without one, so the device gates only the final launch assertion, not the work.
