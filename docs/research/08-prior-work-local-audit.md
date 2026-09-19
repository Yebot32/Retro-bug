# 08 — Prior-work local audit (xaniteOG / XenDroid / Xenra on disk)

Audit date: 2026-09-18. Read-only scan of the developer's existing emulator
projects on this machine. Paths below are relative to
`C:/Users/yebot32/Downloads/Telegram Desktop/` unless absolute.

Scope of what exists on disk:
- `xaniteOG/` — "Xenra" (formerly Xanite): Xbox Original (xemu/QEMU fork in
  `xaniteOG/main/cpp/`) + Xbox 360 (Xenia fork in
  `xaniteOG/XenDroid/emulator-core/src/main/cpp/xenia/`), Android app
  (`xaniteOG/main/` Kotlin + `xaniteOG/xendroid-core/` module) plus a
  completed iOS port of the 360 core (`xaniteOG/XenDroid/ios/`) and an
  iOS plan for the OG core (`xaniteOG/docs/OG-IOS-PLAN.md`).
- `xaniteOG/XenDroid/` — the emulator-core submodule repo (Android + iOS
  build system, docs, CI). This is the richest engineering source.
- Xenra (Xbox 360 product) announcement txts and `.ips`/IPA artifacts — read
  the announcements; **no big binaries were unpacked**.

Negative results (stated so they are not re-hunted):
- **No Vita3K artifacts anywhere on disk.** Greps for `Vita3K`, `vk_ms`,
  `vita` across `xaniteOG/` and the desktop notes found nothing relevant.
  The Vulkan→Metal evidence here is: (a) Xenia's own Metal backend in the
  vendored fork, (b) a vendored MoltenVK 1.4.2 copy in
  `xaniteOG/XenDroid/emulator-core/src/main/cpp/xenia/third_party/MoltenVK/`,
  and (c) documented studies of DukeX (xemu-on-iOS/MoltenVK) and XeniOS
  (xenia-on-iOS) in `xaniteOG/XenDroid/SESSION_LOG.md:151-166`.
- `CROWNRUSH_UI_PROJECT.txt` (desktop) is **not an emulator project** — it is
  a Roblox game-UI redesign log. Nothing transfers to RetroBug input/overlay
  work; the actual on-screen-pad material is in the Xenra announcements,
  `XenDroid/ios/src/XenraAppDelegate.mm` (virtual pad) and
  `xaniteOG/main/java/Ali/Xanite/OnScreenController.kt`.
- `text.txt`, `logs67.txt`, `Vulkan_log.txt` (desktop root) were out of the
  permitted read scope and were not examined.

---

## Reusable patterns

### P1. Two CMake entry points so iOS can never regress Android
`XenDroid/CMakeLists.txt` exists *only* for iOS; Gradle's
`externalNativeBuild` points directly at
`XenDroid/emulator-core/src/main/cpp/CMakeLists.txt`, so the root file "is
inert for the APK" — "Keeping the two entry points separate is the whole
reason the iOS work cannot regress the Android build by accident"
(`XenDroid/CMakeLists.txt:4-8`, restated in `XenDroid/.github/workflows/xenra-ios.yml`
env header and `XenDroid/docs/ios-build.md:8-11`). Directly transferable:
RetroBug should have one Android (Gradle-driven) and one iOS (toolchain-file
driven) CMake root over a shared core, each unreachable from the other's
build.

### P2. Never edit the vendored core; wire everything from the outside
The recurring, explicitly-reasoned rule: keep the upstream tree
"byte-identical to upstream and keep[] merging cleanly" by doing third-party
wiring (`miniupnpc`, `libadrenotools`), extra sources (`upnp.cc`,
`net_utils.cc`) and iOS source *replacement* (`base/system_mac.cc` →
`ios/system_ios.mm`) from the outer CMakeLists via
`target_sources()`/`target_link_libraries()`/`set_target_properties(SOURCES)`
(`XenDroid/CMakeLists.txt:52-104`,
`XenDroid/emulator-core/src/main/cpp/CMakeLists.txt:103-148`). "A new file
never conflicts — only a changed one does"
(`.../cpp/CMakeLists.txt:128-132`). Android platform glue lives as `xe_*`
files *beside* the tree (`XenDroid/emulator-core/src/main/cpp/xe_*.cpp`,
`xendroid_emu.cpp`). For RetroBug: the ARMv7 core (decoder/JIT/memory model)
can be an untouched submodule; the Android runtime/API layer and the
per-platform layers live outside it.

### P3. Android build module split + version pinning
- Two Gradle modules: `:emulator-core` (CMake → `libe.so` +
  `libhardware_ProcessorInfo.so` + JNI-bound Java) and `:app`
  (Kotlin/Compose frontend); core "built once, consumed transitively"
  (`XenDroid/BUILD.md:7-13`). App repo consumes the core *submodule* via its
  own build file (`xendroid-core/build.gradle:13-27`) — three stated reasons:
  one-AGP-on-classpath, don't dirty the submodule pointer, own packaging
  choices.
- Pins: JDK 21, Android SDK platform 35 (app repo uses compileSdk/targetSdk
  36, `xaniteOG/build.gradle:65,84-85`), **NDK 29.0.14206865** pinned in
  `emulator-core/build.gradle:16` (app repo: 28.2.13676358,
  `xaniteOG/build.gradle:248`), **CMake 3.30.3** pinned via
  `cmake { version }` + `cmake.dir` in `local.properties`
  (`XenDroid/BUILD.md:22, 81-92`), Gradle 8.11.1 (XenDroid wrapper) / 9.0.0
  (app repo wrapper) with SHA-256-pinned distribution
  (`XenDroid/BUILD.md:23`, `xaniteOG/gradle/wrapper/gradle-wrapper.properties`).
- `arm64-v8a` **only** ABI (`XenDroid/BUILD.md:3`,
  `emulator-core/build.gradle:26`, `xaniteOG/build.gradle:111-112`).
- `minSdk 29` justified by native ELF TLS ("thread_local = register read, not
  an emutls call", `emulator-core/build.gradle:19-20`).
- 16 KB page support: `-Wl,-z,max-page-size=16384` in the native link
  (`.../cpp/CMakeLists.txt:18-19`) and `-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON`
  (`xaniteOG/build.gradle:~118`). RetroBug must do both (Android 15+ requirement).
- Symbol hiding: `-Wl,--exclude-libs,ALL` on the JNI `.so`; only JNI entry
  points exported (`.../cpp/CMakeLists.txt:27-28`).
- Sanitizer/build-mode variants driven by env vars without touching CMake:
  `XENDROID_HWASAN=true` adds `-fsanitize=hwaddress` via gradle cFlags
  (`emulator-core/build.gradle:29-51`); ccache via `USE_CCACHE` (`:52-55`).
- No explicit `ANDROID_STL` anywhere — they let the NDK/CMake defaults and
  `-stdlib=libc++` handle it (`.../cpp/CMakeLists.txt:14`). INFERRED: default
  `c++_static`; only documented convention is "libc++ + pinned NDK".
- `.gitattributes` normalizes line endings with the *specific* gradlew LF /
  gradlew.bat CRLF rule (`XenDroid/.gitattributes:1-10`).

### P4. Host-tool/target cross-compile for shader bytecode
Vulkan SPIR-V shaders are generated at Android configure time by host Python
+ `glslangValidator`/`spirv-opt`/`spirv-dis`, with a **configure-time
preflight that fails with an actionable message** naming the install path per
OS (`.../cpp/CMakeLists.txt:58-101`, `XenDroid/BUILD.md:103-121`). On iOS the
same problem (a host tool that must emit *device* bytecode) is solved with a
two-stage CI: build `xenia-shader-cc` natively, then cross-build with
`XE_FORCE_HOST_SHADER_CC` + `XE_HOST_SHADER_CC_PATH` +
`XE_METAL_TARGET_SDK=iphoneos` (`.github/workflows/xenra-ios.yml:5-13`,
`XenDroid/docs/ios-build.md:98-113`). The documented invariant: "the tool
must run on the BUILD machine while producing TARGET shaders, so the switch
has to be driven by the target platform" (`ios-build.md:403-409`). RetroBug's
Dex/ELF translation caches and any shader/asset tooling should copy this
shape.

### P5. Emulator process model on Android (maps to RetroBug sandbox)
Per-engine separate processes: `:xaniteog` (OG) and `:emu360` (360), each
`singleTask`, with the rationale "`libe.so` is single-shot per process, and
onDestroy hard-kills this one so a second launch always gets a clean address
space", plus a deliberately WIDE `configChanges` because "an Activity
recreate would re-enter the single-shot boot path — do NOT remove entries"
(`main/AndroidManifest.xml:122-152`). A "user is playing" flag (`setEmuForeground`)
protects the child process from the launcher's orphan-killer across
background/foreground cycles (`PHASE_2_SUMMARY.md:20`, fix 677bb8f2).
RetroBug's per-app isolated sandbox ≈ a second `:app_host` process per guest
APK; these are the exact lifecycle bugs to pre-empt. Note the *opposite*
default too: normal RetroBug execution is in-process, but Xenra shows the
cost — cross-process `SharedPreferences` reads forced them to keep
`commit()` instead of `apply()` (`SESSION_LOG.md:88`) and they built a
standalone two-activity probe APK to diagnose it
(`diagnostic-pref-test/java/com/xanite/prefprobe/WriterActivity.java`).

### P6. Frontend launch intent contract
Exported emulator activity with action `xendroid.intent.action.xendroid`,
game via `game_uri` / `AutoStartFile` (Dolphin convention) / data URI,
`content://` resolved to a real path with fd passthrough fallback, process
exits with the game ("back-to-back launches always cold-boot cleanly"),
ES-DE/Daijishō recipes (`XenDroid/docs/frontend-integration.md`). Reusable
for RetroBug's "launch a legacy APK from a file manager / launcher" story —
in RetroBug terms: the guest package name + activity replace the ROM path.

### P7. Diagnostics-first engineering discipline
Every finding is `file:line | what | why it matters | confidence(H/M/L)` with
FIXED/DEFERRED/OPEN status and commit ids, history never deleted
(`XenDroid/ISSUES.md:3-6`, `xaniteOG/ISSUES.md:1-8`). Deferred items must
carry a written reason (`PHASE_1_SUMMARY.md:62-68`). Perf work is audited
for regressions with an explicit "measured, not assumed" verdict document
(`PERF_REGRESSION_CHECK.md`, `SESSION_LOG.md:8-24`,
`xenra-0.4.0-announcement.txt` "Diagnostic instrumentation… INERT").
GPU driver bug-hunting uses a "Findings ledger — established (measured,
reproducible)" table plus bisection arms and labelled interim constraints
(`XenDroid/docs/a830-gmem-msaa-plan.md:14-30`). This is the workflow culture
to carry into RetroBug's compat work.

### P8. Debug-property A/B without rebuilds
`adb shell setprop debug.xanite.diag_frames 1` triggers per-draw frame
capture at runtime (`PHASE_1_SUMMARY.md:55-60`);
`debug.xanite.fg_temporal_pred` A/Bs an experiment live
(`xenra-0.4.0-announcement.txt` frame-gen section); `-PxenraConformance=ON`
builds a "behave like upstream" diagnostic APK that forces every fork
optimization onto its upstream fallback, "so the artifact can be attributed
to the fork or ruled out in one build" (`xaniteOG/build.gradle`
externalNativeBuild args, ~lines 115-130). For RetroBug: a `debug.retrobug.*`
property set + a conformance build that disables JIT (falls back to
interpreter) per component is directly modeled on these.

### P9. Cross-layer contracts must be single-sourced
Bug class: native preview writer used a directory no Kotlin scanner reads
(fix 48f8f3c3, `PHASE_1_SUMMARY.md:31`); the 360 core log and GPU report
lived outside the app's log dir so exports silently missed them
(`main/java/Ali/Xanite/DebugLog.kt:238-268`); two spellings of a crash
header must both stay recognized after a rename (`DebugLog.kt:64-68`).
RetroBug: file paths/ports/names shared between the native core and the
runtime layer need constant definitions on one side and derivation on the
other.

---

## Concrete assets (paths)

All under `C:/Users/yebot32/Downloads/Telegram Desktop/`.

| Path | What it is (transfer value) |
| --- | --- |
| `xaniteOG/XenDroid/cmake/ios.toolchain.cmake` | 114-line hand-written iOS arm64 device-only CMake toolchain, every decision argued inline. Candidate to copy near-verbatim for RetroBug iOS. |
| `xaniteOG/XenDroid/CMakeLists.txt` | iOS root entry: toolchain guard, `EXCLUDE_FROM_ALL`, source-swap for iOS-incompatible files, third-party wiring from outside. |
| `xaniteOG/XenDroid/ios/CMakeLists.txt` | Full app target: engine lib list, third-party statics list, framework list, `-fno-objc-arc`, C++20 pins. |
| `xaniteOG/XenDroid/ios/xenra.entitlements` | Correct iOS entitlements set + the longest, most accurate comment anywhere on why `com.apple.security.cs.allow-jit` is a red herring and `get-task-allow` is not ours to set. |
| `xaniteOG/XenDroid/ios/Info.plist` | `stikdebug` URL scheme handoff, `UIFileSharingEnabled`, document types (`:96-108`). |
| `xaniteOG/XenDroid/ios/src/ios_emu.{h,mm}` | Native driver: IosWindowedAppContext, ticket-based synchronous `CallInUIThread`, boot thread mirroring the Android `xendroid_emu.cpp` shape "so behaviour proven on Android transfers" (`ios_emu.mm:1-10`). |
| `xaniteOG/XenDroid/ios/src/XenraMetalView.{h,mm}` | Minimal CAMetalLayer-backed UIView (`layerClass` override, per-screen drawable sizing, `:14-19`). |
| `xaniteOG/XenDroid/ios/src/Xenra{AppDelegate,GameScanner,BladeView,BladeCell,Theme}.*` | UIKit dashboard replica of the Android UI: background library scan w/ cached metadata + LRU cover cache, theme palette mirrored byte-for-byte from `colors.xml`, virtual gamepad (`SESSION_LOG.md:93-117`). |
| `xaniteOG/XenDroid/ios/system_ios.mm` | iOS replacement for `base/system_mac.cc` (`system()`/osascript are `__API_UNAVAILABLE(ios)`). |
| `xaniteOG/XenDroid/docs/ios-build.md` | THE iOS JIT/entitlements/signing playbook: measured kills on iPadOS 26.4.2 + iOS 27 beta, csops gate design, Sideloadly/eSign/StikDebug expectations table, install-from-Windows flow, CI cost control. |
| `xaniteOG/XenDroid/.github/workflows/xenra-ios.yml` | Two-stage macOS CI: paths-filtered (10-minute cost discipline), ccache, pinned runner, unsigned-by-default IPA + optional signing secrets. |
| `xaniteOG/XenDroid/emulator-core/src/main/cpp/xenia/src/xenia/base/memory.{h,cc}` | JIT capability model (`JitCapability` enum, probe-by-executing-one-instruction), iOS 26+ TXM `Jit26PrepareRegion`/`Jit26Detach` brk-handshake with SIGTRAP guards, csops `QueryJitSignatureState`. Core artifact for RetroBug's iOS JIT story. |
| `xaniteOG/XenDroid/emulator-core/src/main/cpp/xenia/src/xenia/base/platform.h:26-64` | iOS/Apple define strategy: keep `XE_PLATFORM_MAC` on iOS, add narrower `XE_PLATFORM_IOS`; fork overrides gated by `XE_PLATFORM_xendroid` on top. |
| `xaniteOG/XenDroid/emulator-core/src/main/cpp/xenia/src/xenia/gpu/metal/` + `.../ui/metal/` | Complete Xenia Metal backend (command processor, primitive processor, texture/RT/shader caches, heap pool, MSL shader, MetalFX presenter) + `metal-cpp` glue. Proof the decode→guest-OS→Metal path exists on iOS. |
| `xaniteOG/XenDroid/emulator-core/src/main/cpp/xenia/third_party/MoltenVK/` | Vendored MoltenVK 1.4.2 (Docs: runtime user guide + configuration parameters — the tuning knobs doc is reusable reading). |
| `xaniteOG/XenDroid/emulator-core/src/main/cpp/xenia/src/xenia/ui/vulkan/vulkan_diagnostics.cc` | GPU capability probe: ordered candidate paths, report rewritten in full after *every* append (survives hangs/crashes mid-init), device/SoC property dump via `__system_property_get`, feature checklist with REQUIRED markers, `GpuDiagnosticStage()` → `>>> INITIALIZATION FAILED AT: <stage> <<<`. Direct blueprint for RetroBug's App-Revival diagnostic dump. |
| `xaniteOG/diagnostic-pref-test/` | 2-activity standalone probe APK (Writer/Reader) that answers "did prefs cross processes" — the smallest-possible-repro technique for field bugs. |
| `READ-ME-FIRST.txt` (desktop) | End-user instructions for the diagnostic build: install, grant All-files access, run a failing game, retrieve `Download/xenra-gpu-report.txt`; missing-report path is itself diagnostic. Template for RetroBug "send us your broken app" flow. |
| `xaniteOG/main/java/Ali/Xanite/DebugLog.kt` | Log system: async writer lane + synchronous fsync for E-level lines, FATAL line markers force immediate flush, `flushBlocking()` for crash handlers with 1.5 s budget, head+tail section capping, combined export bundling core log + GPU report + tombstone-enriched crash. |
| `xaniteOG/main/java/Ali/Xanite/CrashReportManager.kt` | Native signal handler capture, enriched crash text with debuggerd tombstone, phantom-crash suppression during shutdown (`xenra-0.4.0-announcement.txt`). |
| `xaniteOG/main/java/Ali/Xanite/PerGameSettingsManager.kt` | Per-game override store (SHA-256 game-id keyed prefs), runtime key projection, built-in compat profiles keyed by title with user-overrides-win + reasoning comments (`:88-204`). |
| `xaniteOG/main/java/Ali/Xanite/Xbox360Patches.kt` | Asset→storage patch-set seeding: stamp by set signature, never overwrite user files, restore deleted stays deleted, stamp only on complete copy (`:31-121`; bug+fix `PHASE_2_SUMMARY.md:21`). |
| `xaniteOG/main/java/Ali/Xanite/GpuDriverHelper.kt`, `Xbox360GpuDriver.kt` | Driver-ZIP importer (meta.json + zip-bomb limits), Mali/Adreno device gates, per-game driver override, explicit `system_selected` adoption stamp so a reset cannot be silently un-done by mtime logic (`GpuDriverHelper.kt:20-33`). |
| `xaniteOG/main/java/Ali/Xanite/OnScreenController.kt` | Overlay pad: single reusable View, scale clamp 0.5–2.0, haptics, face-button colors, menu passthrough hook. |
| `xaniteOG/XenDroid/BUILD.md`, `GETMORERAM_INTEGRATION.md`, `GAME_COMPAT.md`, `ISSUES.md`, `SESSION_LOG.md` | Process docs cited throughout. |
| `xaniteOG/docs/OG-IOS-PLAN.md` | Decision record: TCI-first no-JIT path, MoltenVK-over-CAMetalLayer renderer decision, per-step dependency ordering, "known risks (from code, not guessed)". |
| `xaniteOG/main/cpp/CMakeLists.txt:79,711-716,1082-1113,1210-1213` | `XEMU_TCG_INTERPRETER` option: interpreter-vs-JIT selected by include-path swap + regex source-exclusion, FATAL_ERROR if the vendored libffi is missing. The JIT/interpreter dual-backend build switch, reusable verbatim in spirit. |
| `xaniteOG/XenDroid/patches/xenia-canary/` | git submodule → `github.com/xenia-canary/game-patches` (`.gitmodules:133-135`): the community per-title patch DB, hundreds of `.patch.toml` files (sample: `415607E6 - Call of Duty 4 … .patch.toml` — `[[patch.be16]] address=0x8213537a value=0x0500` with RE comments, `is_enabled = false` default). |
| `xenra-0.3.5 / 0.3.6 / 0.4.0 announcement.txt` | Product-stated architecture/decisions: patch install validated by embedded title-id not filename; `allow_plugins` off by default; conformance default 15→14 with user-value-preserving migration; Exynos driver unlock via `dlopen_ext` namespace hook; JIT hint-cache lock fix ("measured: promoted=28800 vs dropped=179M" → `tier1_budget` per-game profile). |
| `xaniteOG/xenra-handoff-33.txt:12-21,99-196` | Session handoff that solved the year-long iOS JIT mystery: TXM universal protocol, iOS 27 kills unprepared executes even with get-task-allow, device/OS/install-method matrix. |
| `xenra-ios-*.ips`, `xenra-ios-*.ipa` | Crash reports + build artifacts from the iOS sessions (only the JSON headers were sampled; binaries not unpacked, per instructions). |

---

## iOS/JIT lessons

1. **iOS JIT = debuggable-process, not entitlement.** `MAP_JIT` is gated on
   Apple's private dynamic-codesigning entitlement (browser processes only);
   `com.apple.security.cs.allow-jit` is macOS Hardened Runtime and "does
   NOTHING on iOS — an active red herring"
   (`XenDroid/ios/xenra.entitlements:13-31`,
   `XenDroid/docs/ios-build.md:335-341`, `base/memory.cc:196-206`). What
   works: a *development*-style signature with `get-task-allow` + a debugger
   attached at least once per launch (`CS_DEBUGGED`)
   (`ios-build.md:214-243`).
2. **Wrong-killer trap.** Executing unpermitted code is an **uncatchable
   SIGKILL** ("CODESIGNING / Invalid Page", measured ~70 ms after launch
   under a distribution signature) — SIGBUS/SIGSEGV/SIGILL guards don't see
   it (`ios-build.md:270-305`). Defence: ask `csops(CS_OPS_STATUS)` *before*
   mapping anything executable and never attempt execute without
   `CS_DEBUGGED` (`memory.cc:215-233, 246-262`). On iOS 27 beta even
   get-task-allow doesn't save an unprepared execute (`memory.cc:246-253`,
   `xenra-handoff-33.txt:17-21`). **Treat "JIT works on current iOS" as
   per-release re-verification, not a settled property**
   (`ios-build.md:238-241`).
3. **iOS 26+ universal TXM/SPTM protocol**: `mov x16,#N; brk #0xf00d; ret`
   handshakes (`Jit26PrepareRegion` / `Jit26Detach`), each wrapped in a
   SIGTRAP guard so an unanswered brk reports instead of dying; every region
   that will ever hold code must be prepared before detach; the code cache
   prepares its whole reservation once; late-created regions need re-attach
   (`memory.h:140-167`, `memory.cc:140-187`, `ios-build.md:15-77`). Also
   mprotect *succeeding* while execute faults — "allocation succeeding is
   NOT the same as the page being executable" (`memory.h:94-100`).
4. **Capability probes must be refreshable.** Attach lands seconds after
   launch; the boot gate reading the cached probe answer blocked the entire
   JIT path until fixed (`SESSION_LOG.md:39-52` I-1;
   `memory.h:126-135`). Design rule: point-of-use `QueryJitStatusRefresh()`,
   cached variant only for HUDs.
5. **A real interpreter is the only honest no-JIT fallback.** Xenia has no
   PPC interpreter, so 360-on-iOS *cannot* run guest code without JIT —
   handled as a clean refusal + explanation + handoff button, documented as
   a physical limit (`XenDroid/ISSUES.md:17-22`, `SESSION_LOG.md:20-25`).
   The QEMU side *does* have TCI: selected at build time by include-path swap
   (`main/cpp/CMakeLists.txt:79,1110-1111`), needs libffi with
   W^X-safe trampoline-table closures on Darwin (`FFI_EXEC_TRAMPOLINE_TABLE`
   must match between `ffi.h` and `ffi.c` "or it is a silent ABI break"),
   and yields **zero executable mappings outside the binary** — immune to the
   iOS 27 kill class (`docs/OG-IOS-PLAN.md:9-34,108-126`). For RetroBug
   (arm64 host running arm32 guests): ship an interpreter as the guaranteed
   iOS path, JIT as the fast path behind exactly this protocol; the
   "TCI first, JIT second" ordering decision applies verbatim
   (`OG-IOS-PLAN.md:90-97`).
6. **Memory pressure kills are silent.** Jetsam exceed = instant kill with
   no catchable signal; a 360 session (512 MB guest RAM + JIT cache +
   staging) blows the default cap; `increased-memory-limit` +
   `extended-virtual-addressing` are *restricted* entitlements that are
   **silently inert** unless the App ID carries the capability (GetMoreRam /
   Xcode flipping `INCREASED_MEMORY_LIMIT` on Apple's developer services),
   and "a successful install is not evidence that either is active — measure
   at runtime" (`GETMORERAM_INTEGRATION.md:31-91`, `ios-build.md:343-352`).
   Candidate follow-up they propose and RetroBug should adopt: report the
   live jetsam ceiling via `task_info` on the diagnostics screen
   (`GETMORERAM_INTEGRATION.md:84-90`).
7. **Fixed guest address space on iOS**: use OS-chosen base
   (`MapViewsMac`-style) rather than a fixed window when extended VA
   addressing may be absent; the reservation "fails intermittently, which
   presents as a startup crash on some devices and not others"
   (`xenra.entitlements:52-64`).
8. **Simulator is worthless for JIT validation** ("no JIT story worth
   testing and no real GPU, the two things this app exists to measure" —
   `cmake/ios.toolchain.cmake:9-18`, `ios/CMakeLists.txt:140-143`).
9. **Signing is packaging, not build**: toolchain disables code signing so
   `cmake --build` works keychain-less; CI ships unsigned IPA and *never
   fails on missing signing secrets*; end users sign with their own identity
   (`ios.toolchain.cmake:105-113`, `ios-build.md:116-134, 198-211`).

---

## Vulkan→Metal lessons

1. **Two viable routes, both proven by this codebase:** (a) native Metal
   backend (Xenia's: `xenia-gpu-metal` + `xenia-ui-metal` linked into
   `xenra-ios` with `XE_ENABLE_VULKAN=FALSE` — the Vulkan path is simply
   absent, `ios/CMakeLists.txt:50-71`); or (b) Vulkan-over-**MoltenVK** with
   MoltenVK presenting through a native `CAMetalLayer`
   (`xemu_ios_set_external_metal_layer` → MoltenVK surface/swapchain on the
   external layer). The OG side *chose* (b) because "the fork's Vulkan
   renderer is the code Android ships and has years of correctness work on
   it… a Metal NV2A rewrite would throw that away"
   (`docs/OG-IOS-PLAN.md:60-68`, `SESSION_LOG.md:151-160`). For RetroBug's
   second target this is the key decision framework: bridge (MoltenVK) to
   reuse the Android Vulkan path, vs native Metal. The disk evidence favors
   "bridge first, native later" when the Vulkan path is battle-tested.
2. **MoltenVK specifics found**: vendored copy is **1.4.2**
   (`.../third_party/MoltenVK/Docs/Whats_New.md:14-16`); it is *not* wired
   into the shipping iOS build (that tree is Xenia's macOS Vulkan path) —
   the OG-iOS plan sources it in CI from the GitHub release tarball, "not
   committed to this repo (DukeX also requires it supplied locally)"
   (`OG-IOS-PLAN.md:66-69`). INFERRED: no extension-pruning code exists on
   disk; the pruning lessons are the *config*-level ones (see 4).
3. **MoltenVK does not provide EGL** — an offscreen EGL/GLES compositing
   layer has to be bypassed by letting the Vulkan swapchain own presentation
   directly (`OG-IOS-PLAN.md:70-76`). RetroBug: any Android
   `Surface`/EGL-shim layer needs a Metal-native presenter path.
4. **Shader/AIR targeting is the gotcha, not the API.** iOS rejects
   macOS-AIR metallibs; the shader host-tool invocations must read
   `XE_METAL_TARGET_SDK` and emit `-sdk iphoneos -std=ios-metal2.3
   -mios-version-min=16.0`, defaulting to historical macOS behavior
   byte-for-byte when unset (`docs/ios-build.md:98-113`; `:399-409`;
   `gpu/metal/CMakeLists.txt:41-44`, `ui/metal/CMakeLists.txt:31-33`). A
   known broken state was documented honestly: committed bytecode headers
   were generated with the macOS SDK, so the renderer "may refuse its
   metallibs on device" until the two-stage CI lands
   (`ios/CMakeLists.txt:13-18`). Also: `APPLE` being true on iOS silently
   armed the macOS MSL rules — same trap for any `#ifdef __APPLE__` in
   RetroBug (`ios-build.md:399-403`).
5. **Extension/feature fallbacks must be visible.** Android-side lesson that
   transfers: a per-game opt into `render_target_path_vulkan=fsi` on a
   driver lacking the feature "silent[ly] fallback" and wasted the debug
   session (`GAME_COMPAT.md:114-118`); Qualcomm blob reads zeros through
   dynamic-offset uniform descriptors so `vulkan_dynamic_constant_buffers`
   must stay off there (`GAME_COMPAT.md:144-147`). RetroBug on iOS should
   probe MoltenVK capabilities at init (via the P7/GpuProbe pattern) and
   report *requested vs active* for every feature.
6. **UI-layer Metal conventions**: metal-cpp with manual retain/release and
   ARC off target-wide to avoid mixed ownership
   (`ios/CMakeLists.txt:117-123`); custom `CAMetalLayer` view via
   `layerClass` + per-screen drawable sizing (`XenraMetalView.mm:14-19`);
   XeniOS's borrowed surface technique — explicit CALayer resize, *no*
   autoresizingMask on iOS, screen-scale backing (`SESSION_LOG.md:161-166`);
   MetalFX (iOS 16+) is what pins the deployment target
   (`cmake/ios.toolchain.cmake:43-55`).
7. **TBDR hardware envelope is a compat surface in itself.** The Adreno GMEM
   investigation (measure the 12 MB envelope, derive constraints from it,
   MSAA-into-GMEM fault at the RB/CCU intersection) and the
   `vulkan_mid_frame_submission_draws` idle-bubble fix are
   methodology-transferable to Apple GPU tile memory behaviour once RetroBug
   renders guest GL/Vulkan on Metal (`XenDroid/docs/a830-gmem-msaa-plan.md`,
   `GAME_COMPAT.md:136-141`).

---

## Compat/patch data lessons (→ RetroBug "App Revival")

Three layered mechanisms, all leaving the guest binary on disk untouched:

1. **Per-app config overlays** (`<root>/config/<ID>.config.toml`, loaded
   *after* global at title launch, overriding per key
   `category.name`; `XenDroid/GAME_COMPAT.md:8-17`). Hard lessons:
   - A wrong section header "is silently ignored" — lookup is
     `category.name`, so `[GPU]` vs `[Vulkan]` matters and the symptom is the
     global value standing (`GAME_COMPAT.md:16-18, 76-80`). RetroBug: an
     overlay system must *echo effective values at load* and warn on
     unmatched keys.
   - **Load-order boundary**: cvars consumed during subsystem setup
     (`mount_*` devices) cannot be overridden by something that loads at
     title launch ("read too late", `GAME_COMPAT.md:127-131`); conversely a
     GPU-init cvar was *made* per-game-applicable by reading it live at the
     point of use (`GAME_COMPAT.md:86-88`). RetroBug maps exactly: an App
     Revival flag only works where the runtime still asks — design the read
     sites for late binding.
   - Flags stay per-app when they cost performance globally
     (`clear_memory_page_state`, `depth_float24_convert_in_pixel_shader`,
     `vulkan_depth_unorm24`) with the measured device/title rationale dated
     and logged in the compat doc itself (`GAME_COMPAT.md:29-108`).
2. **In-memory code patches** (`<root>/patches/<TITLEID>.patch.toml`,
   `xenia-patcher` applies at load; DB = `XenDroid/patches/xenia-canary/`
   submodule of xenia-canary/game-patches):
   - Format: title name/id, **module hash gate** (`hash = "4145F96D2DEE2AB5"`
     must match the loaded module), `is_enabled = false` by default, typed
     address+value writes with the reverse-engineering rationale as comments
     (sample CoD4 file; `GAME_COMPAT.md:90-106`).
   - Load requirements are a compatibility trap: filename regex
     `^[A-Fa-f0-9]{8}.*\.patch\.toml$` — a bare `.patch` is "silently
     rejected… 0 titles loaded"; verify via log lines `PatchDB: Loaded
     patches for 1 titles` + `Patcher: Applying patch for: …`
     (`GAME_COMPAT.md:100-106`).
   - Fable II's null-head race patch is the model case: a guest bug masked
     on real hardware by coarser scheduling/stronger store ordering, exposed
     by true 8-core concurrency, fixed by changing **one guard load** to the
     correct field so the engine's existing retry path runs
     (`GAME_COMPAT.md:93-101`) — precisely RetroBug's "app raced on modern
     ARM cores" class.
   - Policy: patches ship *off*; "Enabling them wholesale would be the app
     deciding to modify people's games for them", many are mutually
     exclusive alternatives (`Xbox360Patches.kt:17-23`); install validates
     the title-id **inside** the file, not the filename
     (`xenra-0.3.5-announcement.txt:73-76`).
   - Seeding bundled sets: stamp by set signature, never overwrite user
     files, a user-deleted patch stays deleted, and only claim the stamp on
     a complete copy (partial-failure retry fix cb689895,
     `PHASE_2_SUMMARY.md:21`, `Xbox360Patches.kt:31-121`).
3. **Built-in per-app profiles in code** as the last-resort tier below user
   overrides: `builtInCompatibilityProfile()` keyed on title with the
   measurement and the reopen-trigger written into the comment (Forza
   descriptor race history, `tier1_budget=512` justified by
   "promoted=28800 vs dropped=179M" traces; `PerGameSettingsManager.kt:143-204`,
   `xenra-0.4.0-announcement.txt`). Precedence: built-in < user per-app <
   (global default). And: **the UI must show the effective value** — Forza
   displayed "Vulkan" while a profile ran OpenGL, which is why
   `builtInProfileFor()` exists (`:124-131`).
4. Priority discipline: "Each was general — no Fable-specific hacks shipped"
   (`GAME_COMPAT.md:62-64`) — core fixes first, compat flags only when the
   fix is device- or title-specific, with the reverse-engineering rationale
   recorded beside the flag.
5. Per-app GPU *driver* override reusing the same store
   (`setting_gpu_driver` per-game, system-driver fallback with logging,
   Exynos/Mali unlock, `Xbox360GpuDriver.kt:19-31` +
   `xenra-0.4.0-announcement.txt:182-199`).
   RetroBug analogue: per-app backend flags (interpreter vs JIT, GL
   translation path, memory layout mode).
6. Mod/plugin loading requires a runtime-computed hash of guest code — a
   manifest "cannot be computed outside a running game," so the app must not
   fabricate one (`xenra-0.3.5-announcement.txt:77-83`). Relevant to
   RetroBug: revival patches keyed by APK/DEX digest should be verifiable
   offline (APK hash) rather than at load — Xenra's choice of module-hash
   gating costs offline installability (their patch-install UI still matches
   by title id, `:73-76`).

Input/overlay conventions (secondary): on-screen pad as one custom View with
configurable scale clamp + haptics + menu hook (`OnScreenController.kt:20-50`);
virtual pad shared center-normalized stick handler after the first-touch
anchor bug (`XenDroid/ISSUES.md:54-58`); physical-pad registry with explicit
bound-device tracking so player-1 falls back to the on-screen pad
(`PHASE_2_SUMMARY.md:23`); SDL `gamecontrollerdb.txt` shipped as asset
(`build/intermediates/.../gamecontrollerdb.txt`); guest-prompt pump on the UI
thread with refuse-on-throw (`PHASE_2_SUMMARY.md:22`).

---

## Anti-patterns / mistakes to avoid

1. **Guards that can never pass**: testing `CMAKE_SYSTEM_NAME` before
   `project()` in the root CMakeLists — "exactly the mistake this replaced"
   (`XenDroid/CMakeLists.txt:15-27`).
2. **Globs + platform suffixes**: `xe_platform_sources()` globbing `*_mac.*`
   onto iOS dragged in a file that is a *compile error* (`system_mac.cc`,
   `XenDroid/CMakeLists.txt:55-64`); `XeniaHelpers.cmake` missing `_ios` in
   `XE_PLATFORM_SUFFIXES` means `surface_ios.h` is "being picked up by the
   Android build today… a live latent bug" (`docs/ios-build.md:391-396`).
   Prefer explicit source lists — the Android build "lists every source
   explicitly and globs nothing" (`ios-build.md:10-11`) — and put an
   existence-assert before any source-swap (`CMakeLists.txt:74-79`).
3. **Probes engineered for the wrong killer** (SIGSEGV/SIGBUS/SIGILL guards
   around a failure delivered as uncatchable SIGKILL) — never attempt
   execution to *test* permissions (`docs/ios-build.md:282-305`).
4. **Caching a runtime-varying capability** (JIT attach lands after launch;
   the cached snapshot poisoned the whole boot path) — refresh at
   point-of-use (`XenDroid/ISSUES.md:81-88`, `SESSION_LOG.md:39-52`).
5. **Silent-ignore interfaces everywhere**: wrong TOML section
   (`GAME_COMPAT.md:16-18`), silently inert restricted entitlements
   (`GETMORERAM_INTEGRATION.md:66-77`), patch DB filename rules
   (`GAME_COMPAT.md:100-106`), silent driver/feature fallback
   (`GAME_COMPAT.md:114-118`), the driver-adoption stamp whose evidence file
   got deleted by the reset itself (`GpuDriverHelper.kt:14-25`), and
   `buildTypes.debug.packaging` blocks "silently ignored by AGP 8"
   (`xaniteOG/build.gradle:262-283`). Every one cost a debug session.
   RetroBug's rule should be: nothing is applied without an "active vs
   requested" echo (their proposed `task_info` jetsam readout is the model).
6. **Dead or misplaced cross-layer paths**: snapshots written to a dir no
   scanner reads (`PHASE_1_SUMMARY.md:31`), 360 logs invisible to the export
   bundler (`DebugLog.kt:238-245`), stale crash-header spelling after rename
   (`DebugLog.kt:64-68`), two directories split by a rename meaning exported
   "debug log" never contained the crash log (`DebugLog.kt:24-29`).
7. **Unconditional `add_subdirectory(third_party)`** configures SDL3,
   wxWidgets, MoltenVK, glslang, FFmpeg even though "building only
   `xenra-ios` keeps them out of the *build*, but they still get
   *configured*" — called out as the biggest single risk, with the escape
   hatch documented (own top-level project pulling only the minimal deps)
   (`docs/ios-build.md:418-430`).
8. **False bookkeeping**: three "FIXED" citations in ISSUES.md pointed at
   the wrong commits; the fix pass had to re-open already-"closed" bugs
   (`SESSION_LOG.md:27-30`). Discipline: cite the commit that *diffed*, not
   the one that was in the window.
9. **Main-thread I/O everywhere until audited**: 10 separate UI defects were
   file/log/proc reads on the UI thread (E1-E10, `xaniteOG/ISSUES.md` Hunter
   batch E; `xenra-0.4.0-announcement.txt` responsiveness list). RetroBug's
   scanner/settings/diagnostics screens need the same discipline from day 0.
10. **Commit() vs apply() cross-process races** — the pref-probe APK and the
    deliberate `commit()` retention (`SESSION_LOG.md:88`,
    `diagnostic-pref-test/`): multi-process shared prefs are not a config
    bus; use a file/DB or explicit intent handoff.
11. **Android-isms that bite on modern devices**: orientation requests ignored
    `sw >= 600dp` on targetSdk 36 (needed the
    `PROPERTY_COMPAT_ALLOW_RESTRICTED_RESIZABILITY` opt-out, removed at API
    37 — a debt clock is documented, `main/AndroidManifest.xml:103-121`);
    JNI `NewStringUTF` over guest bytes aborts the JVM on invalid Modified
    UTF-8 (`PHASE_1_SUMMARY.md:39`); guest-triggerable `assert()` must be
    "ignore-and-continue like hardware", not abort (`PHASE_1_SUMMARY.md:40-42`
    rows for Super I/O / NV2A user / pfifo).
12. **Unverified claims in docs**: they flag their own "not authoritatively
    confirmed" Sideloadly/`get-task-allow` answer and say "do not design
    around it" (`docs/ios-build.md:243-257`). Keep that convention; mark
    inference, date the device/OS of every measurement.
13. **CI cost blindness**: macOS runners bill 10× — mitigated with
    paths-filtered triggers, no matrix, single arch, no LTO, ccache, and a
    named degradation plan ("delete the push trigger"), plus *pinned* runner
    images because `macos-latest` moved Xcode "without notice"
    (`xenra-ios.yml:22-58`, `:78-80`).
