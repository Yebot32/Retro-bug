# RetroBug — handoff (2026-10-15) — START HERE

You are continuing **RetroBug**: an emulator that runs legacy 32-bit ARM (armeabi / armeabi-v7a) Android
game libraries on 64-bit ARM phones by translating the ARM code (vendored dynarmic JIT = "Tier J", a
hand-written IR interpreter = "Tier I"). Engine in `core/`, on-device launcher app in `android/`
(`android/jni/launcher.cpp` + `android/jni/gl_forward.cpp` + `android/java/.../LauncherActivity.java`).
Everything below this section is older, detailed session history — read it only when you need depth.

## M2 — multi-entry core (2026-10-16, ACCEPTED on the tablet and the OnePlus) — newest state, read first

Branch `m2-multi-entry-core` (not merged, not pushed). Plan: `docs/superpowers/plans/2026-10-15-m2-multi-entry-core.md`.
Ledger (git-ignored): `.superpowers/sdd/2026-10-15-m2-multi-entry-core/progress.md`. Newest notes: `NEXT-AI-HANDOFF.txt`.

**What M2 delivers.**
- `GuestProcess` (`core/runtime/guest_process.cpp`): one persistent guest process per app — 4 GiB biased window,
  bionic's real main thread, every libc layout fact asserted against the image. The in-app runtime
  (`android/jni/runtime_boot.cpp`) boots it once per process ("scope: booted once"), links DT_NEEDED siblings from
  `assets/rb32`, streams zip entries, runs constructors + guest `JNI_OnLoad` only (boot-only, no per-game profile).
- `GuestThread` + Global Guest Lock (FIFO ticket lock, 200k-step slices; released in host slots and blocking syscalls).
- Guest pthreads on real host threads: `clone`, `futex` (`FutexTable`), `exit`/`exit_group`, sleeps; native pthread
  oracle `tests/corpus/oracle-pthread.txt` (`tools/run_pthread_oracle.sh`).
- Adopted ART threads: a Java thread calling into the guest gets a synthesized guest pthread made by libc's own init
  functions; at host-thread exit its TLS-key destructors run to completion, it is unlinked, its mapping freed (kept,
  and named, when it cannot be unlinked).

**Deviations closed:** M1 (1b) isolated boot (siblings now linked), (4) profile-in-boot (boot-only mode), M1 (2) the
64 MiB per-run window (now a process-lifetime 4 GiB window).

**Evidence (tablet R52Y106BECZ, 2026-10-16, after the final-review fixes below).**
- Launcher `RB_WAIT=100 bash tools/run_launcher_on_device.sh R52Y106BECZ gumball`: "RESULT: ran to the end — 7068
  frame(s) on tier J, final scene GumballMainMenu, no fault, drawn on screen".
- After the TLS fix (launcher rebuilt): ABGo bootcheck again 258 constructors, 0x10006, no fault, with the `tls:` line;
  Gumball launcher run (2026-10-16 16:10): "RESULT: ran to the end — 7063 frame(s) on tier J, final scene
  GumballMainMenu, no fault, drawn on screen". (The attempt before it stopped at 3827 frames on `eglSwapBuffers`
  EGL_BAD_SURFACE 0x300d — the display surface went away right after the human unlocked the tablet; not an
  emulation fault, did not repeat.)
- `am start -S -n com.retrobug.launcher/.LauncherActivity -e bootcheck angry-birds-go`: scope booted once, 258
  constructors on Tier J, `JNI_OnLoad` 0x10006, "RESULT: startup finished with no fault"; 92 `Java_*` wait for M3.
- `bash tools/run_java_tests.sh`: 141 + 54 checks, 0 failures.
- Engine lanes `RB_SERIAL=R52Y106BECZ bash tools/run_engine_tests.sh arm64-v8a`: exit 0, all 19 gating lanes 0 failures
  (guest_thread 445, guest_process 159, abi_call 127, monitor 172, diff 998, dynarmic 200,561) and both advisory lanes
  green (guest_libc 147, guest_apk 1376) — after the TLS fix below.
- Earlier (2026-10-15, OnePlus 75ff8cd3): all 19 gating lanes 0 failures on the M2 code; new launcher installed.

**Final-review fixes (2026-10-16).**
- An adopted thread that exits while another live host thread holds its guest thread no longer frees that
  `GuestThread` (use-after-free): it moves to `stranded_` and lives until the process goes (also after a group exit).
  Regression check in `unlinkable_exit_keeps_the_mapping` (the holder still sees its thread after the exit).
- `CallGuestToCompletion`'s re-entry across turn budgets now has a test (`abi_call_test`
  `a_call_to_completion_is_re_entered_across_turns`: fib(12) with a 1000-step turn).
- guest_libc's `librbifunclibc` row failed on 64-bit-only phones (no `/system/lib/ld-android.so`): the script now pushes
  `build/device-libs/{libdl,ld-android}.so` (pulled from the tablet) as `/data/local/tmp/rb_device_*` and the row prefers them.

**Fixed: intermittent boot fault — libc's own TLS was never registered (2026-10-16).** About 1 run in 5–10 of
`rb_guest_thread_tests`, a fresh `GuestProcess` faulted in `__libc_preinit` (`__set_errno_internal` storing to 0x1387):
TLS_SLOT_THREAD_ID (TP+4) held 0x10eb instead of the thread. A store watch caught the writer: **GWP-ASan**
(`GuardedPoolAllocator::init`, `gwp_asan_malloc`), which libc turns on in a random few processes, keeps its
thread-locals in libc's own `PT_TLS` block through an initial-exec `R_ARM_TLS_TPOFF32` slot that the loader left
unapplied (0), so they sat on bionic's TLS slots. Fix: the loader's new `LoadOptions::resolve_tls` hook answers a
module's own TLS relocations; GuestProcess places libc's block where the device's linker does (static-TLS offset
0xb78 = TP+0xb6c, static TLS 0xba8 — the probe's measured values), registers it as static TLS module 1 in the shared
globals (module table, counts, generation 1; six new [image] facts) and copies its init image with libc's own
`__init_static_tls` (main and adopted threads; `pthread_create` already does). Runtime log: `tls: libc.so is static
TLS module 1 ...`. Test: `guest_process_test` `libc_thread_locals_live_in_libcs_own_tls_block` (libc's
`__tls_get_addr({1, 8})` = block + 8; TP+4 intact after constructors) plus layout rows. Evidence: 30/30 clean
`rb_guest_thread_tests` runs with a store watch on TP+4 (before the fix: a failure within 3–7 runs).

**OnePlus 75ff8cd3 acceptance (2026-10-16, human confirmed every install).** Plan Task 6 steps 1–3:
- New launcher installed (16:12:46). Engine lanes `RB_SERIAL=75ff8cd3 bash tools/run_engine_tests.sh arm64-v8a`:
  exit 0, all 19 gating lanes 0 failures and both advisory lanes green — guest_libc 147/0 (its `ld-android.so` row
  failed here before the libc-sibling push), guest_apk 1376/0.
- **M1 (a) closed: a re-repack installs as an update.** Angry Birds Go `-e repack base`: `install: status=0 (SUCCESS)`,
  firstInstallTime still 2026-10-15 14:38:58, lastUpdateTime 2026-10-16 16:18:34, arm64-v8a. Gumball
  `-e repack gumball`: status=0, firstInstallTime still 2026-10-15 14:40:19, lastUpdateTime 16:19:14.
- Angry Birds Go by its icon, pid 24394: `tls:` module line, "scope: booted once", 258 constructors on Tier J
  (16:18:45.36 → 48.36, ~3 s), `JNI_OnLoad` 0x10006, "RESULT: startup finished with no fault", no profile lines; then
  `UnsatisfiedLinkError` at `AngryBirdsGoActivity.isDeviceAmazon()` (`Java_com_rovio_angrybirdsgo_AngryBirdsGoActivity_isDeviceAmazon`).
- Gumball by its icon, pid 26352: scope booted once, 24 constructors on Tier J (~0.85 s), no `JNI_OnLoad` (12 `Java_*`
  for its own Java), "RESULT: startup finished with no fault"; then on its GL thread `UnsatisfiedLinkError` at
  `JNIGLSurface.jniClearTextures()` from `onSurfaceCreated` — this run the GL thread got there before the known
  targetSdk billing crash (implicit `bindService`), which is still there for the shims to fix.
- Remaining before merge: a final whole-branch review, then the human's OK to merge to `main`.

**Next after M2:** targetSdk compatibility shims (Gumball's implicit `bindService`) → M3 JNI bridge (see the M3 MUST
list in `NEXT-AI-HANDOFF.txt`).

## M1 — on-device repackager + stub boot (2026-10-15, ACCEPTED)

Branch `m1-repackager` (not merged to `main`, not pushed). Design: `docs/superpowers/specs/2026-10-15-general-32bit-app-support-design.md`.

**What M1 delivers.** The RetroBug launcher repackages a staged 32-bit APK on the phone into a normal installable
64-bit app whose own process boots the game's 32-bit library: `AxmlEditor` (binary manifest rewrite),
`ApkRepackager` (APK transform; libs move to `assets/rb32`), `RepackSigner` (apksig, AndroidKeyStore alias
`retrobug-repack`), `RepackInstaller` (PackageInstaller, off the UI thread). Native side: prebuilt `librbstub.so`
plus `rb_runtime_boot` in `librbguest.so` (Inspect over the one library the stub names plus `assets/rb32sys`; sibling app libs in `assets/rb32` are NOT linked and their imports are trapped; constructors on Tier J, guest
`JNI_OnLoad`, report file).

**Driving it.**
- `adb shell am start -n com.retrobug.launcher/.LauncherActivity -e repack <name> [-e install 0]` (`install 0` = build only).
- `-e bootcheck <name>` checks the boot path without a full repack/install.
- Outputs: `/sdcard/Android/data/com.retrobug.launcher/files/repack/`.
- Runtime logs: `adb logcat -s RBRUNTIME` (use the game's pid) and `/data/data/<pkg>/files/rb-report.txt`.
- Host Java tests: `bash tools/run_java_tests.sh`.

**Acceptance evidence (OnePlus 75ff8cd3, human confirmed the installs).**
- Angry Birds Go: installed arm64-v8a, min/target 24; own process pid 613: `rb_runtime_boot` read `assets/rb32/libABK.so`
  + rb32sys libs; 258 constructors (libABK 253) on Tier J in ~4 s; `JNI_OnLoad` returned 0x10006; `RESULT: startup
  finished with no fault`; then `UnsatisfiedLinkError` `Java_com_rovio_angrybirdsgo_AngryBirdsGoActivity_isDeviceAmazon`
  from `onCreate` — the designed M1 end point (M3 adds the exports).
- Gumball: installed arm64-v8a, min/target 24; own process pid 7414: 24 constructors (libgumball 19) on Tier J;
  headless profile ran 1200 frames to GumballMainMenu (413 fps); `RESULT` ran to the end; then `onCreate` crashed in
  its own Java (below).
- Tests: engine 18 lanes green via `RB_SERIAL=R52Y106BECZ bash tools/run_engine_tests.sh arm64-v8a` on the tablet
  (exit 0; dynarmic 200,561 assertions; guest-apk 1376; diff 998; monitor 167);
  `bash tools/run_java_tests.sh` exit 0, 0 failures (141 + 54 checks).

**Known deviations.** (1) The stub is prebuilt and exports only `JNI_OnLoad` until M3 generates real per-app stubs.
(2) Inspect's 64 MiB window and per-run teardown stay until M2. (3) minSdk/targetSdk raised to 24 (install floor).
Additional known deviations: (1b) each stub boots its library in isolation, with teardown after the run (see above for what is linked).
(4) The in-process boot calls the launcher's `Inspect()`, which runs launcher-only per-game code: for `libgumball.so` that is the
hand-written Gumball profile (stand-in JNIGLSurface + 1200 headless frames; the source of the "1200 frames" evidence), run
synchronously inside the game's `JNI_OnLoad` on the loading thread, after reading the whole APK into memory (`bad_alloc` on a
1 GB+ APK would abort). Fix at the start of M2: a boot-only mode (constructors + guest `JNI_OnLoad`, no profile phase),
streamed/mmapped zip reads, a wider catch.

**Not yet verified on a device** (each needs the human to confirm a dialog; offered as a follow-up):
(a) ~~re-repacking the same game installs as an update~~ VERIFIED 2026-10-16 on the OnePlus (status=0, firstInstallTime unchanged; see M2);
(b) installing a repack over a natively installed original reports the installer's own incompatible-signature text (expected
status=5 INSTALL_FAILED_UPDATE_INCOMPATIBLE).

**Deferred to later milestones** (final review):
- API-level check for on-device Java (compile `android/java` with `--release 8` or lint) before M3.
- The stub finds its APK via the hidden ActivityThread API; derive from `dladdr` by M3.
- Signing-key lifetime (dies with a RetroBug uninstall, so repacked games can't update) and printing the cert SHA-256, by M5.
- Install-path hardening for API 26-32 (APK in app-specific external storage; exported status receiver <= 32).
- Scripted repack re-runs on Activity recreation.
- A failed re-repack deletes the previous good output.
- The confirm dialog can be silently blocked if RetroBug is backgrounded.


**New finding — targetSdk bump breaks old Java.** Gumball's `GumballBillingService.bindToMarketBillingService` does an
implicit `bindService` (`com.android.vending.billing.MarketBillingService.BIND`), which throws `IllegalArgumentException:
Service Intent must be explicit` at targetSdk >= 21. Not an emulator fault. Needs a new "targetSdk compatibility
shims" component (spec Architecture; scheduled before M3 acceptance, which needs Gumball to reach its activity).

**Tablet note.** RetroBug's "Install unknown apps" permission is allowed on the tablet; the implementing agent did not toggle it, but whether the human did or an `adb install -r -g` granted it is unknown.
Whether to revoke is the human's call. Native Angry Birds Go 1.8.7 on the tablet was untouched.

**Next = M2, and the targetSdk compatibility shims before M3's acceptance** (M2: multi-entry core: thread contexts, GGL, clone/futex/exit, adopted ART threads; per the spec).

## Where it stands (all verified on the tablet)

- **Current target game: Gumball Lite** (`com.twentysixdigital.gumballlite`, `libgumball.so`, ARMv5 Thumb,
  GLES1). It is staged in the launcher's library. The Angry Birds APK used earlier is a dead end (the
  "android_port" build is broken natively too) — don't go back to it.
- Gumball boots fully: constructors → `jniClearTextures` → `jniSurfaceChanged` → `jniInit(0)` → a real-time
  `jniRun` frame loop. Splash → **main menu** at ~4 s. **It is drawn on screen** through a real GLES1
  context: ~3.1M GL calls/min forwarded, 11 real textures, 0 unforwardable GL names, no faults.
- The launcher shows live progress during a run and ends the report with a `RESULT:` line (shown at top).
- Tier J runs Gumball faster than real time; Tier I ~6 fps. Both agree on behaviour.
- **All 18 test lanes are green** on the tablet (dynarmic 200,561 assertions; abi_jni 157, linker 40,
  elf 207, monitor 150, tier-diff 980, guest-libc 136, guest-apk 1376 …).

- **Touch works — Gumball is PLAYABLE into a mini-game** (2026-10-15, later). Taps on the run screen reach
  `jniTouch(x, y, action)`; a real tap on "Karate Chop Challenge" took the game MainMenu → LoadingScreen →
  **KarateChop**, drawn and taking taps (screenshot-verified). Details below under "Touch input".

## Angry Birds Go 1.10.1 (2026-10-15, later) — startup complete, no engine driver yet

- The human's "base.apk" (from `/sdcard/retro-bug test/32 bit games/` on the OnePlus) is **Angry Birds Go
  1.10.1** (`com.rovio.angrybirdsgo`, `libABK.so` 16 MB, ARM-state, Exient engine, apktool-repackaged) —
  NOT the dead "android_port" Angry Birds. It runs natively on the human's 32-bit phone. Staged on the
  tablet as `angry-birds-go-1.10.1` (run: `-e run angry-birds-go`).
- Walls fixed, each with rows (all 18 lanes green: elf 214, abi_call 123, diff 998, monitor 167):
  1. Unresolved **weak** imports were bound to the `0xbad0000` trap instead of 0 — its static
     `__cxa_guard_acquire` tests `__google_potentially_blocking_region_begin` for null. New
     `LoadOptions::unresolved_trap`; corpus `tests/corpus/weak_unresolved.c` (`librbweak.so`).
  2. **`mremap`** (163) served (grow in place / MAYMOVE move / shrink; FIXED refused) — bionic's
     `__cxa_atexit` grows with it.
  3. **kuser helpers** (0xffff0f60 cmpxchg64, 0xffff0fa0 barrier, 0xffff0fc0 cmpxchg, 0xffff0fe0 get_tls)
     are host entries in `AbiContext::IsHostEntry`, run by `RunKuserHelper`; both tiers.
  4. **CNTVCT** (`mrrc p15,#1,c14`, OpenSSL `_armv7_tick`) answered from host `cntvct_el0` on both tiers
     (`HostVirtualCounter`); CNTPCT still refused.
- Result: **all 253 libABK constructors run and `JNI_OnLoad` returns 0x10006.** The launcher has no profile
  for it, so nothing is drawn. Next for it: an Exient profile (`ExientGLSurfaceView.nativeInitialize/
  nativeSurfaceCreated/Changed/nativeDrawFrame/nativeInputAddTouchEvent`, read the DEX first), then GLES2
  forwarding, `pthread_create`, OpenSL ES, `AAssetManager_*`, `libjnigraphics`. 53 imports still trapped.

## Git state

- M1 work is on branch `m1-repackager` (local, not merged to `main`, not pushed). All earlier 2026-10-14/15 work is committed on `main` (local, **not pushed** — ask the human before pushing).
  The side branch's GuestLinker IFUNC fix (`d06d354`) was cherry-picked onto main; the branch's snapshot
  commit under it (`30047c0`) was not needed. The junk files are gone.
- `build/device-libs/` holds the tablet's own `libc.so` and `libstdc++.so` (pulled, not in git; required).

## Touch input (2026-10-15)

- Contract transcribed from the DEX: `JNIGLSurface.onTouchEvent` queues `Touch(x = (int)getY(),
  y = getWidth() - (int)getX(), action)` with DOWN→1, UP→2, MOVE→3, CANCEL→4, anything else 0 (getAction()
  includes the pointer index, so POINTER_DOWN is 0), plus a "jab" throttle (>5 events within 1 ms of each
  other are dropped). `onDrawFrame` calls `jniTouch` for each queued touch **after `jniRun`**, then clears.
- Implemented: Java `SurfaceView.setOnTouchListener` → `nativeTouch(action, x, y, viewWidth, timeMs)`; the
  native side (`QueuedTouch`, `g_touches`, launcher.cpp) does the mapping/throttle and the frame loop drains
  it. Touches are accepted only while a drawn frame loop runs (`OpenTouches`). Report: `input:` lines per
  down/up (with the raw view coordinates), a totals line at the end.
- A played run's 60 s cap now counts from the **last touch** (hard cap 15 min); untouched runs still end at 60 s.
- `adb shell input tap` sends down+up within one frame, and Gumball's menu does **not** react to that
  (the game samples touch state in jniRun) — a real finger works. To script a tap, try
  `input swipe X Y X Y 150` (a 150 ms press) — untested.
- The first tap reached the wall **`NewByteArray` (JNI slot 176)**; `NewByteArray` + `SetByteArrayRegion`
  (208) are now in `JniEnv` with abi_jni rows (164 checks). All 18 lanes green after that.
- Next walls to look for while playing: whatever the KarateChop/Dodgeball/CharacterCreator scenes name
  (read `input:` / `stopped` lines), `jniTextureReadyRaw`, sound, `fcntl64`, `FindClass`.

## Devices and commands

- Tablet **Samsung Tab S9 SM-X710, serial `R52Y106BECZ`** (Android 16; it can also run 32-bit natively, so
  it is the ground-truth oracle). adb is NOT on PATH: `$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe`.
  ninja/cmake: `$LOCALAPPDATA/Android/Sdk/cmake/3.31.6/bin`. Shell is Git Bash on Windows; no Python.
- Test suites (builds, pushes, runs all 18 lanes; non-zero exit on failure):
  `RB_SERIAL=R52Y106BECZ bash tools/run_engine_tests.sh arm64-v8a`
- Launcher: `bash tools/build_launcher_apk.sh` then
  `RB_WAIT=20 bash tools/run_launcher_on_device.sh R52Y106BECZ gumball` (`RB_TIER=1` interpreter, `2` JIT;
  default runs the Gumball frame loop on J). Report: `adb -s R52Y106BECZ logcat -d -s RBLAUNCH`.
  Screenshot: `adb -s R52Y106BECZ exec-out screencap -p > shot.png`.
- New games are added through the launcher's "Add a game" picker (push the APK to `/sdcard/Download`,
  drive the picker with `uiautomator dump` + `input tap`). Drive device dialogs yourself, but **never
  bypass Google Play Protect or other security prompts** — that is the human's call.

## Key design facts (don't relearn these)

- Gumball profile lives in `launcher.cpp` (search `Gumball (twentysixdigital`). It mirrors the game's own
  Java (`JNIGLSurface`, read from its DEX with `dexdump`): 16 host methods on the class model —
  `dataFromFile` (APK assets, empty byte[] when absent), `databankRead/Write` (in-memory), `queueTexture`
  (real decode+upload via `LauncherActivity.uploadTexture`; sync = nested `jniTextureReady`, async = next
  frame), the rest counted no-ops (sound, text, alerts, store, ads).
- `JniEnv` (`core/abi/jni_env.cpp`) has a host class model: `DefineHostClass/DefineHostMethod/
  BindHostObject`, `GetMethodID`, `Call{Object,Boolean,Int,Void}Method{,V,A}`, host-side byte[]
  (`GetArrayLength`, `GetByteArrayRegion`, `Get/ReleaseByteArrayElements`). `FindClass` still refuses.
- GL forwarding (`gl_forward.cpp`): signature table "ret:args"; one 10-int/8-float function type calls any
  GLES1 entry point (AAPCS64 allocates x/s registers independently). Headless runs keep counted no-ops.
- Every problem must arrive as a **named** report line; every new behaviour needs a test row that can fail
  both ways; measure on the tablet, don't theorize. Identify walls from the binary (`llvm-objdump
  --triple=thumbv5te` for Gumball's Thumb code, `llvm-nm -D -C`, `dexdump -d` for Java).
- Bugs found this round worth remembering: cross-module IFUNC imports were bound to the *resolver*
  (strcpy was a silent no-op — fixed in launcher `export_of`, and in GuestLinker on the side branch);
  `CallGuest` reported only the last run segment's step count (fixed); `PT_ARM_EXIDX` constant was wrong
  (fixed); old-NDK `init_array` -1 sentinels (skipped like bionic).

## Next steps, in order

1. ~~Touch input~~ done. ~~Merge `d06d354`, commit~~ done. Push when the human says so.
2. Play each mini-game and fix the walls they name.
3. Likely next walls: `jniTextureReadyRaw` (needs RGBA pixels — `uploadTexture` can return them), sound
   (`soundCommand` is a no-op), `fcntl64` (refused ENOSYS), `FindClass`.
4. Later: Tap Tap Revenge 4 (`com.tapulous.taptaprevenge4`) is the ranked second target; Unity/Mono games
   (e.g. LEGO DC Mighty Micros) need threads + GLES2 + JIT-written code support first.

---

# Earlier session history (detail; may be partly superseded by the section above)

# RetroBug — session handoff (2026-10-10 ~00:20 +04)

**Read this first.** You are continuing RetroBug: an ARMv7-on-ARM64 CPU emulator for Android
(`core/` engine + `android/jni/launcher.cpp` on-device app + a vendored dynarmic fork). The human's
active goal is **"reach into gameplay"** — drive the launcher until Angry Birds' own native code
executes deep enough to be called gameplay. Acceptance evidence is the **device report**
(`adb logcat -s RBLAUNCH`), not unit tests alone. Do not stop at green suites; the wall ladder below
is the progress measure.

Working tree state: **~2 sessions of work are UNCOMMITTED.** HEAD is `6249d02` and is itself unpushed
(`git status` shows `ahead 1` + dirty). CI is red until pushed (workflow fix is in the tree). This
document plus the code is the source of truth; task list in the Qoder UI lists the open items.

## Where the run stands — the wall ladder (each fixed and measured on the SM-X710 Tab S9)

1. `__libc_preinit` → `getauxval` faulted at 0x0 → launcher now supplies **auxv** (decodes
   `getauxval`'s globals-read offset +0x414 at load, writes AT_RANDOM + AT_NULL). ✓ device-verified.
2. `ldab` in `__libc_safe_arc4random_buf` (ARMv8 encoding) → **patch 0005**
   (LDAB/LDAH/STLB/STLH Thumb rows). ✓
3. `stlex` in `__cxa_guard_release` → **patch 0006** (STLEX/STLEXB/STLEXH/STLEXD rows). ✓
4. libc's `netdClientInitImpl` called `dlopen` through a trapped slot → the **dl*/__loader_* host
   slots** are bound (dlopen/android_dlopen_ext → NULL, sdk version → 34, dtor hooks counted no-ops;
   `say_dl` discloses each), and the report now **names** unanswered scope imports
   (`scope_unanswered`, deduped). ✓
5. Preinit's tail reads `*(TP-4)` = `TLS_SLOT_BIONIC_TLS` → the page below the TLS block is mapped
   and the thread-struct pointer written at `TP-4` (comment corrected against AOSP research: TP-4 is
   `bionic_tls*`, TP+4 is the thread struct, errno at `[TP+4]+0x29c`). ✓
6. The app's constructors: `mmap2` was refused ×19 → **guest-heap `mmap2`/`munmap`** + **fd 1/2
   console capture** (`write`/`writev`, `capture_dropped`) + `futex`/`rt_sigaction`/`faccessat` in
   the syscall monitor. ✓ unit-verified (147 checks) and device-verified: **all 17 constructors ran
   (12 of them `libangrybirds.so`)**.
7. The launcher called the game's `Java_..._MyRenderer_nativeInit` with a raw `char*` where the DEX
   declares `Ljava/lang/String;` → the launcher now **mints a real jstring** via the env's own
   `NewStringUTF` before the call. ✓ device-verified: the game then ran **324,979 steps** of its own
   code before stopping.
8. **CURRENT WALL: `VectorZeroExtend32: no Tier I semantics yet`** at pc=0x473c8 (libc's NEON path,
   called from the game). **The fix is IN THE TREE but NOT YET BUILT/RUN**: `core/exec/interp.cpp`
   now implements all eight `VectorZeroExtend8/16/32/64` + `VectorSignExtend8/16/32/64` (arranged
   widen, UXTL/SXTL semantics per `emit_arm64_vector.cpp:1858-1876`), with diff rows
   `the_wide_vector_adds_widen_like_hardware` (`vaddl.s32` = `{0xefa0,0x0001}`, `vaddl.u32` =
   `{0xffa0,0x0001}`; widens BOTH operands then adds — d0=0xFFFFFFFF, d1=2 gives {1,0} signed vs
   {0x100000001,0} unsigned).

### 2026-10-14 session (Claude Code) — walls 8–11, all device-run on the SM-X710

8. Vector extend: **device-verified**. The diff row was wrong, not the engine: `vaddl q0, d0, d1` writes
   q0 = d0:d1 (the row read d2/d3, and expected 0x1'00000000 for 0xFFFFFFFF+2). Fixed; both tiers agree.
   Also: futex `& 0xff` kept FUTEX_PRIVATE_FLAG and refused every private op incl. 0x89/0x8a — now
   `& 0x7f` + the 9/10 mapping, with 0x89/0x8a rows.
9. `nativeInit` ran out of `kCallBudget` inside Lua's GC (real work) → `kEntryBudget` (50x) for the entry
   point only.
10. `GetObjectClass: needs the guest class model (C09)` — KA3D reads EVERY file via
    `MyRenderer.readFile(String)[B` (`io::FileInputStream` +0xf0b7c). **JniEnv now has a host class
    model**: `DefineHostClass/DefineHostMethod/BindHostObject`, `GetMethodID`, `Call{Object,Boolean,Int,
    Void}Method{,V,A}` (AAPCS variadic/va_list/jvalue decoding), host-side `byte[]` (`GetArrayLength`,
    `GetByteArrayRegion`), New/DeleteGlobalRef for permanent objects. Rows: compiled fixture
    `read_file` (=5055) and `mix_args` (all 3 forms =882) + refusals. The launcher binds the stand-in to
    `com/rovio/ka3d/MyRenderer` with `readFile` transcribed from the DEX (`name.zip` first entry, then
    `assets/name`, else null), served from the APK with zlib inflate (`ZipEntry`, `-lz`).
11. `terminate called after throwing io::IOException` — `dl_unwind_find_exidx` answered NULL so no
    catch was findable. Now answered from each module's PT_ARM_EXIDX (loader records `exidx`/
    `exidx_count`). **Latent bug found by the new row: `elf::PT_ARM_EXIDX` was 0x70000068, is 0x70000001.**
    Result: **`nativeInit` RETURNS** (`reason=Returned`, value 0 = its own catch(Throwable) path).

**CURRENT WALL:** `GameApp::GameApp` → `FileBundle::parseIndex(App::path("bundleIndex.idx"), …)` →
`readFile("")` → null → IOException → nativeInit's catch → returns false. `App::path` joins
`AndroidOSInterface::getDefaultDataPath()` (vtable slot 3, returns the literal `""`), the app's subdir
(`App+8`) and the name via `io::PathName`; the result is `""`. The APK has **no** `bundleIndex.idx` under
any name, so this may be how this "android_port" build behaves on hardware too — unverified: a native
install on the Tab S9 is blocked by Google Play Protect ("built for an older version of Android"); the
human must approve that themselves if a hardware oracle is wanted (`pm install
--bypass-low-target-sdk-block` from /data/local/tmp, then the Play Protect dialog). Next: decide whether
`App::path`'s "" is correct emulation (trace `io::PathName(String,String)` at +0xebc14 and `App+8`), and
what a successful real launch does here.
Report additions: `java:` block (every readFile with source or null reason, stack-scan callers for an
empty name via `JniEnv::calling_state()`), `unwind:` line, GL list sorted by calls.

### 2026-10-14 (later): target switched to Gumball Lite — it reaches its MAIN MENU on both tiers

The Angry Birds "android_port" APK was confirmed broken natively by the human (it never ships
`bundleIndex.idx`). Triage of the tablet's 31 installed 32-bit apps picked **Gumball Lite**
(`com.twentysixdigital.gumballlite`, `libgumball.so` ARMv5 Thumb, GLES1, no pthread_create). It is staged
in the launcher's library (`files/games/gumball-lite.apk`, added via the picker from /sdcard/Download).
Run: `RB_WAIT=150 [RB_TIER=2] tools/run_launcher_on_device.sh R52Y106BECZ gumball`.

Result (device-verified): `jniClearTextures` → `jniSurfaceChanged` → `jniInit(0)` → 1200 × (`jniRun(1/60)`,
`jniAcceleration`) all return. **Frame 242 (4.03 s game time): GumballSplash → GumballMainMenu**, 474 GL
calls/frame, 11 textures, `soundCommand("Menu_Music.caf")`. Tier J: 91.6 frames/s; Tier I: 6.1 frames/s;
both agree frame-for-frame on the scene switch.

What it took (all in the tree, uncommitted):
- Loader: `init_array` 0/-1 sentinels skipped like bionic (`init_array_sentinels`; linker_test row).
- Scope: the device's `libstdc++.so` (build/device-libs, packaged as an asset, placed only if DT_NEEDED);
  liblog answered by host slots 336.. with a guest-printf formatter → **`guest log:` report block**.
- **Engine bug: cross-module IFUNC imports were bound to the resolver.** `FindExport` only maps an IFUNC
  export to its implementation if the defining module resolved a slot of that name; libc doesn't for
  strcpy/stpcpy/strcat/other `_chk`s, so the game's `strcpy` was a silent no-op (names stayed heap junk →
  splash never found its texture). Launcher `export_of` now runs the resolver (cached) for scope + app
  imports. **`GuestLinker` has the same gap — not fixed there yet; needs a test.**
- **Bridge bug: `CallGuest` reported only the last run segment's instruction count.** Now the whole call.
- JniEnv: `Get/ReleaseByteArrayElements` (modes 0/COMMIT/ABORT, arena-backed, `elements_probe` = 5511002
  row), `HostArray()`; launcher arena 32 KiB → 1 MiB.
- Launcher Gumball profile (`JNIGLSurface`, transcribed from DEX): 16 host methods — `dataFromFile`
  (assets/, empty byte[] when absent), `databankRead/Write` (in-memory), `queueTexture` (size from PNG/JPEG
  header, `@2x` halved, 128² placeholder; sync = nested `jniTextureReady`, async = next frame), the rest
  counted no-ops. Scene switches reported by vtable→`_ZTV` class name; post-run scene/libc probes.
- Report bookkeeping: host-answered non-GL imports no longer listed as trapped; GL list sorted by calls.

UI: the report screen now shows live progress while a run works (`nativeProgress` polled every 300 ms:
elapsed time, line count, latest line / "gumball frame N of 1200 on tier X - scene Y"), and Gumball's entry
calls run on Tier J unless tier 1 is chosen explicitly — a default-tier run finishes in ~2 s (1200 frames
in 1.7 s) instead of a silent 90 s. Verified with on-tablet screenshots.

### 2026-10-15: GL renderer — Gumball's main menu is DRAWN on the tablet

`android/jni/gl_forward.{h,cpp}`: `EglTarget` (GLES1 context on the launcher's SurfaceView via
`ANativeWindow_fromSurface`, current on the run thread) and `GlForward` (≈90 GLES1/OES entry points with
"ret:args" signatures; softfp args from r0..r3+stack, guest pointers via `GuestMemory::Host`; one
10-int/8-float function type calls any of them — AAPCS64 allocates x/s registers independently).
GL slots forward while `gl_live`; glGetString stays guest-answered; failures recorded per name, fall back
to no-op. Textures: `queueTexture` uploads real pixels through static Java `uploadTexture` (BitmapFactory,
POT scale, LINEAR/CLAMP, GLUtils.texImage2D — Gumball's own textureLoaded) and hands the game the real GL
name. Frame loop: real-time dt (≤0.25 s), `eglSwapBuffers` per frame, 60 s cap. Java: RUN screen
(SurfaceView + status overlay), the run starts on the first surface of the game's own orientation
(`getPackageArchiveInfo` → first activity's screenOrientation; Gumball = portrait, it rotates its own
content); manifest `configChanges` so rotation doesn't restart the activity. Headless runs unchanged.
Verified by screenshot: full main menu, ~3.1M GL calls forwarded per minute, 0 unforwardable names.
All 18 suites green.

Next for "gameplay": inject `jniTouch` taps (menu → a mini-game), then decode PNG for `jniTextureReadyRaw`
if a raw texture appears; real pixels need a GL forwarder (big). Suites: all 18 lanes green after every
change above (abi_jni 157, linker 40, elf 207, monitor 150, diff 980).

**Earlier immediate next action (done this session):** build (`ninja -C build/tests-arm64-v8a rb_exec_tier_diff_tests
rb_syscall_monitor_tests`, then `tools/build_launcher_apk.sh`), run suites on `R52Y106BECZ`, then
`RB_WAIT=130 tools/run_launcher_on_device.sh R52Y106BECZ angry` and read the next named wall.

## Verification status of the uncommitted work (be careful with claims)

- **Device-verified suites** (last full run, SM-X710): memory 190/0 (incl. 16 KiB granule rows),
  elf 186/0, diff 968/0 (before the vaddl row), abi_jni 112/0, monitor 147/0 (before the futex-0x89
  and capture-chunking edits), dynarmic 200,561 all-pass. The guest-libc ADVISORY lane went from
  132/4 to **136/0** — its four `strtod` walls fell to patches 0005/0006; make that lane gating again
  once re-run.
- **In tree, built but NOT yet device-run**: the vector-extend batch; `futex` ops 9/10 (bionic's real
  `0x89/0x8a` — the previous `&0x7f` mask refused the real mutex slow path; review catch);
  `CaptureOutput` chunking (a single write >4 KiB used to lose bytes silently); `scope_unanswered`
  dedup; TP-4 comment correction.
- **Known open** (tracked as tasks): VCMP sNaN IOC / FZ in compare+to-fixed / fixed→single rounding /
  fault-pc reporting (B5); Tier J exclusive-load destination writeback divergence (issue-#35 family —
  minimal repro in the acquire diff row, do not assert the wrong value green); TLS/GL cluster is
  mapped AFTER module placement (move before `load_scope` — a module tail in [0x3FF000,0x410000)
  would be silently replaced; pre-existing, widened by the TP-4 page); `munmap` can free launcher
  shim pages; `mmap2` assumes window < 2 GiB (fine at 64 MiB, tests 64 KB); `rt_sigaction` ignores a
  failed oldact write; `refusals()` accessor unused until the self-audit lands; self-diag
  implementation (task: crash capture + `Auditor` wiring; `rb/diag/audit.h`, `GuestMemory::Audit`,
  `JniEnv::Audit` already compile).
- **Uncommitted junk to clean before any commit**: the 38-byte `file` at repo root and
  `.tmp_diff_6249d02.txt` (both known-junk from earlier sessions).

## Devices, commands, loop

- **Tab S9 SM-X710** (`R52Y106BECZ`): AArch32 oracle, 4 KiB pages. **OnePlus CPH2573** (`75ff8cd3`):
  64-bit-only negative control (its launcher was reinstalled under the local signing key this
  session; Angry Birds re-staged in its library).
- `adb` is NOT on PATH: `$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe`.
- Suites: `RB_SERIAL=<serial> tools/run_engine_tests.sh arm64-v8a` (builds, pushes, runs all 18
  lanes; script exits non-zero on any gating failure).
- App: `tools/build_launcher_apk.sh` → `RB_WAIT=130 tools/run_launcher_on_device.sh <serial> angry`
  → `adb -s <serial> logcat -d -s RBLAUNCH`.
- ColorOS installs use the `pm install-create/write/commit` session API (the scripts do this);
  **drive every on-device dialog yourself** (uiautomator dump + `input tap`) — never wait for the
  human to press Update/Allow.
- CI: red until pushed; the fix (build `libm libz`, and the Windows `awk`/pipefail race in
  `tools/build-guest-libs.sh`) is in the tree. Local HEAD is 1+ commits behind the tree.

## Hard-won gotchas (do not relearn these)

- **Never trust a piped exit code.** `ninja ... | tail` reports tail's status; a failed build once
  shipped a stale binary to the device. Check `$?` of the tool itself (or redirect to a file).
- **Guest test blobs need their own addresses per row.** Re-planting over a previous blob's address
  executes the stale cached translation on both tiers (no cache invalidation on host-side writes).
- **Fixture writes need mapped pages** — `GuestMemory::Write` refuses unmapped addresses; `Map`
  the fixture range first.
- **Identify walls from the binary**, not from theory: `llvm-objdump`/`llvm-nm` over
  `build/device-libs/libc.so` (base 0x2000, so guest = file + 0x2000). Every wall tonight was named
  this way. `llvm-mc` is absent; assemble probes with `clang --target=armv7... -mthumb` +
  `llvm-objdump -d`.
- **dynarmic decoder tables ignore `ArchVersion`.** New v8 encodings are added as captured rows in
  `patches/dynarmic/` (0001 LDAEX, 0005 acquire byte/half, 0006 STLEX). All patches must keep
  reverse-applying (`git apply --check --reverse --directory=external/dynarmic <patch>`); the v8
  arch-version pin stays v7.
- **The report has rules**: every problem arrives as a NAMED line ("refuse loudly"); new behavior
  needs a disclosure line plus a unit/diff row that can genuinely fail. Crashes are unacceptable
  (see `RefusingCoprocessor` for the p0–p14 case).
- Machine-local memory files exist at `C:\Users\yebot32\.qoder\memory\` (user scope) and
  `C:\Users\yebot32\.qoder\projects\C--Users-yebot32-Downloads-Telegram-Desktop-Retrobug\memory\`
  (project scope) — read them for collaboration preferences and project state.

## What "into gameplay" means here, and the path

This build runs the game's native library through the engine; there is no Java side. "Into
gameplay" = the game's own init executes its real work through the emulator — assets opened from its
APK (the file provider serves exactly that path), engine objects built on the guest heap, GL calls
answered by counted stubs — and ideally `nativeInit` returning. After that, extending `Inspect` to
drive `nativeUpdate` a few frames pushes the boundary further if the human wants frames.

Expected next walls (each will name itself): more Tier I IR gaps from KA3D's NEON/FP code
(`Vector*` ops — the audit's coverage list is in `docs/32`), possibly `clone`/threads if the engine
spawns them (refused by name today), NDK math in libm. Fix pattern per wall: identify from the
binary → implement in
`interp.cpp` (Tier I) with the A64 emitter as the semantics reference → add a differential row that
can fail both ways → build → device run → read the next wall.
