# RetroBug on iOS — research and implementation plan

Status: plan, 2026-10-25. Owner decisions are settled (§2). Nothing here is implemented yet.

## 1. Context and what the research changed

RetroBug today runs 32-bit **ARM Android** games on a 64-bit Android phone. The phone supplies three
things the engine does not: **ART** (runs the game's Java/DEX), **the Android framework** (Activity,
SurfaceView, GLSurfaceView, input, audio, storage) and **GLES/EGL**. On Android the engine only has to run
the game's *native* code and forward JNI/GL into those host services.

An iPhone supplies none of the three. So an iOS port is not "recompile the engine": it is:

1. the same engine, interpreter-only (Apple forbids an executable code cache for third-party apps —
   `pthread_jit_write_protect_np` does not exist on iOS, and `mmap(PROT_EXEC)` on anonymous memory is
   silently downgraded to RW; `docs/research/04-ios-feasibility.md` §1 settles this), and
2. a **replacement for the Android Java framework and for EGL**, written in C++, plus
3. an iOS shell (SwiftUI) that imports an APK from Files and runs it in-process.

The repo already anticipated this: `core/README.md` fixes the layering (`core/` shared verbatim, no JNI/
Objective-C/GL types; `platform/android|ios` behind interfaces), `docs/ios-notes.txt` records the no-JIT
decision and the AOT idea, and `docs/research/04-ios-feasibility.md` lists the hard constraints. What does
**not** exist yet: `platform/`, `rb::vfs`, `rb::gfx`, `rb::audio/input`, any iOS shell, any Java replacement,
and a fast-enough interpreter tier.

Two engine facts drive the design:

* **The Java seam is already an interface.** `rb::abi::HostJava` (`core/include/rb/abi/host_java.h:24-115`)
  is 51 pure virtuals — a JNI implementation in disguise. Android's is `ArtJava` (real ART);
  `core/tests/fake_java.h` already proves the engine runs with **no VM at all**. A table-driven C++ "fake
  Java" is therefore a supported shape, not a hack.
* **The interpreter is the weak point, and not only because it is an interpreter.** Tier I calls
  `isa::TranslateOne` *inside* its `Run` loop and allocates an `IR::Block` on every block entry
  (`core/exec/interp.cpp:3116`) — there is **no decode cache**. That is the first thing to fix for iOS
  (decode once per pc+page-version, LRU-evict), before any measurement means anything.
  `ExecOpts::enable_jit` is declared and **never read**, and `GuestProcess::Create` always constructs
  `JitExecutor` (`core/runtime/guest_process.cpp:460-462`); on iOS the JIT object must **not exist**.
  Because Tier I caches nothing today, guest writes are also the missing self-modifying-code invalidator —
  a block cache needs a write observer that does not exist yet (`GuestMemory::Observer` fires on
  Map/Protect/Unmap only).

## 2. Owner decisions (fixed)

1. **Scope of v1:** the games that work on Android today — **9 native titles plus the 5 Unity titles that
   actually reach a screen** (MadTruck, TT2, CD2, Rolling Sky, ABE; Cars 3 boots but stays black). Not a
   general DEX/ART implementation in v1.
2. **Import in place:** the APK is chosen in Files and loaded in-process. No repack, no install, no rename/
   stub/shim/sign. The whole repack pipeline stays Android-only.
3. **UI:** SwiftUI on iOS; every compatibility/game rule lives once in shared C++ and is called from the
   Android Java UI through JNI and from Swift directly.
4. **Distribution:** signed IPA for sideload (AltStore/SideStore/personal team) and TestFlight for testers.
   No App Store attempt now (4.7 covers console/PC emulators, not Android apps; no precedent).
5. **No JIT, ever, on iOS.** Guest code is data; guest `mprotect(PROT_EXEC)` is faked in the memory model.

## 3. Target architecture

```
                    ┌────────────────────────────── shared C++ ──────────────────────────────┐
  iOS app (SwiftUI) │  rb::core   mem · isa · exec(Tier I[,J]) · loader · linker · syscall    │
  ├─ import/library ├────────────────────────────────────────────────────────────────────────┤
  ├─ settings/long  │  rb::vfs      paths, mounts, fd table (APK zip + container)              │
  │  press menu     │  rb::gfx      guest GLES → command stream → backend {EAGL, Metal}       │
  └─ calls C API    │  rb::audio    guest OpenSL/AAudio → Core Audio                          │
                    │  rb::input    UIKit touches → guest MotionEvent                         │
  Android app (Java)│  rb::rules    AXML read · ABI · NativeActivity · art · library · saves   │
  └─ JNI → same API │  rb::java     HostJava impl #2 (table-driven shim)  #1 = ART (Android)    │
                    └─────────────────────────────────────────────────────────────────────────┘
```

* **`core/apk` + `core/rules` (new, shared):** ports of the pure-Java parsers/rules — `AxmlEditor` read path,
  `ApkScan`, `DexScanner` (read), `NativeMethods`, `ElfRepair`, `ArtPicker`, `LibraryRules`, `ObbRules`,
  `Patch`/`Mod`, `SaveArchive`, `GameSettings`. One implementation of every decision; the Android repack
  (`ApkRepackager`, `StubElfWriter`, shim dex) stays Java and **consumes** these rules through JNI.
* **`platform/android` and `platform/ios`:** the only places with OS types. Android adapts to the current
  behaviour first (no behaviour change), iOS is new.
* **The Java replacement (iOS):** `IosJavaHost : rb::abi::HostJava` backed by a **class registry** —
  `UnityPlayer`, `UnityPlayerActivity`, `GLSurfaceView`, `Context`, `AssetManager`, `Bundle`, `Surface`,
  `AudioTrack`, `MediaPlayer`, `SharedPreferences`, `SystemClock`, etc., each a small C++ object with a
  method index table. `FindClass/GetMethodID/Call/NewObject` become table lookups; `RegisterNatives` keeps
  feeding `Inbound`. New games add *rows*, not code. The method set is discovered, not guessed: run each
  Android-working title with `ForwardingJni::SetCallTrace` on and generate the table from the trace
  (gate G2).
* **Graphics:** implement the 28 EGL entry points on **EAGL** (`CAEAGLLayer`) first — fastest proof, and iOS
  still ships OpenGL ES 1.1/2.0/3.0 (deprecated) — behind `rb::gfx`, then add a Metal backend fed by the
  same command stream. GLES1 stays on EAGL (no ES1-on-Metal translation in v1).
* **Build/versioning:** one version source (`git rev-list --count` + `RB_VERSION_NAME`), CMake presets for
  `android-arm64`, `macos-host` (tests), `ios-arm64` (static lib consumed by an Xcode project). CI matrix:
  macOS host tests + iOS simulator compile; existing Android device lanes unchanged.

## 4. What each iOS subsystem must do (short form)

| Subsystem | iOS mechanism | Engine change |
|---|---|---|
| Guest code | Tier I only; never PROT_EXEC | `guest_memory` gains a **software exec policy**: exec requests recorded in guest PTEs, host pages stay RW; self-modifying code invalidates decoded blocks (already flagged) |
| Guest "processes" | no fork/exec: threads + per-process page tables | none for v1 (all working titles are single-process); refuse multi-process games by name |
| Files | APK = zipped mount; libs extracted to Application Support; OBB via bookmark or copy; `isExcludedFromBackup` | `rb::vfs` resolves `/system`, `/data`, `/sdcard` into the container + zip |
| Graphics | EAGL (v1) → Metal (v2). **GLES1 must be verified on the first device run** (Apple documents ES 1.1/2.0/3.0 in `OpenGLES.framework`): if no ES1 context works, old ES1 titles need a GL1→GLES2 shim (gl4es-class) or are refused by name | GL/EGL thunk provider becomes `rb::gfx` backend; `dlopen` of libEGL/libGLES replaced by a static table |
| Audio | AudioUnit RemoteIO; never take the translator lock in the callback | guest OpenSL slot table → `rb::audio` sink |
| Input | UIKit touches → lock-free queue → guest MotionEvent | `rb::input` (also serves Android's current path) |
| Java | `IosJavaHost` class registry (§3); guest `Java_*` entry points are called **directly** (`abi.CallGuest(module->FindExport(...))`), so the ARM64 inbound trampolines are Android-only | none (`HostJava` already exists) |
| Lifecycle | quiesce at an instruction boundary; save-state on background; jetsam memory budget + `increased-memory-limit` | needs the pause/snapshot contract the research lists as day-one |

## 5. Deliberately out of scope for v1

DEX/ART emulation (so games with custom activities or Java SDKs — Among Us, Cars 3's online stack — are
expected to fail and are marked as such in the library), App Store submission, multi-process guest apps,
Metal-native GLES3.1+, controller/haptics polish, and the AOT translator from `docs/ios-notes.txt` §4
(interesting for speed, but it requires a per-game desktop step; keep as a later "Pro" path).

## 6. Performance gates (these decide the milestone plan)

* **G1a — cache first (blocking):** add the decode-once `IrCache` (pc + mode + page-version key, LRU byte
  budget) and the write observer that invalidates it. This is not an optimisation: today every block entry
  pays a decode + allocation, so no fps number taken now predicts iOS.
* **G1b — measure with the cache:** TT2, MadTruck and Rolling Sky on the Tab S9 with **CPU = Safe**
  (GameMenu → CPU, `runtime_boot.cpp:99`), reported next to their "Fast" numbers, plus a macOS arm64 run of
  a real title (M1's headless boot).
* **Pass bar:** ≥10 M guest instr/s and ≥20 fps on MadTruck's first scene → build v1 on cached Tier I.
* **Fail:** add **M1b — Tier I′**, the macro-op/threaded-dispatch engine (fixed-layout op stream,
  computed-goto dispatch, superinstruction table generated from a spec), 4–6 weeks; it also speeds up
  Android's "Safe" tier. Evidence: iSH, UTM SE/TCTI, PPSSPP (docs/research/04 §3.4).
* The milestones below assume the pass bar is met after caching; a miss inserts M1b before M3.

## 7. What is reused, what is new, and why

The iOS build is **the same core/ library as Android**, not a second engine. The reuse is the bulk of the
code; the genuinely new work is exactly what Android gets from the phone for free.

| Android piece | iOS | Work |
|---|---|---|
| `core/` engine: mem, isa, Tier I, ELF loader, linker, syscall monitor, abi/JNI forwarding, guest threads, signal frames, patch engine (~15k LOC) + 26 test suites | **reused verbatim**, same static lib | none — this is the dividend of the Android build |
| GL/EGL thunk table (~450 entries), GL forwarding | reused as data; EGL resolved through a new backend | EAGL adapter (~600 LOC), not a renderer |
| Game rules: AXML, ABI scan, NativeActivity, art pick, library/OBB rules, patches, saves (`AxmlEditor`, `ApkScan`, `LibraryRules`, `ArtPicker`, `ObbRules`, `Patch`, `Mod`, `SaveArchive`) | **ported once to C++** (`core/compat`) so both UIs call one implementation | mechanical port; the existing Java tests and a golden corpus are the oracle |
| Boot code (`runtime_boot.cpp`: ApkZip, LoadWithNeeded, BootOne) | **lifted** into `core/host/boot` (log/paths swapped) | move + de-Android-ise, not a rewrite |
| Compatibility corpus (14 working titles), settings, patch/mod formats, diagnostics | reused as the acceptance spec | none |
| **ART** (runs the game's Java) | **does not exist on iOS** | new: table-driven Java host — the single largest item |
| **Android framework** (Activity, GLSurfaceView, Surface, input, audio, storage) | **does not exist on iOS** | new: C++ service rows on top of UIKit/Core Audio/sandbox |
| **EGL** | **does not exist on iOS** | small adapter over EAGL |
| Linux host syscalls (memfd, eventfd, epoll, statx, getdents) | Darwin equivalents | small, mechanical |
| `PROT_EXEC` for guest pages | forbidden on iOS | small: exec policy in the memory host |

Two improvements land **in Android too**, not just iOS: the decode cache (Android's "Safe" CPU mode stops
re-decoding every block) and the software exec policy (a cleaner model than relying on host page flags).

### The one structural cost, and how it was trimmed

`core/` currently calls the OS directly in ~8 files (mmap flags, memfd, syscalls, gettid, clock, page size).
Without a thin platform seam, iOS would need a **forked copy of core** — every future engine fix applied
twice, i.e. exactly the compatibility drift we are trying to avoid. So the seam stays, but **only the eight
calls iOS actually hits**; the speculative `rb::gfx`/`rb::audio`/`rb::input` interfaces from my first draft
are deferred until a second backend exists.

## 8. Milestones (revised, lean)

| # | Goal | Exit criteria | Est. |
|---|---|---|---|
| **S0** | Same core compiles for iOS | **S0a done 2026-10-25** (`b970685`): `rb::platform` seam + POSIX/Darwin implementations for memory, image files, tid, guest-visible facts and clocks; macOS CI runs `rb_platform_tests` (26/26) and an iOS-arm64 job compiles `rb_platform rb_mem rb_loader`. **S0b done** (`a68f866`…`1be11d6`): open/AT flags, dirfd, stat/statfs, statx, poll/ppoll/pselect, pipe2/dup3/accept4, renameat2, entropy behind the seam; `check_layering.sh --strict` clean; iOS CI compiles `rb_platform rb_mem rb_loader rb_syscall`; macOS runs the platform test (26 checks). Directory listing on Darwin refuses by name until it gets a per-fd cursor | 3 w |
| **S1** | **A game on screen** | **S1a done 2026-10-25** (`b682ec0`): decode-once Tier I cache keyed by pc/mode/IT/FPSCR, cacheable only on non-writable pages, page-overlap flush eviction; interp lane 578→164 ms, gate 26/26, ABE title renders on Tier I. **S1b done 2026-10-25** (`0ee5f2f`, `60bd9ea`): `rb::host::Boot` + `rb::loader::ZipReader` are the shared boot (the Android runtime is a platform wrapper); Darwin CI builds the whole engine for iOS arm64 (`rb_host` pulls runtime/abi/inbound/exec/isa/dynarmic); device gate 28/28; Blocky Roads boots through the real launcher path (58 constructors, JNI_OnLoad returned). **Open**: EAGL/EGL adapter, touch, audio, `IosJavaHost` + recorded JNI trace, SwiftUI shell + unsigned-IPA CI job, and a measured Tier-I throughput on a spinning title (Rolling Sky) | 6–8 w |
| **S2** | Unity titles | UnityPlayer profile from the recorded JNI traces; **MadTruck or TT2 first scene ≥20 fps** | 6–8 w |
| **S3** | Parity | import + `core/compat` ported rules (label/icon from resources.arsc) + library/long-press menu/saves; lifecycle save-state; `docs/compatibility.md` iOS column | 4–5 w |
| **S4** | Ship | jetsam budget, `increased-memory-limit`, IPA/TestFlight pipeline, public-API symbol check, docs | 3–4 w |

**Total 22–28 engineer-weeks; first playable demo ≈ 9–11 weeks in.** No Tier I′ in the critical path: if the
cached interpreter misses the speed bar, the 2D native set still ships and Tier I′ (4–6 w) becomes a
follow-up that also speeds up Android.

## 9. File-level work list (key items)

**New**
* `platform/include/rb/platform/{memory_host,sys,clock,thread,log,storage,gfx,input,audio}.h` — the OS seam,
  header-only ABCs/data, no OS includes.
* `platform/linux/…` — the current Linux/Android implementations, moved verbatim (memory host, syscall
  backend, clock, thread id). `platform/android/{storage,log,gfx}.cpp`, `platform/stub/…` for tests.
* `platform/darwin/…` — eventfd→pipe+counter, epoll→kqueue, statx synthesis, `pthread_threadid_np`,
  `getentropy`; `platform/ios/` — EAGL EGL, Core Audio sink, UIKit input, container paths, SwiftUI glue
  (`RBApp.swift`, `RBGameView.swift`, `RBGL.mm`, `RBInput.swift`, `RBAudio.mm`, `RBStorage.swift`).
* `core/exec/ir_cache.{h,cpp}` — decode-once cache keyed by (pc, mode, page-version), LRU byte budget, plus
  the write observer in `core/mem` that invalidates it.
* `core/host/boot.{h,cpp}` — `ApkZip`, byte provider, scope boot, `LoadWithNeeded`, `BootOne` lifted from
  `android/jni/runtime_boot.cpp` with the Android log/paths removed.
* `core/host/java/` — `IosJavaHost` (class table, refs, frames, throwables, monitors, gap list) and
  `profiles/{android_base,unity_player}.cpp`.
* `core/compat/` + `core/include/rb/compat/capi.h` — the shared rules C API (ports of ApkScan, DexScanner,
  LibraryRules + `GlesTier`, ArtPicker, ObbRules, GameSettings, ElfRepair), called from Java via JNI and from
  Swift directly.
* `core/loader/{zip.cpp,axml.cpp,arsc.cpp,img_size.cpp}` — APK zip, binary manifest reader, `resources.arsc`
  label/icon lookup (`RES_TABLE` → package → type spec/type by config → entry → `TYPE_STRING` or `TYPE_REFERENCE`),
  PNG/JPEG/WebP size readers.
* `core/gfx/`, `core/vfs/`, `core/audio/`, `core/input/` — the interfaces `core/README.md` already promises.
* `core/runtime/{savestate,background}.cpp` — quiesce at a safe op, guest-clock freeze/compensation.
* `ios/RetroBug.xcodeproj` + `ios/RetroBug/…`; `CMakePresets.json` (`android-arm64`, `macos-host`,
  `ios-arm64`); `cmake/rb_version.cmake`; `.github/workflows/ios.yml`; `tools/check_layering.sh`;
  `tools/jni_gap_report.sh`; `tools/run_host_tests.sh`.
* Tests: `rb_platform_tests`, `rb_exec_policy_test`, `rb_ir_cache_test`, `rb_host_boot_test`,
  `rb_jni_host_test`, and the C++ side of the Java golden corpus.

**Changed**
* `core/mem/guest_memory.cpp` — exec policy, no `MAP_FIXED_NOREPLACE`, page size from the platform, write
  notifications for the cache.
* `core/loader/guest_elf.cpp` — `memfd_create` behind `TempFile`+`MapFileFixed`.
* `core/syscall/host_io.cpp`, `core/syscall/monitor.cpp` — syscall backend + clock/uname/affinity/prlimit/
  sysinfo/sched through `rb::platform`.
* `core/runtime/guest_thread.cpp` — `gettid` through the platform (Darwin `pthread_threadid_np`).
* `core/runtime/guest_process.cpp` — `allow_jit` option; JIT object not constructed on iOS; `rb_jit` is a
  separate target that iOS never links.
* `android/jni/thunks.cpp` — the GL name/signature table moves to `core/host/gl_table.cpp` (pure data),
  leaving resolution to `gfx.h`.
* `android/java/**` rule call sites → JNI into `core/compat`; `tools/build_launcher_apk.sh` consumes the
  shared version/icon source.

## 10. Risks

| Risk | Mitigation |
|---|---|
| Tier I has no decode cache, let alone speed for Unity 3D | G1a cache first, then G1b measure; M1b Tier I′ if the bar is missed; publish measured numbers, never promise fps |
| iOS GLES: ES1 availability is **unverified on current devices**, and most old native titles are ES1 | first device run settles it; if ES1 is gone, add a GL1→GLES2 shim (gl4es-class) or refuse those titles by name; `rb::gfx` split keeps Metal a later swap |
| Java surface grows per game | Table-driven shim + trace generator; unsupported classes are reported by name in the UI, never silent |
| Guests that JIT (Mono/il2cpp) write code and jump to it | no-exec policy + explicit "guest generated code" refusal with a named report; iOS list marks such titles |
| Memory/jetsam (500 MB APKs, 2–4 GB OBBs) | stream extraction, budgeted arenas, `increased-memory-limit`, backup exclusion |
| Suspension kills the guest | quiesce + save-state (M4); before that, keep sessions short and warn in the UI |
| Two UIs drifting | all decisions in `core/rules`; UI only renders and dispatches |
| Signing friction (7-day personal builds) | TestFlight for testers; document AltStore/SideStore; no in-app downloader |

## 11. Verification

* **Shared engine:** all **24 gating + 2 advisory** suites must stay green on Android *and* pass on macOS
  (host build) at every milestone — that is the "same engine" proof.
* **No-JIT build gate:** an iOS build with the JIT tier compiled out must build and pass the tier-diff and
  interp lanes (CI job), so nobody can reintroduce a codegen dependency.
* **Rules parity:** `DexStringsTest`-style host tests ported alongside each C++ rule; a fixture where the
  Java and C++ implementations must return identical verdicts for the same APK.
* **Device matrix:** iPad (M-series) and iPhone (A15+); per-title checklist from `docs/compatibility.md`,
  recording fps and the exact screen reached; iOS column added to that table.
* **Golden path demo:** import SMB Bounce APK → icon/label → play → touch works → audio plays → background/
  foreground → resume either at a safe point or at the menu.

## 12. Open questions (decide during M0/M1)

1. Which exact titles make the v1 list — pending G1 and the JNI trace (Cars 3/Among Us may drop out).
2. Apple developer identity/team for signing, and who is in the TestFlight group.
3. Whether Tier I′ also replaces the Android "Safe" tier, or ships iOS-only first.
4. OBB handling: copy vs bookmark-backed external file (depends on measured copy times for 2–4 GB).
