# Session backlog — `session/m5-m6`

Status: `todo` · `doing` · `done` · `blocked` · `deferred` (out of this session's reach, kept for later).
Sources: four read-only analyses (2026-10-18) — conversion+build (C), UI+Data tab (U), graphics/sound/input/perms +
patch groundwork (G), emulator+bridge (E) — plus device tests. Paths are under `android/` unless they start with
`core/` or `tools/`; `java/…` means `android/java/com/retrobug/launcher/`. "unconfirmed" = read from code, not seen
on a device.

## A. Session tasks (in order)

| # | Task | Status |
|---|---|---|
| A1 | Analysis → this backlog | done |
| A2 | Data tab: device edge cases, fixes, merge into the session branch | done (9ec4366) |
| A3 | SpongeBob Game Station: playable offline (patch on the `.rb` copy only) | blocked (G-Presto anti-tamper; see spongebob-gamestation.md) |
| A4 | M5: compatibility table for every 32-bit game on the Tab S9 (`docs/compatibility.md`) | done (4 rows need a clean re-run) |
| A5 | M6: patches, mods, resolution | done (device-verified 2026-10-18) |
| A6 | Polish | todo |
| A7 | Release prep, final summary | todo |

## B. Data tab (branch `data-tab`) — device-tested 2026-10-18

Passed on the Tab S9: OBB install into the copy-owned folder (SMB Bounce 100 MB → title screen), export, import,
byte-identical round trip, undo offered, adb shell refused, zip-slip refused, non-zip refused, another game's save
refused, no-OBB status text.

| # | Finding | Where | Status |
|---|---|---|---|
| B1 | Export on a copy without the save helper opens "Save as", then fails and leaves a 0-byte zip (seen: Gumball) | java/SaveClient.java:41-57, GameMenu dataTab | done |
| B2 | No free-space check before OBB install/add; a failed copy leaves `.part` | java/ObbStore.java install/add | done |
| B3 | Conversion says Ready even when the automatic OBB copy failed | java/ConvertJob.java OBB branch (`onDone(true…)`) | done |
| B4 | Each 8 MB progress tick rebuilds the whole tab (PackageManager + FUSE stat + a thread each time) | GameMenu.java progress()/select | done |
| B5 | Undo zip truncated before the new one is written; staging left on exceptions | shim RbSaveProvider.restore, SaveArchive.unpack | done (writeTo, unpack cleanup; SaveArchiveTest 46/46) |
| B6 | Refusals not logged; "null" messages; raw ZipException text | RbSaveProvider:101, GameMenu.busyWith | done |
| B7 | rb-report.txt exported/overwritten as a save | SaveArchive.ours | done |
| B8 | Two OBB installs for one game can race on the same `.part` | GameMenu.busyWith vs ConvertJob | done |
| B9 | Import while the game runs: replace races the game's own writes (kill after 300 ms) | RbSaveProvider:107-140 | deferred (risk accepted: the import kills the process right after; documented) |
| B10 | Picker result lost if the activity is recreated | LauncherActivity picked_ | deferred (polish) |
| B11 | Export includes Android/data/<pkg>/files which may be large | SaveArchive SECTIONS | todo (show size in the done message) |

## C. Bugs and gaps found by the analysis (selected; the full reports are summarized in worklog.md)

| # | Sev | Finding | Where | Status |
|---|---|---|---|---|
| C1 | bug | `META-INF/` dropped whole — `META-INF/services/*` (ServiceLoader) lost | java/ApkRepackager.java:248 | todo |
| C2 | bug | versionName stamp breaks games (fixed on data-tab by the meta-data stamp) | ApkRepackager.java:226-232 | done on data-tab |
| C3 | bug | Static repack flags set by the scripted `-e repack` path before the slot is claimed; stubCheck without a claim | LauncherActivity.java:202-205, 843 | todo |
| C4 | risk | Diagnostic intent extras honoured by the exported activity in release builds; parseInt crash | LauncherActivity.java:177-229 | todo (release prep) |
| C5 | gap | Signed `<pkg>.rb.apk` kept forever in external files | ConvertJob.java:79-81 | todo |
| C6 | gap | No free-space check before converting | ConvertJob.java:79-84 | todo |
| C7 | gap | Base-has-v7a + other splits classified GAME (copy misses split resources) | GameCatalog/LibraryRules.classify | todo |
| C8 | gap | `sharedUserId` kept on rename | AxmlEditor.renamePackage | todo |
| C9 | gap | `maxSdkVersion` never removed | ApkRepackager | todo |
| C10 | risk | Every entry read fully into memory (OOM on big games) | ApkRepackager.java:262-263, 305-307 | done (8b3d22e; Among Us 497 MB on device) |
| C11 | bug | Every `show(HOME)` builds a new LibraryScreen: conversion state lost after visiting Settings | LauncherActivity.java:296-319, LibraryScreen | todo |
| C12 | bug | Install prompt backed out → slot can stay held for the process lifetime (device dependent) | ConvertJob/RepackInstaller | todo (timeout/abandon) |
| C13 | bug | Menu sizes computed once; rotated menu clipped | GameMenu.java:65-85 | todo |
| C14 | bug | Menu header action uses a stale Game after target change | GameMenu.java:130-136, 347-358 | todo |
| C15 | bug | Added APK copied straight to its final name; a failed copy leaves a broken tile | LauncherActivity.java:1115-1149 | todo |
| C16 | polish | Disabled primary button text invisible | HoloTheme.java:79,93-96 | todo |
| C17 | polish | Hard-coded badge colours; "Update available" same as orange accent | GameTile.java:77,85 | todo |
| C18 | polish | Developer corner still unstyled; back from DEV goes home | LauncherActivity.java:331-512 | todo |
| C19 | placeholder | "Coming with M6" texts | GameMenu.java:303-306,372,399-416 | done (M6 landed) |
| C20 | polish | Stretched non-square icons; ArtCache per screen; no in-flight dedupe | ArtCache.java | todo |
| C21 | gap | Empty state ignores the "Library shows" filter; no Add button | LibraryScreen.java:140-141 | todo |
| C22 | risk | Catalog scan exceptions leave "Looking for your games…" forever | GameCatalog.java:52-87 | todo |
| C23 | gap | Permission shim asks for every dangerous permission at once (SMS, contacts, location…) | shim/Shims.java:122-172 | todo (ask only what is needed) |
| C24 | gap | Settings parsed with `contains`, 1 KiB cap — blocks M6 lists | shim RbShimProvider:39,72-74; jni/runtime_boot.cpp:66-81 | done (93d258c) |
| C25 | gap | dlopen/dlsym stubbed → all Unity games fail (Teeny Titans 2, FC All-Stars) | jni/host_slots.cpp:315-340 | done (44096c0) |
| C26 | gap | No signals delivered (Mono/Boehm GC) — Unity/Mono needs this too | core/syscall/monitor.cpp:507-510 | deferred (engine milestone) |
| C27 | bug | JNI_OnLoad/constructors run under a 4M-step budget that poisons the process | jni/runtime_boot.cpp:753-754 | todo |
| C28 | bug | ARM32 vs AArch64 `O_*` flags not translated (O_LARGEFILE→O_NOFOLLOW, O_DIRECTORY→O_DIRECT) | core/syscall/host_io.cpp:291,297 | todo |
| C29 | gap | Missing GLES2 ext / GLES1 fixed-point / GLES3 / sensor teardown thunks | jni/thunks.cpp | todo (per M5 evidence) |
| C30 | gap | `glGetString` advertises ES 3.2 + extensions with no thunks | jni/host_slots.cpp:471-481 | todo |
| C31 | gap | NativeActivity games unsupported (no ANativeActivity_onCreate) | StubElfWriter.java:197 | deferred (say so on the tile) |
| C32 | perf | Always-on profiler map grows forever | jni/runtime_boot.cpp:542-582 | todo |
| C33 | risk | `rb_runtime_boot` returns JNI_VERSION_1_6 even on failure | jni/runtime_boot.cpp:860 | todo |
| C34 | gap | Many syscalls ENOSYS (uname, sysinfo, sched_*affinity, prlimit64…) | core/syscall/monitor.cpp:1403-1418 | todo (per M5 evidence) |

## D. Release prep (from C)

| # | Finding | Where | Status |
|---|---|---|---|
| D1 | versionCode/versionName hard-coded 1 | tools/build_launcher_apk.sh:212, AndroidManifest | todo |
| D2 | d8 without `--release`; no shrinking | tools/build_launcher_apk.sh:183,194 | todo |
| D3 | Old "probe" text in CI release notes and build script output | .github/workflows/build-apk.yml:177-185; build_launcher_apk.sh:274-281 | todo |
| D4 | CI publishes with a throwaway key when the secret is missing | build-apk.yml:129-131 | needs-me (not touched: publishing is out of scope) |
| D5 | App icon is the Android robot | AndroidManifest.xml:23-27 | needs-me (artwork) |
| D6 | Manifest typo "librguest.so"; label not a resource | AndroidManifest.xml:17,48 | todo |
| D7 | No keys or passwords committed (checked) | — | done |

## E. M6 plan (from G)
- Patches: native byte patches applied in memory at load (`runtime_boot.cpp` LoadWithNeeded, gated by sha256 +
  original bytes), manifest patches at conversion, behaviour patches in the shim (Java) — no dex rewriting.
- Mods: asset replacement at conversion time (keep compression method), reversible by re-converting.
- Resolution: aspect via `resizeableActivity=false` + `maxAspectRatio` at conversion; render scale via the shim
  (`SurfaceHolder.setFixedSize` + touch scaling) for Java-surface games.
