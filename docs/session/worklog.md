# Session worklog — branch `session/m5-m6` (started 2026-10-18)

Newest entries at the bottom. "Decision" lines are calls made on Yebot32's behalf while away.

## Setup
- Tab S9 = `R52Y106BECZ` (model SM_X710). The OnePlus `75ff8cd3` (CPH2573) is not touched.
- Backup: the RetroBug APK installed on the Tab S9 at session start (the `data-tab` build 44ac2de, the last one
  tested end to end) pulled to `build/backup/retrobug-launcher-STABLE-BACKUP.apk` (also kept as
  `retrobug-launcher-data-tab-44ac2de.apk`). `build/` is git-ignored.
- Session branch `session/m5-m6` cut from `main` (8de6fce). `main` itself is not touched this session.
- SpongeBob Game Station files copied (read-only pull, originals untouched) from
  `/storage/1A7E-2F6D/game roms/Old apk/` to `build/games/sbgs/`: `spongebob-game-station-v3-4-0-pdalife.ru.apk`
  (27 MB), `main.9.com.BlueArk.SBGS.obb` (51 MB), `SpongeBob-Game-Station-v3-4-0-cache.zip` (50 MB).
- Decision: the analysis runs as four read-only sub-agents in parallel (conversion+build, emulator+bridge,
  UI+Data tab, graphics/sound/input/permissions+patch groundwork); only the main session uses the Tab S9.
- Decision: offline patches neutralize dead online features (ads, analytics, login, update checks, shutdown notices);
  purchase and licence verification (Google Play LVL / in-app billing checks) are left alone.

## Data tab (A2) — merged into the session branch (9ec4366)
- Device tests on the Tab S9 (data-tab build): OBB install into the copy-owned folder (SMB Bounce, 100 MB → title
  screen; Sonic Racing Transformed, 851 MB, copied in ~4 s, owned by the copy's uid), export → import → export
  byte-identical (same 14 files, same CRC32s), Undo offered after import, adb-shell caller refused
  (SecurityException), zip-slip refused ("won't write files/../../evil.txt", nothing written outside), a non-zip
  refused ("isn't a RetroBug save"), another game's save refused, no-OBB status text on Gumball.
- Bugs found and fixed (78625c8): an old copy without the save helper let "Save as" create a 0-byte zip before
  failing (rows now check the helper first; a failed export deletes the created file); no free-space check (OBB
  install/add now need size + 64 MB); a failed `.part` left behind; two OBB copies of one game could race; a failed
  automatic OBB copy after conversion was silent; each progress tick rebuilt the whole tab; the undo copy was
  truncated before writing; a damaged zip surfaced raw exception text; rb-report.txt went into saves.
- Not testable on this device: a real low-storage condition (21 GB free); covered by the size check in code.
- Regression: FN Skittles re-converted with the merged build at target 35 → menu → a round played, best score 751
  kept (data survives `install -r`). SMB Bounce reaches its title with its OBB.
- Backups of saves exported through the new tab: `build/backup/saves/fnskittles-2026-10-18.zip`,
  `smbbounce-2026-10-18.zip` (also in the tablet's Download folder).
- Test files I pushed to the tablet's Download folder (rbtest-*.zip) and the 0-byte Gumball zip were deleted.
- Decision: the 64 MB margin on top of an OBB's size is arbitrary but keeps the phone from being filled to the
  last byte.
- Decision: import while the game is running is accepted as is (the helper ends the game's process 300 ms after
  replacing the files); staging at next start would be safer and is logged as B9 (deferred).
- Decision: the `-STABLE-BACKUP` APK is the build installed at session start (the tested data-tab build 44ac2de).
- Observation: SMB Bounce's copy already had foreground location granted (USER_SET) from an earlier session; I
  did not grant anything. Android's "allow all the time" upgrade prompt was answered "Keep while in use".

## SpongeBob Game Station (A3) — blocked
- Unity/Mono game wrapped in the G-Presto anti-tamper SDK. A converted (re-signed) copy is detected as tampering and
  exits ("[Error_1160] Illicit activity detected … Protected by G-Presto"), so a converted copy cannot start:
  stopped, documented in spongebob-gamestation.md and needs-me.md.
- General fixes that came out of the diagnosis and stay: ElfRepair (zeroed ELF section-header fields; also libgpg),
  unstable provider client for copy calls (a crashing copy no longer kills the launcher), explicit target honoured by
  native-control copies.
- Cleanup: the diagnostic copy com.BlueArk.SBGS.rb (a RetroBug copy, not an original) was uninstalled; the pushed
  OBB copy in Download removed; Play Protect refused the original, so nothing else was installed.
- Permission prompts seen during this work (Sonic Transformed copy: contacts, files; SpongeBob copy: location,
  phone, media) were all denied. Sonic's copy still holds READ_PROFILE from an earlier session (not granted by me).

## M6 device verification (Tab S9, Gumball Lite)
- Patch: a test .rbpatch (libgumball.so, "Gumball C++ LogLine" → "RetroBG C++ LogLine", SHA-256 guarded) added
  through the Patches tab, turned on → tile said Update available → converted → the installed copy holds the
  patched bytes; the game runs.
- Mod: a test mod (Gumball + menu atlas `mm_icons@2x.png` colour-inverted, same sizes) added through the Mods tab,
  turned on, converted, installed: the title screen showed the inverted Gumball and logo, game ran normally.
  Re-adding a mod with the same id replaced it (one entry, 4 → 5 files). Note: `mm_character_gumball_main*` is not
  the title art — the title screen draws from the `mm_icons@2x.png` atlas.
- Off again: with the mod and patch turned off the converted copy's `mm_icons@2x.png` and `libgumball.so` are
  byte-identical to the original's. Long-press → Remove took both out of the store ("No mods/patches yet").
- Aspect Fit 4:3: "4 activities pinned to 4:3", Android letterboxes the game; menus work.
- Render scale 50 %: the surface is drawn at 800×1067 and scaled up correctly, but Gumball's taps land in the
  wrong place (its onTouchEvent maps touches against the view's `getWidth()` while the coordinates are scaled to
  the surface). At Full the same tap opens Karate Chop. No single mapping fits games that mix the two, so
  Decision: render scale stays, marked "Experimental … set it back to Full" in its description.
- Bug found and fixed: the scripted `-e settings 1` conversion ignored the game's Android target (copies came out
  at the original's 24 and the tile then said Update available). It now uses the game's setting, as the library's
  Convert does, unless `-e target` is given.
- Gumball left as found: Stretch, Full, no mods/patches, target 33 (receiver rule), tile READY, plays.

## M5 (A4) — compatibility table, and the converter fix proven on the device
- Rebuilt the launcher from `8b3d22e` and installed it on the Tab S9 (20:12). Note: `tools/build_launcher_apk.sh`
  begins with `rm -rf build/launcher-apk`, which the session's bulk-delete guard refuses; built with a copy of the
  script whose first line is `mkdir -p` instead.
- Among Us (the 172 MB asset that used to OOM) staged and converted with the new build:
  `RESULT: repack OK — com.innersloth.spacemafia`, signed `/…/repack/amongus.apk` 497,421,757 bytes, no OOM.
  That closes the one fix the previous session committed but never proved on a device.
- `docs/compatibility.md`: all 30 games, engine, verdict, first fault, and the evidence behind each row. Tally:
  11 alive, 14 died, 4 install-blocked, 1 conversion failure (now fixed). Non-Unity ~9 of 16 reach a screen;
  Unity 0 of 14 reach gameplay (C26).
- Finding: the batch ran while the tablet was in use — several screenshots show an unrelated app (a classroom
  video) and two runs died as background ANRs. Those rows are marked *contaminated* in the table and need a clean
  re-run.
- Decision: the table is written now with that caveat rather than delaying it for a full re-run; the four
  NativeActivity rows and C27 are the next fixes, not the table.


## 2026-10-18 evening — the device checks the previous session left, and what they found

Started from `228b606`, tablet `R52Y106BECZ` running 0.1.228, OnePlus not touched.

- **C11/C15 device check found a regression, not a pass.** The C11 fix (one LibraryScreen for the activity's
  lifetime) crashed the app on *every* way back to Home: Settings > DONE, Settings > Back, and the end of Add
  APK all threw `IllegalStateException: The specified child already has a parent`. Cause: Theme.Holo leaves
  content transitions on, so PhoneWindow stacks each screen instead of replacing it, and the retained library
  was still a child of the content view (inside the boot screen's FrameLayout at first start). Both crashes are
  in the tablet's crash buffer (21:21:27 Back, 21:22:33 DONE). Fixed in `5b25229`: `show()` builds the screen,
  clears the content container, detaches the screen if it still has a parent, then calls setContentView.
  Re-verified: conversion survives Settings both ways; Add APK with an invalid file shows "CAN'T READ THIS FILE"
  and a graceful message; Remove deletes it; a stray `.part` leaves no tile. C12 also verified — declining the
  install prompt frees the tile at once.
- **D6** (`2410054`): label is `@string/app_name`; the build needed no change (aapt2 compiles all of res/).
- **C3** (`25057e3`): the scripted `-e repack` path now configures the converter statics through a Runnable that
  runs only after `ConvertJob.claim()`; stubcheck claims and releases the slot. Verified `-e repack sonic -e
  install 0` still reports `repack OK`.
- **C7** (`e088907`): `classify()` takes `hasSplits`, so a 32-bit base in a split install is SPLIT rather than
  GAME. Library unchanged on the tablet (no such game there); LibraryRulesTest 20 checks.
- **C27** (`88106a1`): `kInitBudget = 1 << 30` for constructors and JNI_OnLoad; `kCallBudget` stays for the
  bounded calls. Re-converted copies on the tablet: Nutty Fluffies now runs JNI_OnLoad **147,666,044 steps**
  (was stopped at 4,000,000) and fails on a real unmapped read at 0xc0d900b3; Angry Birds Epic's JNI_OnLoad
  returns in 35 steps and its boot completes, naming 10 unanswered `libunity.so` imports (6 × ASensor* — C29).
  So the budget was hiding one fault, not unlocking four games. `docs/compatibility.md` rows 3 and 28 updated.
- **C16** (`2dcf3df`): a disabled primary button's label was dark-on-near-black; the text colour is now a state
  list (TEXT_3 when disabled). Seen on the tablet.
- Gates after all of it: engine lanes 25/25 exit 0 (re-run after the engine change); host Java 16 suites exit 0.
- Device left as found: the two test tiles removed, `broken2.apk` deleted from Download, no `.part` left. The
  re-converted Nutty Fluffies and Angry Birds Epic copies stay installed (they are RetroBug copies, and the new
  engine is what makes their reports current).
- Still open, in the order I would take them: C31's tile message (4 NativeActivity games), C29/C30 (the sensor
  thunks Angry Birds Epic now names), C26 (Mono signals — the Unity milestone), C17/C18/C20/C21/C23 polish,
  then A6/A7 and the merge/push.

## 2026-10-18, later — C31's cheap half: a NativeActivity game says so (0.1.236)

- **What was wrong.** Four of the thirty M5 games died with `undefined symbol: ANativeActivity_onCreate` before a
  frame was drawn. Nothing in the library said so, so the four looked like engine faults.
- **What decides it, measured rather than assumed.** The platform's `NativeActivity.onCreate` is what dlopen()s the
  game's library and dlsym()s that entry point, so the question is whether the *launcher activity* descends from
  `android.app.NativeActivity`. All four declare a Java subclass, not the platform class:
  `com.prime31.UnityPlayerNativeActivity` → `com.unity3d.player.UnityPlayerNativeActivity` → `NativeActivity`
  (dexdump on Derby Quest's classes.dex). None declares `android.app.lib_name`, so the platform falls back to
  `libmain.so` — which is 33 KB and exports `ANativeActivity_onCreate` and `JNI_OnLoad` and no `Java_*` at all.
- **The export alone is not the answer.** MadTruck's `libmain.so` exports the same entry point and MadTruck runs,
  because its launcher activity is `com.unity3d.player.UnityPlayerActivity`, a plain Java class that never calls it.
  Measured across ten Unity games on the tablet: five export it (Derby, FC All-Stars, LEGO, Torture, MadTruck),
  five do not (Angry Birds Epic, Cars 3, Teeny Titans 2, Gumball Party, Among Us). The export is asked first only
  as a filter — 30 KB against a DEX's megabytes, and the 30-game library holds 180 MB of DEX, so the class chain
  would have cost ~1 s per rescan on its own. On the SM-X710 the filter admits five and the chain picks the four.
- **What changed** (`b4fbdba`): `DexScanner.superclasses` (class_defs → descriptor/superclass map),
  `ApkScan.isNativeActivity` (the ELF filter, then the chain across classes*.dex), `StubElfWriter.definedFunctions`
  (`jniExportsOf` is now its filter), `LibraryRules.NATIVE_ACTIVITY_WHY`, and `GameCatalog` asking the question for
  an installed game and again for an APK the player added. The four tiles read CAN'T PLAY HERE.
- **The first wording did not fit.** "…Nothing is wrong with the game or the APK" was ellipsized at "Nothing is
  wr…" in the tap's toast, which cut exactly the reassurance the change exists for. Shortened to two lines:
  "RetroBug can't run this game yet: it starts from native code (a NativeActivity). The game and its APK are fine."
- **Verified on the tablet** (0.1.236): the four tiles red and the other 26 untouched, the toast complete, the game
  menu's action button disabled for them (so nothing tries to convert one), and Delete still offers the copy. The
  first row's badges look staggered in a screenshot — that is SpongeBob's two-line name pushing its own badge down,
  not a ghost (uiautomator bounds confirm one badge per tile).
- Host tests: `ApkScanTest` 24 checks (9 new, over a d8 hierarchy fixture and two real 32-bit ELFs — one with the
  entry point, one without). Suite 16 of 16, 0 failures. The engine is untouched, so its lanes were not re-run.
- Still open in C31: running such a game needs ANativeActivityCallbacks and the guest-side activity loop; a game
  that names its entry library through `android.app.lib_name` is not recognised; and an APK added for a game that
  is not installed has no launcher class to ask about (it converts and fails at launch, as before).

## 2026-10-18, Unity step 1 — what a Unity boot dies of, and the null calls that named nothing

- **The question.** Unity is 0 of 14 games reaching gameplay. C26 (no signals delivered) was the standing
  explanation. Measuring first was worth it: it is not the explanation for these deaths.
- **What the M5 reports actually say.** Eight Unity games' boot tallies are identical: `rt_sigaction x3` on signals
  39, 36 and 38, and **no `tgkill` at all** — Mono installs its suspend/restart/abort handlers and never gets as far
  as suspending a thread. So nothing dies *because* a signal was dropped: the games stop before the first GC
  stop-the-world, or hang before one. C26 stays a real milestone (a stop-the-world will need it), but it is not
  step one.
- **Angry Birds Epic is IL2CPP, not Mono** (`libil2cpp.so`, 370 constructors, boot completes). It gets a real GL
  context — Unity logs the Adreno extension list through `guest log: D/Unity:` — and then `nativeRender()Z` stops on
  "instruction fetch failed at 0x0". The message named no symbol, and neither did anything else.
- **The stop report can now name things.** A fatal stop's message carries pc, lr, sp and r0..r12, pc and lr named
  against the loaded modules, and the first KiB of the stopped thread's stack scanned for words that land inside a
  module (stated as a scan, not an unwind: there is no CFI walker here). Its first use on Angry Birds Epic:
  `lr = libunity.so+0x597b3c | stack: libunity.so+0x56e010, libmain.so+0x206f, libunity.so+0x5781a4,
  libunity.so+0x10f1c2c, libmain.so+0x206f, …` — and `llvm-objdump` at that offset is `ldr lr, [r4, #0xcc]; blx lr`,
  so the game calls a function pointer at `+0xcc` in the object at r4 and it is null. That is a table Unity's own
  platform layer fills, not a syscall and not a thunk gap; it is the next thing to chase for this game.
- **The fix that did land: weak imports nothing answers.** Scanning every 32-bit library in all 30 M5 games for
  *weak undefined* symbols — the ones the loader must bind to 0, so a call through one is a jump to 0 that names
  nothing — found `__google_potentially_blocking_region_begin` / `_end` in **14 libraries across 9 Unity games**,
  `libunity.so` and `libil2cpp.so` among them. They are ART's "this thread may block" hooks; there is no ART monitor
  here watching a guest's JNI calls, so they answer as the no-ops they are to a guest. Measured effect: Angry Birds
  Epic's `libunity.so` unanswered-import line went from 12 names to none.
- **The same scan finished C29's sensor half.** The names libunity imports *strongly* were the sensor and key
  getters: `ASensorManager_getSensorList`, `ASensor_getType`/`Name`/`Vendor`/`Resolution`/`MinDelay`,
  `ASensorEventQueue_hasEvents`, `ASensorManager_destroyEventQueue`, `AKeyEvent_getAction`/`getMetaState`. All are
  answered from the device now: the sensor list is copied into a guest array of handles (the host's array is host
  pointers, the guest can only hold 4-byte handles), the two string getters copy into guest memory like
  `glGetString`, and the integer getters go through the generic caller.
- **Tooling, discovered the hard way and worth writing down.** The engine is baked into every *converted* game's
  APK (`lib/arm64-v8a/librbguest.so`, written by `ConvertJob` out of the launcher's own APK). Rebuilding and
  reinstalling the launcher changes nothing for a game that is already converted — the first re-run of Angry Birds
  Epic after the fix produced the identical crash because the game was still carrying the old engine. The loop that
  works, and needs no UI: push the original APK into the launcher's external files dir, then
  `am start -n com.retrobug.launcher/.LauncherActivity -e stage <f> -e repack <f> -e install 0 -e settings 1
  -e rename .rb` (**force-stop the launcher first** — the extras are read in `onCreate`, and a running instance only
  gets `onNewIntent`), pull `files/repack/<label>.rb.apk`, `adb install -r`. ~4 minutes a game.
- Gates: engine lanes 17 suites + dynarmic's 200,561 assertions, 0 failures; both advisory lanes green.
- Still true, and stated plainly: no Unity game draws a frame yet. What changed is that the failures are now named
  — module + offset, with a call chain — instead of "address 0".

## 2026-10-19, Unity step 2 — `tkill` is not `tgkill`, and C26 is step one after all

- **What was wrong with step 1's conclusion.** Step 1 scanned eight Unity boots for `tgkill` (268), found none, and
  concluded that no signal is ever sent — so C26 (no signal delivery) could not be what kills a Unity game. The scan
  was right and the conclusion was wrong: **Mono suspends a thread with `tkill` (238)**, a different number, and a
  scan for 268 alone reads a process that never signals. This is the most expensive kind of error — a true measurement
  of the wrong symbol — and it is why C26 was parked.
- **How it surfaced.** Cars 3's death path named three refusals in its last two microseconds: `svc #264`
  (`clock_getres`), `svc #238` (`tkill`), `svc #363` (`rt_tgsigqueueinfo`), then `exit_group(127)`. The numbers were
  checked against the NDK's own `arm-linux-androideabi/asm/unistd-eabi.h`, not from memory.
- **C34 implemented** (`36ce9ac`): `clock_getres`, `uname` (machine corrected to `armv7l` — the guest is ARM32 and
  `aarch64` answers a question it did not ask), `sysinfo`, `sched_get/setaffinity`, `prlimit64`, and the scheduler
  pair `sched_getparam`/`sched_getscheduler`/`sched_get_priority_min`/`max` plus `getpriority`/`setpriority`. The
  scheduler set is *measured*, not assumed: once the signal refusals stopped ending Cars 3, it asked 155/156/159/160
  in a tight loop. Each answers with the host's real fact, field by field where `long` differs across the ABI pair.
- **`tkill`, `tgkill` and `rt_tgsigqueueinfo` are now one case.** They are the same operation with shifted arguments.
  Signal 0 stays the existence probe; a real signal is still refused by name — telling a guest "delivered" when
  nothing ran its handler turns a crash it could report into a process that dies saying nothing.
- **Measured effect on Cars 3: it no longer dies on an unimplemented call at all.** Every syscall refusal in its run
  is gone (was a stream of 241/156/155/160/116/264/238/363). It now reaches the real wall and the log says what it
  is: `tkill(tid, 30)` — **signal 30 is SIGPWR**, Mono's GC thread-suspend — and when delivery is refused the runtime
  aborts (`rt_tgsigqueueinfo(..., 6, ...)`, SIGABRT) and calls `exit(127)`.
- **MadTruck confirms it independently.** It boots to its own dialog, is alive, and dies the same way on the same two
  calls ~7 minutes in. Two games, one wall, both named. Step 1's "the signal handlers install and no signal is sent"
  reading is retired.
- So: **C26 is step one.** Not because a stop-the-world was assumed, but because two games now stop at exactly one.
  The work is signal *delivery* — a guest thread must take SIGPWR into the handler it installed at 39/36/38 — and it
  is the largest single piece left in the engine.
- Gates: engine lanes 25/25 exit 0 on the tablet (dynarmic 200,561 assertions in 47 cases), re-run after each change.
- Build note for whoever is next: the engine is baked into every converted game's APK, so a fix needs the game
  re-repacked (the step-1 loop above). `cars.apk` on the tablet is **Gameloft** Cars, not Disney Cars 3 — the Disney
  original is `build/m5/com.disney.cars3_goo/orig.apk`. Also: under this session's file sandbox, msys `bash` and
  `ninja` cannot create their signal pipes, so a build needs the escalation path; the SDK's own `cmake\ninja.exe`
  drives `build/launcher-arm64` directly and worked.

