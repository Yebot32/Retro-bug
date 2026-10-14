# RetroBug — handoff (2026-10-15) — START HERE

You are continuing **RetroBug**: an emulator that runs legacy 32-bit ARM (armeabi / armeabi-v7a) Android
game libraries on 64-bit ARM phones by translating the ARM code (vendored dynarmic JIT = "Tier J", a
hand-written IR interpreter = "Tier I"). Engine in `core/`, on-device launcher app in `android/`
(`android/jni/launcher.cpp` + `android/jni/gl_forward.cpp` + `android/java/.../LauncherActivity.java`).
Everything below this section is older, detailed session history — read it only when you need depth.

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

- All 2026-10-14/15 work is committed on `main` (local, **not pushed** — ask the human before pushing).
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
