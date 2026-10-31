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

## 2026-10-19, OnePlus acceptance — the 64-bit-only target, first run since M4

- **Why it mattered.** The OnePlus CPH2573 (`75ff8cd3`) is the only genuinely 64-bit-only device here
  (`ro.product.cpu.abilist32` is empty; the Tab S9 is 32-bit-capable). It still ran versionCode 1 / versionName 1
  from 2026-10-18 03:01 — it had never seen M5, M6 or anything after. Every acceptance claim since M4 was made on a
  device that *can* run 32-bit code natively, which is not the target this project exists for.
- **Installed 0.1.238** (the HEAD build at the time; `adb install -r -g` worked, no ColorOS silent-drop this time).
  Launcher opens clean: all four tiles render with icons and badges, no exception in logcat.
- **FN Skittles (`com.halfbrick.fruitninjavsskittles.rb`), launched by its icon:** engine boot reports
  `constructors: 483 ran on Tier J`, `JNI_OnLoad returned 0x10004`, and **`RESULT: startup finished with no fault`**.
  The permission prompt (C23 — the shim asks for every dangerous permission at once) appeared; once answered the
  game reached its menu, rendered correctly, and played. **yebot's own verdict, a non-developer who plays it on
  32-bit hardware: he "couldn't tell a difference compared to playing it on a real 32 bit device".** That is the
  strongest acceptance signal this project has produced, and it is a real-time game under emulation.
- This closes the previous handoff's item 1 ("install 0.1.236 on the OnePlus and re-run the icon-launch
  acceptance") and the older "nothing has been proven on the 64-bit-only target since M4".
- Left as found: no PIN entered, no prompt tapped on the human's behalf beyond what he answered himself.

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


## 2026-10-20, C26 — signal delivery lands, and the frame wrote 24 bytes too far

- **What was done.** The uncommitted C26 work from the 01:42 session was finished and device-verified:
  `rt_sigaction` records the guest's `{handler, flags, restorer, mask}`; `tkill`/`tgkill`/`rt_tgsigqueueinfo`
  queue a real signal for a live tid instead of refusing; delivery happens at the ABI's slice boundary (every
  200,000 instructions, every syscall, and the first instruction of every turn — so clone()d threads, adopted
  host threads and the main thread all get it); the measured Aarch32 `rt_sigframe` is built on the guest stack;
  `rt_sigreturn` restores the interrupted register file from the frame; a futex or nanosleep wait for the
  target returns `EINTR` first; and deliveries nest (Mono's SIGPWR suspend handler waits for the SIGXCPU
  restart signal to run *inside* it). Two lanes were added: 614 checks in `rb_guest_thread_tests`, including
  a blocked-futex interrupt and the nested stack.
- **Device evidence, Cars 3 (`com.disney.cars3_goo.rb`).** `tkill(4246, 30)` and then `tkill(4246, 24)` are
  queued and delivered into Mono's handler at `libmono.so+0x2bdb59`; the restart signal nests inside the
  suspend handler; both `rt_sigreturn`s restore; the thread resumes inside libc's `syscall()` wrapper and the
  game keeps running at ~115 M guest instr/s. The SIGPWR/SIGXCPU pairs then repeat on two threads (4246 and
  the adopted 4286) — Mono's stop-the-world is working. This is the wall from 2026-10-19 gone.
- **The bug that made it visible, and the one it found.** The first game run after the signal fix delivered
  both signals and then died on a null read at `libc+0x78468` (`__futex_wait_ex`'s error path, `ldr r0,[r4]`
  with r4=0). The register dump (now in every fatal thread stop) showed the *interrupted* state already had
  r4=0, on the instruction after `svc` inside libc's generic `syscall()` wrapper. The cause was in the frame
  writer: the zeroing loop wrote 64-byte chunks for a 872-byte frame, so its last chunk ran 24 bytes past the
  frame — and the 16 at `SP..SP+16` are the interrupted `syscall()` wrapper's saved `r4..r7`. The `pop`
  restored zeros, and bionic read through the null errno pointer. The loop now shortens its last chunk, and
  the delivery test leaves a sentinel at `SP..SP+16` and fails if a byte there changes.
- **Three more defects the lanes caught.** `uc_sigmask` held the handler's mask instead of the interrupted
  mask (the signal stayed blocked after the first delivery); `rt_sigreturn`'s outcome r0 overwrote the
  restored r0 (the futex returned 0 instead of -EINTR); and the test fixture's `pc` was Thumb-tagged while
  `mode` said Thumb, which this engine's front end rejects — the frame stores a clean pc and CPSRs mode bit.
- **Gates.** 23/23 engine lanes, 0 failures on the SM-X710; `rb_guest_thread_tests` 614 checks (was 563).
  The full suite was re-run after the last frame change. `/data/local/tmp` fixtures unchanged.
- **What is left, stated plainly.** Cars 3 no longer dies of signals but still presents a **black screen**.
  Its first ~8 seconds are real Unity startup — the GL extension list, `OnLevelWasLoaded` on LoadingFader,
  Crittercism init — at 115 M guest instr/s; then the guest stops executing entirely and all 74 host threads
  are asleep (last profiler line 04:17:09, no further RBRUNTIME output; evidence
  `build/m5/com.disney.cars3_goo/c26f-runtime.log`). So the next wall is a **hang once the game's Java/SDK
  side starts**, not a rendering loop and not C26. MadTruck boots to its own Information dialog and stays
  idle there (0 M instr/s); its previous ~7-minute death needs interaction to reach and was not reproduced,
  so MadTruck's signal path is unverified beyond "boots and does not die". No Unity game has drawn a frame yet.
- **Tooling.** The build tree is still driven by the SDK's own `cmake/ninja.exe` directly (the sandbox-pipe
  note in the handoff stands); the launcher APK is packaged with `build/build_launcher_noclean.sh`. The
  repack loop is the one in the handoff: stage the original in the launcher's external files dir, force-stop,
  `am start ... -e stage <f> -e repack <name> -e install 0 -e settings 1 -e rename 1`, pull
  `files/repack/<label>.rb.apk`, `adb install -r`.

## 2026-10-20, after C26 — VPhoneGaga, a liveness supervisor, and MadTruck's ALooper wall

- **VPhoneGaga 5.0.6 was analysed, not copied.** It is a whole virtual 32-bit Android phone: a downloaded
  userspace (bionic + ART + zygote + system_server) inside a user-space kernel, with **QEMU linux-user TCG**
  translating ARM32 (`libtranslator.so`: `accel/tcg`, `target/arm/tcg`) and Box64/FEX for the Wine/x86 path.
  `libuserkernel32.so`/`libloader32.so` are ELF32 guest blobs; the 32-bit image is a first-run download
  (`androidfs_*.7z`, `block.img`), not in the APK. Nothing is copyable into an Apache-2.0 clean-room tree —
  QEMU is GPL-2.0, emugl is GPL lineage, Wine LGPL, OpenSSL 1.1.1 Apache-incompatible. The transferable
  lessons are architectural: a **liveness supervisor** (its `shell_process`/`notify_rendered`), a
  **no-unbounded-wait contract** for virtualized services, and pollable pseudo-devices. The full 32-bit
  image is a 6–12 month alternative and contradicts AD-3; not recommended next.
- **The liveness supervisor landed** (uncommitted): `GuestThread` carries wait markers (futex/sleep/host
  held/host released, slot, last syscall); the launcher's profiler now says `stall: no guest instruction
  retired in the last 10 s` and dumps one line per guest thread with pc/lr/sp named against modules, the
  futex word, a stack scan for module-shaped return addresses, and the futex table's counters. That turned
  "black screen" into a named thread and a call chain. Tests: the futex lane checks the marker.
- **Cars 3 is not signals and not Crittercism.** The native original (32-bit) reaches MainMenu on the same
  Tab S9 in ~18 s, so the game and its dead backend are not the cause. JDWP showed the Java side healthy
  (main idle in the Looper, UnityMain inside `nativeRender`); the stall report names **adopted tid 4245**
  parked in a libunity `pthread_cond_wait` (refcounted-object wait). Cecil-injected `RBTRACE` logs narrowed
  the managed side to the Unity **scene activation / UI OnEnable** phase (UIBehaviour, MaskableGraphic,
  Graphic), and emptying `Crittercism.Awake`/`CrittercismAndroid.Init` did **not** move the stall. Next
  tool: an EHABI unwind of the guest stack (the scan is not enough) or a UI-asset-level comparison.
- **MadTruck's next wall was a missing thunk, and is fixed.** With C26 it reached `nativeRender` and died on
  `ALooper_pollAll` (unanswered import, trap 0xfffe0230). `thunks_android.cpp` now answers
  `ALooper_pollAll`/`pollOnce` with `ALOOPER_POLL_TIMEOUT` (-3) (the engine has no guest input queue on a
  looper; polling the host looper would consume the host's events and could block forever) and `ALooper_wake`
  as a no-op. MadTruck now renders the Spil Games splash and runs at 60–127 M guest instr/s. Its new wall is
  a **billing/semaphore spin**: UnityMain at 100% CPU repeats "Failed to wait on a semaphore (Interrupted
  system call)" after the Android Native billing plugin's `AN_Connect`/`AN_RetrieveProducDetails`; no crash.
- **Teeny Titans 2 (Unity 2018.4, IL2CPP) now renders its title screen** — it previously died with
  `exit(127)`, the same signal wall. Its stall report shows the main thread at the return sentinel with
  libil2cpp stack words and the adopted UI thread in a libunity `pthread_cond_wait`; 31 workers wait on one
  pool futex. It is frozen on the title screen, not crash-looping, and that is the next named wall.
- **The futex interrupt now respects the target's blocked mask.** `Monitor::QueueSignal` asks the runtime for
  the target `GuestThread` and only wakes a futex wait if the signal is not blocked there, as the kernel
  does. MadTruck's repeated semaphore EINTRs are from unblocked SIGPWR/SIGXCPU (kernel-correct), but a
  blocked signal can no longer manufacture a spurious EINTR.
- Compatibility summary after the session: Blocky Roads / Sonic Transformed / Gumball Lite still run;
  Teeny Titans 2 and MadTruck now render their first screens; Cars 3 still freezes in UI; Angry Birds Epic's
  null table entry and the NativeActivity titles are unchanged. No new Unity game reaches gameplay yet.
- Gates: 23 engine suites green on the SM-X710; `rb_guest_thread_tests` 618 checks (the marker check now
  waits for the marker; its first form raced the enqueue). The diagnostic C# patches and decompiled trees
  live under `build/` (ignored) and are not shipped; the installed Cars 3 was restored to the clean C26
  build, the installed MadTruck/launcher carry the ALooper thunks.

### Pre-commit review hardening (2026-10-20, same session)

- An independent reviewer audited the staged diff. Applied: the liveness sampler is now owned and **joined**
  (never detached) so a failed scope boot cannot leave it reading a freed GuestProcess; `rt_sigaction`
  answers a query from the recorded disposition (and reports EFAULT instead of installing a default);
  the sigaction's **sa_mask** is ORed into the handler mask; the interrupted **VFP file** is saved in the
  delivery record and restored on `rt_sigreturn` (the kernel's VFP sigcontext block is still not modelled —
  a named gap); a sleeper registers **before** the GGL is released (tkill can no longer miss it); standard
  pending signals coalesce (a tkill loop cannot grow the queue); a signal number outside 1..64 is EINVAL;
  the frame's page precheck starts at the containing page; the handler's CPSR has its IT bits cleared; and
  two stale no-signal comments are corrected.
- One reviewer finding was rejected with evidence: `adopted_` is **not** raced — `AdoptCurrentHostThread`
  takes the GGL at entry and `RetireAdopted` takes it before touching the list; `StallReport`'s deliberate
  no-GGL read is a stall-only diagnostic.
- New lanes: a blocked signal must leave a futex wait parked (lookup + mask), and a sigaction query returns
  the previous handler without erasing it. Gate: 23/23 suites; `rb_syscall_monitor_tests` 216 checks,
  `rb_guest_thread_tests` 646, `rb_exec_tier_diff_tests` 1011; launcher APK packages.

## 2026-10-20/21 — `rt_sigsuspend` was the missing call, and MadTruck runs (3D Unity)

- **The gap.** Cars 3's runtime log had `svc #179 is not implemented; refusing with ENOSYS` at the edge of the
  Mono GC. 179 is `rt_sigsuspend` (NDK `arm-linux-androideabi/asm/unistd-eabi.h`), and it is exactly what
  Mono's suspend handler calls while it waits for its restart signal. Without it the SIGPWR handler could not
  park, the stop-the-world handshake did not settle, and the game stopped making progress.
- **What landed.** The monitor answers `rt_sigsuspend(mask, 8)`: it installs the temporary mask, registers a
  signal wait under the GGL, blocks until a deliverable signal is queued, and returns `-EINTR`; the signal stays
  pending and is entered at the next ABI boundary. `ReturnFromSignal` puts the pre-suspend mask back after the
  handler's `rt_sigreturn` (the kernel's order: the handler runs under the temporary mask). FutexTable gained
  `BeginSignalWait`/`WaitForSignal`, woken by `Interrupt` only for signals the target has not blocked.
- **MadTruck now runs — the first 3D Unity game to reach gameplay.** After the mod's own Information dialog it
  renders the Spil splash, the **main menu**, the **Egypt level map**, the **GOALS screen**, and the **level 1
  race** (3D track, HUD, tilt/rocket controls), and the tutorial dismisses and the controls respond. Unity still
  logs non-fatal "Failed to wait on a semaphore (Interrupted system call)" during GC and a `fork` ENOSYS from its
  crash reporter; neither stops the game. Evidence: `build/m5/com.spilgames.mtc/mtc-sig2.png` (menu),
  `mtc-play.png` (map), `mtc-level1.png` (goals), `mtc-race.png`/`mtc-race2.png` (race).
- **What did not move.** Cars 3 still freezes in Unity scene activation/UI (the adopted UnityMain thread in a
  libunity `pthread_cond_wait`) before it reaches a sigsuspend; the native original reaches MainMenu, so this is
  a remaining engine gap (C37). Teeny Titans 2 (IL2CPP) renders its title screen and freezes (C40).
- Gate: 23/23 engine suites on the SM-X710; `rb_syscall_monitor_tests` 223 checks (sigsuspend lane),
  `rb_guest_thread_tests` 646, tier-diff 1011.

## 2026-10-20/21 — remaining Unity games re-tested

- **Crash Drive 2** (Unity 2017.4 IL2CPP, 3.51) with C26 + ALooper + `rt_sigsuspend`: boot reaches the GL list,
  then `guest thread tid 4249 stopped: instruction fetch failed at 0x0` with pc 0x4, lr in libunity. It is the
  same null platform-table call recorded on 2026-10-18, not a signal-path wall; the signal fixes do not touch it.
- **Teeny Titans 2** with `rt_sigsuspend` again: instead of freezing at the title it now reaches its own
  SIGABRT handler during boot — Unity's `E/CRASH` dump (signal 6, pc in libc `abort`, the dumped stack filled
  with 0xff) — and exits. The crash is finally visible; the abort's caller cannot be recovered from the dump,
  and the stack pattern suggests the abort frame is not where the handler looked. Open as C40.
- **What remains, named:** C37 Cars 3 (Unity UI/libunity condvar; native original reaches MainMenu), C40 TT2
  (IL2CPP early SIGABRT with a lost caller), C41 the null platform-table call shared by Crash Drive 2 and
  Angry Birds Epic (pc 0x4 after `blx` through a null table entry), C31 NativeActivity titles, Rolling Sky's
  host-ART `NoSuchFieldError`, and Gumball's DRM `libgvradio` fault.
- **Working now:** MadTruck (Unity Mono, 3D menu/map/race), plus the previously working native titles
  (Blocky Roads, Sonic Transformed, Gumball Lite, KitKat, FN Skittles, etc.).

### Loader: data symbols are not function traps

- `data_start`/`__data_start` and `_end`/`__end__`/`_edata` are linker-script **data** symbols. The
  unresolved-import fallback bound them to per-name **function trap slots** — a code address where a data
  range was asked for. Boehm (Unity IL2CPP) and Mono use the pair as the bounds of the data segment they
  scan, so a trap value there is a wrong fact at both ends.
- `GuestModule` now records the first and last writable PT_LOAD; the loader answers those names with
  `data_start`/`data_end` before the unresolved-import path. TT2's `libil2cpp.so` unanswered list is now
  empty (only libmain's three `__cxa_*` remain), and Crash Drive 2's is clean too.
- It did **not** stop TT2's early SIGABRT (C40) or Crash Drive 2's null platform-table call (C41); both
  remain open, now with the data-range explanation ruled out. Gate: 23/23; guest libc 147 checks.

## 2026-10-21 — C40–C44: the tgkill decode, and three more engine gaps

- **`tgkill` was decoded as `tkill`.** The monitor read `tid = arg0, sig = arg1` for both, but the ABI is
  `tkill(tid, sig)` and `tgkill(tgid, tid, sig)` / `rt_tgsigqueueinfo(tgid, tid, sig, info)`. So
  `tgkill(4242, 4246, 30)` was logged as `tgkill(4242, 4242, 4246)` and refused EINVAL — the tid was read
  as the signal. Boehm's `GC_suspend_all` aborts on any `pthread_kill` error outside {0, ESRCH}, which is
  exactly what happened in TT2 and CD2 (the guest-stderr capture added first printed
  "pthread_kill failed at suspend"). Statically confirmed in both libil2cpp copies: `GC_sig_suspend` is
  `.data 0x1e` (SIGPWR=30), statically initialised, and the abort arm is `mov lr,pc; b abort`. Fix:
  `tkill` keeps `(arg0,arg1)`, the other two use `(arg1,arg2)`; new monitor lane
  `the_three_argument_signal_calls_read_the_signal_from_r2` (233 checks, +10).
- **Device effect:** TT2 boots to its Robin title (`tt2-after-tgkill.png`) instead of SIGABRTing;
  CD2 boots through Unity 2017.4.32f1, the SPIL GDPR dialog, IAP and Swrve, and reaches its loading screen.
- **C41: the null GL entry point.** The pc-0x4 fault was `blx` through a slot Unity filled from
  `eglGetProcAddress`: the name was `glTexImage3DOES` (CD2 `libunity+0x268`, loaded by the sequence at
  `+0x709160`; ABE stores its own OES entries at `+0x25c/+0x264`). The host thunk table listed only the
  core `glTexImage3D`, so `HostSlots::Resolve` answered NULL and `strne` left the slot zero. Added
  `glTexImage3DOES`/`glTexSubImage3DOES` with the core signatures to `kGl3`; the device lane
  (`gl_thunk_test`) checks the table, `HostSymbol` and a real 3D upload through the alias (22 checks).
  Device: CD2 reaches the consent dialog (`cd2-after-tgkill.png`, `cd2-menu.png`), ABE reaches the
  publisher splash (`abe-after-c41.png`), Rolling Sky reaches the Cheetah Mobile splash
  (`rs-after-c41b.png`) after its DEX patch. None reaches gameplay; all wait in C46.
- **C43: synchronous guest faults become SIGSEGV.** `RunResult` now carries `fault_addr/has_addr/write`
  from both tiers (`SetFault`, `FaultAt`); `AbiContext::set_fault_deliver` offers a named-address fault to
  the runtime, which builds the frame with `si_code` (SEGV_MAPERR read/fetch, SEGV_ACCERR write), `si_addr`
  and `sigcontext.fault_address`, and resumes at the handler. A fault with SIG_DFL, a blocked SIGSEGV, one
  inside the handler, or a full delivery stack stays fatal — the kernel's default action. Device: Cars 3's
  Mono JIT null now becomes a handled `NullReferenceException` ("Object reference not set to an instance of
  an object") and the game keeps running. Lane:
  `a_synchronous_fault_reaches_the_guest_sigsegv_handler` for both tiers (guest thread 692 checks).
- **C44: VADDHN/VRADDHN/VSUBHN/VRSUBHN were not decoded.** Dynarmic's A32 ASIMD table had them commented
  out; Cars 3's libunity image path executes `vaddhn.i16` (`f2c234a4` at `libunity+0x64f2e4`) and the front
  end stopped it as "unpredictable instruction", ending the process. Added the four entries (patterns
  `1111001U1Dss...0100/0110...`), `HighNarrowingInstruction` in the A32 translator, and Tier I
  `VectorLogicalShiftRight8/16/32/64` + `VectorNarrow16/32/64`. Tier-diff lane
  `the_high_narrowing_vector_ops_narrow_like_hardware` uses d2 (the low half of source q1) as the
  destination, so a write-before-read implementation fails; 1039 checks.
- **Cars 3, patched build:** with the NOP'd libunity it runs at ~64 M guest instr/s, handles the Swrve 403s
  and the managed null, and stays on a black screen — the NOP removes a real startup barrier, so this is
  not the fix. The unpatched original still waits in it.
- **What remains:** **C46**, the shared Unity startup condvar. CD2 (loading), TT2 (title), ABE (splash),
  Rolling Sky (splash) and Cars 3 (unpatched) all show an adopted UnityMain thread in
  `libunity+0x60e650`-family `pthread_cond_wait`/futex with every worker idle. It is the same family as
  C37, now with four independent witnesses and a named next probe (identify the `{flag, refcount}` object
  at `+0x20`, its setter, and whether a JNI/Choreographer callback is missing).
- Gate: 23/23 suites on the SM-X710 — tier-diff 1039, guest thread 692, monitor 233, gl thunk 22; advisory
  guest libc 147, guest APK 1376. Build `build/gate-c44.log`.

## 2026-10-24 — C46 is not a deadlock; the guest keeps rendering. The real wait is il2cpp-side
- **The stall detector was lying, and it cost this session its first hours.** TT2's full dump said "no
  guest instruction retired in the last 10 s" for ten minutes while the guest was calling
  `eglSwapBuffers` ~35 times a second. `AbiContext::slices()` only moves when a thread retires a whole
  200k-instruction stint; Unity's render loop calls JNI/GL thunks every few thousand instructions, so it
  never reaches one. **Fixed:** `AbiContext::host_activity()` counts syscalls + host-slot dispatches, and
  the profiler now prints `alive: no slice in 10 s, N host call(s) so far — sub-slice stints, not a
  stall` instead of a stall report while that counter moves. The `stall:` path is now true quiescence.
- **What TT2 actually does after the privacy/terms cache** (engine reproducibility: 3 consecutive runs):
  the *screen* is black, but the guest is not idle — `eglSwapBuffers` ~35/s, `glClear` 2/frame,
  `glDrawElements` ~160/s, `glBindTexture` ~270/s. SurfaceFlinger's latency query for the guest's
  SurfaceView layer shows a frame every ~33 ms, so the frames reach the compositor; the content is black,
  not lost. The guest's own Unity log stops at `cached new terms` and never advances.
- **The parked thread is not the one C46 named.** Clone tid 4245 `UnityMain` waits at futex
  `0xff2f52c0` (word 0x4) that is **never woken** (0 wakes for that address all run). Its stack is
  libil2cpp: `+0x35e830 <- libc.so+0x81927 (pthread_cond_wait) <- +0x46d17c <- +0x470840 <- +0x46d17c
  <- +0x46d208` (the last is a polling loop over a byte flag). The earlier C37/C46 note named a
  libunity `0x60e650`-family wait; for TT2 the live wait is an il2cpp one. The object around the wait:
  `0xabababab 0xabababab 0x19 0x9 0xff2bf410 0x0 0x3 0x0` (dump from addr-0x20).
- **Thread creation is over after the first 4 s:** 31 clones, every one from tid 4244 `UnityMain` with
  lr 0x18f1e9, then no `clone` for the rest of the process (new per-clone trace line in
  `Monitor::Clone`). The UnityChoreographer thread is **not** a guest clone at all: host tid named
  `UnityChoreograp` is a Java/ART thread, adopted as guest 4277.
- **The Java side is healthy.** New `ArtJava::DumpThreads()` (a `Thread.getAllStackTraces()` dump through
  our own JNI bridge — `kill -3` from a non-debuggable app dumps nothing) shows: `UnityChoreographer` is
  inside `Choreographer.doFrame -> $Proxy11.doFrame -> bitter.jnibridge.JNIBridge.invoke (native)`, i.e.
  VSYNC callbacks arrive and enter guest code; `UnityMain` (Java) is inside
  `UnityPlayer.nativeRender`. A bounded JNI trace (300 lines on the first real stall) shows the pump
  calling `longValue()J` and `postFrameCallback(...)` ~30 times a second. So the VSYNC pump runs; it just
  never releases whatever 4245 waits on.
- **A real emulation defect found on the way:** `Process.setThreadPriority(4246)` throws
  `IllegalArgumentException: Given thread 4246 does not exist` from `UnityPlayer.nativeRender` — Unity
  passes a **guest** tid to a Java API that resolves host tids. 135 occurrences in the first second, then
  none (per-thread `jni_pending` counter). Not proven causal for C46, but every such call fails, and the
  fix is a tid translation at the bridge (guest tid -> bound host tid) for the thread APIs.
- **Also verified:** the original 32-bit APK (`com.turner.ttgfigures2`) runs natively on this SM-X710 —
  built-up gameplay with a PAUSED overlay. Any "TT2 reached its title" screenshot must therefore be
  attributed carefully: package `.rb` vs original. Today's guest builds consistently render black at the
  same point; the earlier title-screen claim needs re-checking against the `.rb` package.
- **Diagnostics added to the launcher build** (all bounded, all in the shipped debuggable build):
  per-clone trace (parent tid/name/lr), `jni_pending` counter in the stall line, JNI call trace on the
  first stall, Java thread dump at the second stall, EGL/GL counters in the stall line, wait-object
  memory window for futex waits, and stacks for parked clones.
- **Next concrete steps:** (1) symbolise the il2cpp wait at `0xff2f52c0` — resolve `+0x46d208`'s caller
  through Il2Cpp code registration + `global-metadata.dat` (both in hand: `libil2cpp.so`,
  `assets/bin/Data/Managed/Metadata/global-metadata.dat`); (2) translate guest tids for
  `android.os.Process.setThreadPriority` (and neighbours) in `ForwardingJni`; (3) re-check whether the
  black frame content is the game's own loading screen (spinner) or an empty scene — sample the guest's
  framebuffer content rather than the composited screen.

### 2026-10-24 (later) — C46's root cause found: the Java thread APIs were given guest tids
- **The fix:** `android.os.Process.setThreadPriority(<tid>)` is called by `UnityPlayer.nativeRender` with a
  **guest** tid; ART resolves host tids and threw `IllegalArgumentException: Given thread 4246 does not
  exist` on every call (135 in the first second). The pending exception left `nativeRender` half-run and the
  engine's main thread parked in an il2cpp condvar nobody could wake. `ForwardingJni` now resolves the two
  `setThreadPriority` overloads once and rewrites the tid argument through a new
  `GuestProcess::HostTidOf()` (guest tid -> the bound host thread's Linux tid, set in
  `BindCurrentHostThread`). Unknown ids are passed through unchanged, so ART's own answer stays honest.
- **Result on the tablet (engine baked into each repack):**
  - **TT2**: past the freeze — consent screen, then the city intro scene at ~53 M guest instr/s
    (`build/m5/com.turner.ttgfigures2/tt2-tid.png`, `tt2-tid2..6.png`).
  - **CD2**: past the GDPR dialog to the Google Play Games profile picker (cancelled — no account action),
    its own rating dialog, then **the main menu and the "Welcome Back / enter your name" onboarding**
    (`build/m5/com.spilgames.a10.google.crashdrive2/c46-cd2e.png`, `c46-cd2-menu.png`).
  - **ABE**: publisher/title screen with its 3D background and its own data-consent dialog
    (`build/m5/com.rovio.gold/c46-abe.png`). Taps did not dismiss that dialog in this run (input path for
    this title still to check).
  - **Rolling Sky**: no longer stalls — it now **crashes in `AppActivity.onCreate`** with
    `SecurityException: opening provider com.turbochilli.rollingsky.ipc.IpcProvider ... not exported from
    UID 10576`: the `.rb` copy calls the *original* install's provider by its hard-coded authority. That is
    a packaging/provider issue (new backlog item C47), not C46.
- `IllegalArgumentException` count on TT2 after the fix: **3** (down from 135) — the remainder are tids the
  clone table does not hold when the call is made (adopted threads), left for a follow-up.

### 2026-10-24 (later still) — C47 fixed: the repack now moves authority literals with the manifest
- **Root cause (sub-agent-verified):** Rolling Sky's `IpcProvider.<clinit>` parses
  `content://com.turbochilli.rollingsky.ipc_provider` (a literal in `classes2.dex`, string_data at
  0x1c494d). The repack renames the manifest's authority but copies the DEX byte-for-byte, so the copy
  opened the **original** install's provider and got `SecurityException` (not exported, other uid).
- **Fix:** `ApkRepackager` now decides the authority rename *with the DEX in view* before writing the
  manifest. An authority the app names by string is renamed **in place**: same length, same string-table
  position (the last label's first two characters become "rb"), e.g.
  `com.turbochilli.rollingsky.ipc_provider` -> `...rollingsky.rbc_provider`; `...rollingsky.provider` ->
  `...rollingsky.rbovider`. `DexScanner.replaceStringPart` rewrites those strings and
  `refreshDigests` recomputes the DEX's SHA-1 signature and Adler-32 checksum (ART rejects a stale one).
  The patcher refuses any edit that changes a record's length or breaks `string_ids` ordering — both
  were learned the hard way: a shorter name failed verification with "String longer than indicated
  size 0", and reordering failed as well. Host lane `DexStringsTest` (11 checks) pins the contract,
  including both refusals; the full Java suite stays green.
- **Device:** `rollingsky-patched` repacks, installs and reaches its **level-select screen** (Cloud,
  0%, 0/10) — `build/m5/com.turbochilli.rollingsky/c47-rs3.png`. C47 done.
- **Cars 3 (unpatched, this run):** no longer stalls — it boots through Crittercism/Kochava/IAP/AES and
  the bundle check at ~64 M guest instr/s, but stays on black frames while Swrve answers 403 and its
  IAP list fails; frames reach SurfaceFlinger (`--latency` shows them), so the remaining wall is the
  game's online bootstrap (C37 family), not C46.
- **C48 reclassified:** ABE's consent dialog is gated on "Checking internet connection...", which never
  resolves; taps *do* reach the window (the VRI frame-rate hint fires on touch), so this is not an input
  defect. The earlier "taps don't land" note was wrong and is replaced here.
- **Adopted-thread tids:** the guest->host tid map now covers clone'd and adopted threads (bind
  publishes, unbind/exit retracts). TT2 still shows **3** `Given thread 4246 does not exist` at startup:
  tid 4246 has no bound thread when Unity asks (a late priority call for a dead worker), which ART answers
  honestly; harmless and not blocking.

### 2026-10-25 — Among Us and the native-activity map
- **Among Us (first run)**: repacks, installs, and dies before Unity with
  `UnsatisfiedLinkError: NativeLoader.load` — a *symptom*. The real stop is earlier: `libEOSSDK.so`'s
  constructor (DT_INIT_ARRAY[59] at `+0xFD4654`) reaches `+0xFD4548`, an **ARMv8-A A32 crypto**
  instruction: llvm-objdump with `--mcpu=cortex-a53` decodes the three stubs at `+0xFD4538/+0xFD4540/
  +0xFD4548` as `sha1c.32`, `sha256h.32` and `vmull.p64`. Dynarmic's A32 table has the v8 AES entries but
  not SHA1/SHA256, and its VMULL pattern swallows `vmull.p64` only to refuse it (`P & sz==0b10` is
  architecturally undefined in ARMv7). The boot then refuses the process, so `libgamedva.com.so` (the
  renamed Unity player) never loads. **New backlog item C49**: implement ARMv8-A A32 crypto
  (SHA1C/SHA256H/SHA1H/SHA1SU1/SHA256H2/…, VMULL.P64, and the AES family dynarmic already lists) in both
  tiers, with a tier-diff lane. Until then Among Us stays unplayable.
- **Gumball's Amazing Party is a different fault**: its `libgvradio.so` constructor (guest 0x24c900) jumps
  to `0xff59e476` — a stack address — and only then hits "undefined instruction". That is a bad
  function-pointer/relocation control flow in an obfuscated DRM library, not a missing opcode; note it as
  such rather than folding it into C49.
- **C31 map (sub-agent, verified)**: no `ANativeActivity`/`android_native_app_glue`/`AInputQueue` code
  exists anywhere (only detection in `ApkScan.isNativeActivity`, which marks the tile unplayable before a
  repack). The repack path never emits `ANativeActivity_onCreate` (`StubElfWriter.jniExportsOf` keeps only
  `Java_*` + `JNI_OnLoad`); `runtime_boot.cpp`'s stub dispatch only knows `JNI_OnLoad`; `ALooper_pollAll`
  answers −3. Smallest plan: export the symbol from the stub, add a `kStubOnCreate` branch that builds an
  `ANativeActivity` and calls the guest entry, then implement `ANativeActivity_*`/`AInputQueue_*`/
  `ALooper_*` semantics in `thunks_android.cpp`, and a Java `NativeActivity`-compatible host activity.

### 2026-10-25 (later) — an undefined encoding now reaches the guest's SIGILL handler
- **What changed:** an *architecturally undefined* encoding (`UndefinedInstruction`) and an UNPREDICTABLE one
  (ARM permits the undefined-instruction exception for those) are classified as a `Fault` carrying
  `illegal_instruction` + `illegal_pc`, in both tiers, instead of `Unimplemented` (which stays the label for a
  gap in *this* engine). The monitored process delivers SIGILL (si_code `ILL_ILLOPC`, si_addr = the
  instruction) to the guest's handler with the pc set to the instruction, so a probe that skips it resumes
  after it — the kernel's answer. Refusals (no handler, blocked, nested) are logged by name.
- **Evidence (Among Us, ARMv8-A crypto probes in `libEOSSDK.so`):** the first probe, `vmull.p64` at
  `libEOSSDK+0xFD4548`, is trapped and **delivered to the guest's handler 0x1c64844**; the constructor then
  moves on to the next probe, `sha1c` at `+0xFD4538`. That second trap is refused — `inside its own handler
  (deliveries 1)` — because the first delivery is never closed: the handler runs and control resumes at the
  probe it chose, yet **no `rt_sigreturn` reaches the monitor**. Until that is understood (how that handler
  returns, e.g. bionic's restorer path), Among Us still stops in the constructor and the boot refuses the
  process; C49 keeps the details.
- New engine lane: `an_undefined_encoding_reaches_the_guest_sigill_handler` runs on both tiers (handler
  installed with `SA_RESTORER`, `udf #0` victim, asserts ILL_ILLOPC/si_addr/interrupted pc and that
  `rt_sigreturn` pops the delivery). Guest-thread lane: 726 checks, 0 failures.

### 2026-10-25 (final) — SIGILL delivery takes Among Us past EOS; the next wall is libunity
- **The handler was abandoning its frame, not returning through it.** EOS's probe handler runs, chooses the
  next probe, and `siglongjmp`s back — it never issues `rt_sigreturn`, so the first delivery stayed open and
  the old "inside its own handler" guard refused the second probe. The kernel's only bookkeeping for that
  state is the **signal mask** (entering a handler blocks the signal unless `SA_NODEFER`, and `siglongjmp`
  restores the mask), so the delivery guard now reads the mask, not the delivery stack.
- **Result on Among Us:** the first probe (`vmull.p64`) and the second (`sha1c`) are both delivered,
  `libEOSSDK.so` finishes every constructor, and **`libgamedva.com.so` (the renamed Unity player) boots and
  its `JNI_OnLoad` returns**. The game then dies in its render thread:
  `fatal: guest memory read at 0xbfffffe4 (pc libc.so+0x940f0, lr libunity.so+0x724189)`, with the log's
  prior line showing `SIGSEGV at 0xbfffffe4 not delivered — blocked in the thread's mask`. That is a new,
  separate wall (a libunity-side pointer near the 32-bit stack top; ~3 GB), not the crypto probes.
- Engine lanes were re-gated after the classification change: 25/25 suites, 0 failures (guest-thread lane
  726 checks, including the new SIGILL row on both tiers).

### 2026-10-25 — release prep: 0.2.0, an original icon, and notes
- **Release build:** `RB_RELEASE=1 RB_VERSION_NAME=0.2.0 bash tools/build_launcher_apk.sh` produces
  `rbguest-launcher.apk` 2,659,673 bytes, versionCode 262 / versionName 0.2.0, minSdk 26 / targetSdk 34,
  apksigner-clean, no `application-debuggable` in `aapt2 dump badging`. Installed on the Tab S9 from that
  exact file: the library loads and an `-e repack` start is ignored (diagnostics are refused in a release
  build). SHA-256 `001a7106…c39`.
- **Icon (D5):** the launcher shipped with Google's Android robot. It now carries an original RetroBug
  icon — a flat teal beetle on a transparent background — generated for the project and checked at all five
  densities (the 48 px one is legible). `tools/make_launcher_icon.ps1` takes the source as an argument and
  regenerated the mipmaps; the robot source stays in the tree, but NOTICE.md now credits it as a previous
  asset that is no longer packaged, and the manifest comment says the same.
- **Build script:** the closing "build is …" line printed "a release build1" (both shell expansions ran);
  it is an if/else now.
- **Notes:** `docs/session/release/RELEASE_NOTES_0.2.0.md` — what it is, install steps, what works on the
  Tab S9, the named limits (C31/C37/C50/C51 and Play-services checks), the signing warning (CI's throwaway
  key, D4), and what changed since 0.1.
- **Backlog:** A7 and D1/D2/D3/D5 closed with what was verified; A6 now lists only post-release polish
  (C17/C20/C21/C23 and the packaging/engine nits); C13/C14/C22 confirmed already fixed in the code.

### 2026-10-25 — the launcher icon stays the Android robot (owner's call)
- The RetroBug beetle icon was replaced at the owner's request; the launcher ships the Android robot again.
  The commit that introduced the beetle is kept in history, but the packaged densities are regenerated from
  `file_0000000033448211bbdb620eed523634.png` and NOTICE.md carries the CC BY 3.0 attribution that applies.
- `tools/make_launcher_icon.ps1` keeps its `-Source` argument (default back to the robot), so another icon
  needs no script change. The generated beetle source was removed from the tree.
- Release build rebuilt from the restored resources: versionCode 264 / versionName 0.2.0, 2,659,673 bytes,
  SHA-256 `7e268510…6ae8`, installed and opened on the Tab S9; Recents shows the robot icon. Release notes
  updated to the new code and hash.

### 2026-10-25 — iOS S0a: the platform seam (memory, image files, tid, guest-visible host facts)
- **What landed:** `core/include/rb/platform/host.h` — the small set of host facts the engine cannot get
  from the C++ standard library: window reservation (incl. "exactly here or fail"), an anonymous image file
  for the loader, the calling thread's tid, the guest's `utsname`, memory info, rlimits in the *guest's*
  numbering, CPU affinity, scheduler facts, and the guest-clock mapping. Implementations:
  `platform/posix/host_posix.cpp` (the code these operations always had, moved) and
  `platform/darwin/host_darwin.cpp` (mkstemp image file, `pthread_threadid_np`, `host_statistics64`,
  guest→Darwin rlimit table, and the clock translation the research flagged: Darwin's MONOTONIC counts
  through sleep, so guest MONOTONIC → `CLOCK_UPTIME_RAW` and guest BOOTTIME → `CLOCK_MONOTONIC`).
- **Call sites moved:** `guest_memory.cpp` (flat-window reservation), `guest_elf.cpp` (memfd → ImageFile),
  `guest_thread.cpp` (gettid), `monitor.cpp` (clock, uname, sched_get{,param,scheduler,priority_*},
  prlimit, sysinfo). Android behaviour is intended to be unchanged; the only deliberate differences are
  that `prlimit` now answers for the calling process (guest pids are virtual) and `sysinfo` applies
  `mem_unit` (always 1 on Android).
- **New gate:** `tools/check_layering.sh` — fails on Android/JNI/GL/Mach headers in `core/`, reports (and
  will fail, under `--strict`, once S0b lands) the platform-specific syscalls still in `core/`.
  It strips comments and string literals first, so its own explanations don't trip it.
- **New CI:** `.github/workflows/host-and-ios.yml` — macOS builds and runs `rb_platform_tests` (the only
  suite that needs no guest/device), and an iOS-arm64 configure builds `rb_platform rb_mem rb_loader`,
  which is the honest "compiles for iOS" statement until the syscall backend lands.
- **Verified here:** arm64 test build + launcher build compile; `rb_platform_tests` builds. The device
  gate could not complete because the tablet dropped off USB mid-run ("device not found"); the Darwin code
  cannot be compiled on the Windows host at all, so both are pending their runners.
- **Next (S0b):** eventfd/statx/getdents64/epoll/ppoll/pselect6/renameat2 and the guest `O_*` flag
  translation behind `rb::platform`, then `check_layering.sh --strict` in CI.

### 2026-10-25 — iOS S0 complete: the syscall backend, and a strict layering gate
- **S0b landed.** `host_io` no longer names or calls a Linux-only interface: guest `O_*`/`AT_*` flags,
  the dirfd (guest `AT_FDCWD` -100 vs Darwin -2), `stat`/`statfs` through a host-neutral shape, `statx`
  marshalling (Darwin has no statx), `poll`/`ppoll`/`pselect`, `pipe2`/`dup3`/`accept4`, `renameat2`,
  entropy, and `eventfd`/`epoll` (kernel on Linux; Darwin refuses by name instead of aliasing a pipe, which
  would give the guest different semantics under the same fd).
- **`check_layering.sh` is strict by default** — `core/` has no Android/JNI/GL/Mach headers and no
  platform-specific host call. `--report` keeps the old advisory mode.
- **Verified:** Android gate 26/26 suites, 0 failures (twice: before and after the syscall move);
  `rb_host_io_tests` 52, `rb_syscall_monitor_tests` 233, `rb_platform_tests` 29 on device. CI: macOS
  platform test 26 checks/0 failures + layering clean; iOS arm64 compiles `rb_platform rb_mem rb_loader
  rb_syscall`.
- **Known gap, stated rather than hidden:** Darwin directory listing returns `ENOSYS`. `getdirentries` is a
  link-error stub with 64-bit inodes, and `readdir` buffers a whole block, so a per-call `DIR` would skip
  entries. It needs one persistent `DIR` per guest fd plus a close/dup hook from the syscall layer.
- **Also**: Darwin's `fdatasync` is `fsync`; `accept4`/`dup3` are accept/dup2 + `fcntl`; the monitor no
  longer uses `cpu_set_t` at all (the platform answers a 32-bit mask).

### 2026-10-25 — iOS S1a: Tier I decodes each block once (and a real title renders on it)
- **The cache.** Tier I called `isa::TranslateOne` inside its run loop and allocated an `IR::Block` on every
  entry. Blocks are now cached by **(pc, mode, IT, FPSCR)**. FPSCR belongs in the key: Dynarmic bakes the
  rounding mode and flush-to-zero into the FP instructions it emits, and the tier-diff `fp_rmode` rows
  failed immediately without it (two of five rows disagreed with hardware by one ulp).
- **Correctness rule.** Only blocks on **non-writable pages** are cached. Guest text is read+exec,
  `GuestMemory::Write` refuses a read-only page, and a writable page (a JIT arena, or this repo's own patch
  test) is decoded afresh — so self-modifying code stays correct without a write observer. That is also why
  the tier-diff SMC row kept its meaning: a code page the guest can write is never cached.
- **Invalidation.** `arm_cacheflush` evicts by **page overlap**, not start address: the fixture patches one
  byte at +8 and flushes from there, and an address-only comparison left the stale block in place (measured:
  `I=1008` after the flush).
- **Contract change, deliberate and recorded in the test**: Tier I no longer promises fresh code *before* a
  flush. It used to match hardware only because it owned no cache; ARM requires the flush, and Tier J — the
  Android default — has always behaved this way. The lane now asserts both tiers are stale pre-flush and
  fresh post-flush.
- **Measured:** `rb_exec_interp_tests` 578 ms → 164 ms on the Tab S9 (same binary shape, 1096 checks);
  tier-diff 1039 checks, 0 failures; whole gate 26/26 suites.
- **Device evidence for the tier itself:** Angry Birds Epic launched with **CPU mode = Safe (Tier I)** and
  rendered its title scene (3D, il2cpp) with its consent dialog. Its boot is host-call bound (1.1 M host
  calls in 10 s) rather than instruction bound, so a throughput number needs a title that actually spins —
  Rolling Sky is the next measurement, not this one. The game's CPU mode and overlay were restored to the
  owner's settings afterwards.
- **Install-flow note for the release backlog:** converting/updating a copy now raises two system dialogs on
  this device — "Update this app?" (PackageInstaller) and Google Play Protect's "App scan recommended" →
  "This app looks safe" — so a conversion is not silent on Android 14+. The launcher reports
  `PENDING_USER_ACTION` while the first is up, and `INSTALL_FAILED_ABORTED: User rejected permissions` if it
  is dismissed. Worth surfacing in the UI ("confirm the system install prompt").

### 2026-10-25 — iOS S1b: the boot moves into core, and the engine builds for iOS
- **`rb::host::Boot` is the shared boot.** The whole Android runtime's mechanics — scope from
  `assets/rb32sys/`, DT_NEEDED-first loading, constructors, guest `JNI_OnLoad`, guest dlopen/dlsym, the
  poison-after-a-touched-failure rule, the stall monitor — now live in `core/host/boot.cpp`. ART, host
  slots, OpenSL ES, logcat and the report path are callbacks (`BootConfig`), so `android/jni/runtime_boot.cpp`
  is a platform wrapper and iOS can call the same boot.
- **The APK reader grew a memory constructor** (`rb::loader::ZipReader`), and `NeededSonames` moved to
  `rb::loader` — the boot no longer needs a launcher to parse an image's dependencies.
- **iOS compile proof, and the real blocker it exposed.** The iOS CI job now builds `rb_host`, which pulls
  `rb_inbound`, `rb_runtime`, `rb_abi`, `rb_exec`, `rb_isa` and dynarmic. The first run failed with
  "No rule to make target rb_host": iOS sets no `CMAKE_SYSTEM_PROCESSOR`, so the arch gates never fired.
  One `RB_AARCH64_HOST` in the top-level CMakeLists (processor **or** `CMAKE_OSX_ARCHITECTURES`) fixed
  both places; `inbound_aarch64.S` also needed Mach-O symbols (`_` prefix, `.private_extern`, no
  `.type`/`.size`). The second run built everything through dynarmic and the trampolines.
- **New lanes:** `rb_zip_tests` 25 checks (stored/deflated/CRC/ZIP64/file-backed, a zip written by the
  test), `rb_host_boot_tests` 46 checks (scope, sibling-first constructors in bionic order, boot-once,
  dlopen/dlsym, missing entry retry, constructor-fault poison). Device gate **28/28 suites, 0 failures**.
- **Real-launcher evidence.** Bootcheck of Blocky Roads: 58 constructors on Tier J, `JNI_OnLoad` returned
  `0x10004`, "startup finished with no fault". Rolling Sky: its Unity `JNI_OnLoad` needs
  `com.unity3d.player.NativeLoader`, which only its own DEX provides; the boot stops by name at
  `RegisterNatives(null class)` and the report is written — the launcher path cannot validate further, the
  repackaged app can.
- **A latent crash found and fixed:** the launcher's `nativeBootCheck` called `NewStringUTF` with the
  guest's `ClassNotFoundException` still pending; ART aborts on any JNI call then, so the process died
  (SIGABRT) before reporting. It now clears the exception and puts it in the report.
- **Not done yet:** EAGL/EGL, touch, audio, `IosJavaHost`, the SwiftUI shell and the unsigned-IPA job; the
  Tier-I Rolling Sky throughput is still the open G1b measurement.

### 2026-10-26 — iOS S1c: EGL on EAGL, the iOS host slots, and the Java shim as a library
- **The GL thunk table moved to `rb::gfx`** so both platforms answer the same imports; Android keeps only the
  symbol source. `rb_gl_thunk_tests` still passes on the tablet (22 checks).
- **EGL on EAGL.** iOS has no libEGL, so `platform/ios/egl_shim.mm` implements the 1.4 subset a
  GLSurfaceView-era title uses: display/config/context/surface life, make-current over a CAEAGLLayer,
  present on swap, query strings/surfaces, and eglGetProcAddress. Compiled for iOS by CI.
- **iOS host slots.** `platform/ios/ios_host.cpp` resolves `rb::gfx` names through `ThunkCaller` to the
  shim/framework and hand-writes what a signature cannot express (returned strings, config handle arrays,
  eglGetProcAddress's guest slot, glShaderSource's pointer array).
- **The Java replacement.** `FakeJava` — already the engine's no-VM Java, covered by the JNI bridge lane —
  is now `rb::java::TableJava`, a library, with `core/tests/fake_java.h` as an alias. The base Android
  profile registers System, SystemClock, String, Integer, Build, Log, Locale, File, Context and Activity;
  an unregistered class is a `NoClassDefFoundError` by name.
- **Verified:** macOS CI: `rb_table_java_tests` 36 checks, `rb_zip_tests` 25, `rb_platform_tests` 26, all 0
  failures; iOS CI builds `rb_gfx rb_java rb_host rb_ios`. The Tab S9 disconnected during this step, so the
  Android gate was last green at S1b (28/28) and must be rerun before the next device claim.
- **Next:** the title bootstrap. Blocky Roads is Cocos2d-x and its exports are known
  (`Cocos2dxHelper_nativeSetApkPath`, `Cocos2dxRenderer_nativeInit/nativeRender`, the touch natives);
  the Java-layer order and the Surface hand-off must come from classes.dex/trace, not from memory.

### 2026-10-26 — iOS S1d: the app and an unsigned IPA
- **A real app.** SwiftUI imports an APK from Files and plays it; the engine's C surface
  (`rb_ios_create/start/touch/*`) owns the boot, the EAGL context and the one render thread. The game's
  Cocos2d-x entry points are called directly (`nativeSetApkPath`, `nativeInit`, `nativeRender`, touches),
  with the Java-layer order read from the APK's DEX.
- **Packaging.** CMake merges the component libraries into `librbengine.a` (`libtool -static`, after renaming
  the platform bootstrap so no two objects share a basename); XcodeGen generates the project;
  `tools/build_ios_ipa.sh` produces `Payload/RetroBug.app` → unsigned IPA. CI builds it on macos-15 (the
  XcodeGen project format is newer than macos-14's Xcode).
- **Artifact:** 1,211,326 bytes, SHA-256 `5a276c87…0552b850`, arm64 executable 5.3 MB.
- **Constraint found and documented:** the boot reads `assets/rb32/` + `assets/rb32sys/`, so the first run
  needs a bootcheck APK (`tools/make_bootcheck_apk.sh`); a store APK has `lib/armeabi-v7a/` and no bionic.
  Bundling bionic into the app is the next step for "import in place".
- **Verified:** device gate 30/30, 0 failures; macOS host lanes 26/25/37/41; the IPA job builds and uploads.

### 2026-10-26 — iOS S1e: the import/Files evidence, and a log worth sending
- **Report:** on the iPad, the "+" picker showed the .apk but a tap did nothing; the app's Documents folder
  was absent in Files; the app felt unstable.
- **Two agents, one answer each.** Research: Files hides an app's Documents folder **while it is empty**
  (the README file now prevents that), and SwiftUI's `.fileImporter` has a known real-device failure where
  a tap is swallowed even with `.item`; the UIKit `UIDocumentPickerViewController(asCopy:)` is the reliable
  path. Review: the boot ran on the main thread inside `makeUIView`; `rb_ios_start` could assign over a
  joinable `std::thread` after an error (terminate); resize re-ran nativeInit without a pause; nothing
  observed backgrounding.
- **Fixes:** UIKit picker with logged delegate callbacks; crash-safe `retrobug.log` (engine lines included
  through `rb_ios_create(log_path)`) and a Share log button; README in Games; build marker (version, build,
  git commit) in the UI and log; boot deferred to the render thread; join-before-restart; error/touch/frame
  reset per start; exception guards; `rb_ios_pause/resume` applied on the render thread via a flag, wired
  to willResignActive/didBecomeActive.
- **Artifact:** 1,983,433 bytes, SHA-256 `4af3b1d0c937ae5c8961d5178d3817f120ec910854a633b1966528d4b16a61d7`.
- **Still to confirm on device:** whether the picker now delivers `.apk` (log: "picked"/"copied"), whether
  "On My iPhone › RetroBug" appears with the README present, and the first boot's failure point from the
  log (ES2/ES1 acceptance, AAsset, Cocos order).

### 2026-10-26 — iOS S1i: the next layer in one build (assets, audio, Java text)
- After the scope booted and the stop named `dlopen`, the next layer was mapped statically before another
  device run: `llvm-nm` on libgame.so (289 imports), its JNI signature strings, and the Android host
  layer as the reference.
- Added: AAssetDir listing over the APK; OpenSL ES answered by failing `slCreateEngine` cleanly (audio
  silent; the four `SL_IID_*` are data imports and answer guest cells); Cocos2d-x Java classes
  (Bitmap/GLSurfaceView/Renderer/Activity) and the listener/Context methods; a safe text entry point (no
  native bitmap yet); `files/` and `cache/` under Application Support; the scope line now prints the window
  the device granted.
- Two agents are auditing the two hard remaining paths (Java text via Bitmap/Canvas and the exact
  OpenSL failure semantics) from the binary; their findings decide whether a CoreText text renderer and a
  fake-engine vtable are needed before the first frame.
- Verified: `rb_table_java_tests` 41 on the tablet; CI builds the IPA (c987fa1).

### 2026-10-26 — iOS S1m: the DLGE app singleton, and the second Java load
- Disassembled KitKat's two libraries. `libdlgl.so:dlxSurfaceChanged` (0x37410) never creates the app:
  it copies `DLApp::_currentInstance` and logs "something wrong, app is null" when that is zero.
  `libandroidkitkat.so:JNI_OnLoad` (0x2b574) is the writer — `new Main`, whose constructor calls
  `DLApp::DLApp` and stores `this` in `_currentInstance`. The DEX's `AndroidKitKatActivity.<clinit>`
  loads `dlgl` then `androidkitkat`; the iOS session booted only the first.
- Added `Boot::BootJavaLibraries` (shared by `rb_runtime_boot` and the iOS session): for a DLGE APK it
  boots `libdlgl.so` first, then every bundled library whose DT_NEEDED reaches one already in the order.
  The iOS profile choice is now the APK's contents (`BundlesDlgl`) rather than the library's name.
- Added the Throwable family to the Android base profile (`Error`, `NoClassDefFoundError`, ...): the
  fault path's `FindClass("java/lang/Error")` now resolves instead of queueing a second throwable.
- The tablet bootcheck of the same APK stops earlier, at `RegisterNatives: null class`: its ART
  `FindClass` runs on the launcher's loader, not the game's. That is the diagnostic harness, not the
  installed game — noted in the handoff so it is not mistaken for the product path.

### 2026-10-27 — iOS S1n: OpenSL has to be a real object model, not a failed call
- The IPA at commit `9a3a4bf` fixed the old failure: `retrobug (7).log` shows
  `java: loads libdlgl.so libandroidkitkat.so`, and the old `surfaceChanged … app is null` block is
  gone. Both attempts now stop inside `libandroidkitkat`'s JNI_OnLoad immediately after
  `sles: slCreateEngine -> SL_RESULT_FEATURE_UNSUPPORTED`, before `creating main app`, then the app
  starts a fresh process. No `signals:` line is written, so the guest interpreter did not name a fault.
- Disassembly narrowed the path: JNI_OnLoad -> `Main::Main` -> `DLApp::DLApp` (0x3584c) -> `new
  SoundContext` -> `alcOpenDevice`. The known-good Android trace creates an output mix **and an
  audio player**, and receives a buffer-queue callback, before `creating main app`. A failed engine is
  therefore not the "run silently" contract; the failure branch is null-safe, but it is not the path
  the title starts on.
- Added `rb::audio::SilentSles` (`core/audio/silent_sles.cpp`): guest IID cells and GUIDs, guest
  vtables, Engine -> OutputMix -> Player, Play and both buffer-queue interfaces, and a muted sink that
  counts and drops buffers. It is platform-free; the iOS host injects memory, slot binding, data
  imports and logging. `rb::audio` is part of the shared core and the iOS engine archive.
- Added `rb_silent_sles_tests`: it drives the same `(*itf)->Method(self, …)` ABI and covers the
  OpenAL startup sequence, IID identity, play/queue state, an unknown IID, and Destroy invalidating
  derived interfaces. The Linux and macOS host lanes run it.
- Added a host-only crash line: SIGABRT/BUS/FPE/ILL/SEGV/TRAP and uncaught C++ termination append
  `RetroBug host crash/terminate` to `retrobug.log` (the guest's own faults still go through the
  interpreter). The next device log can distinguish a guest stop from a host/driver exit.
- **Verified:** `rb_silent_sles_tests` 55 checks / 0 failures on Linux and macOS; layering clean;
  iOS arm64 compile green; unsigned IPA built from commit `98df2aa`, build 4.
  `build/RetroBug-unsigned.ipa`, 2,050,910 bytes, SHA-256
  `afb90d5a581f151e9af3e2a997dfccb1536b8f8ae67b6cbbdf03779c0c6446b6`.
- **Not yet proven:** that this is the only remaining stop, or that the constructor now returns.
  Next device log should contain `sles: silent backend installed`, `slCreateEngine -> 0`,
  `CreateOutputMix`, `CreateAudioPlayer`, then `I/tApp: creating main app`, and the first
  `surfaceChanged`. If it stops earlier, the new SLES lines and host-crash marker name the layer.

### 2026-10-28 — iOS S1o: the second half of the DLGE contract
- The exact Android-side sequence was traced: engine Realize; GetInterface(SL_IID_ENGINE);
  CreateOutputMix + Realize; CreateAudioPlayer with ANDROIDSIMPLEBUFFERQUEUE; player Realize;
  GetInterface(BUFFERQUEUE) (the create/query alias); RegisterCallback; two Enqueues;
  GetInterface(PLAY); SetPlayState(3). The shared silent model now has a test for that exact order.
- **The DLGE bootstrap passed null as every native's `JNIEnv*`.** `dlxSurfaceChanged` uses it for
  `GetStringUTFChars(language)`, so the first surface callback could never have run. It now receives
  `Boot::jni()->env()`, and `rb_dlgl_bootstrap_tests` records the env and call order through fake natives.
- The Java profile had a second startup bug: `LocalStorage`'s methods were registered before the class
  existed, so every save/load was silently returning zero with a pending `NoSuchMethodError`. The class
  is now defined first; GATracker, GooglePlayServices and the three Activity actions the natives call
  have their `(String)V` methods.
- All 25 GLES1 and six AAsset imports resolve through the iOS host table. The bootcheck APK contains all
  48 `assets/default/` resources the engine opens, plus both guest libraries and the four bionic images.
- Added early frame probes (frames 1/10/60/300) that log nine RGBA samples from the EAGL drawable before
  present, so a black screen is distinguishable from no output. Removed a duplicated depth-renderbuffer
  allocation in the EAGL framebuffer setup.
- **Verified:** host suite `rb_table_java_tests` 60/60, `rb_silent_sles_tests` 61/61,
  `rb_dlgl_bootstrap_tests` 23/23; Linux layering clean; macOS host, iOS arm64 compile and IPA green at
  `baf1e0b`. `build/RetroBug-unsigned.ipa` is build 5, commit `baf1e0b`, 2,053,315 bytes, SHA-256
  `1ea2449abd1e8d0c923d9b9fe13748cb12217fe7351720a43683450a86a9eaf2`.
- **Still device evidence only:** `I/tApp: creating main app`, `surfaceChanged`, then `frame N pixels:`
  with at least one non-zero channel. Audio buffers are deliberately not callbacks in this silent build.

### 2026-10-28 — iOS S1p: build 5 reached the audio boundary, and the host fault is now traceable
- The first build-5 device run (`retrobug (8).log`) reached `sles: player state PLAYING (silent)`, then
  stopped with `=== RetroBug host crash: signal SIGSEGV ===` before `creating main app`; a second attempt
  stopped earlier, after `sles: audio player realized`. This is the OpenAL context/audio-thread boundary,
  not the old app-null or JNIEnv failures.
- Build 6 adds a lock-free last-host-call breadcrumb in `IosHost::Resolve`, host `backtrace()` frames on a
  real host signal, and capped guest-thread spawn/bind lines in `GuestProcess`. It changes no game
  behavior; the next `retrobug.log` will identify whether the SIGSEGV is in the OpenAL feeder thread,
  a host slot, or the host-thread setup.
- **Verified:** build-6 CI run `40619908982` green on Linux/macOS/iOS compile/IPA; host tests remain
  60/61/23. `build/RetroBug-unsigned.ipa` is build 6, commit `1a2dfe0`, 2,054,874 bytes, SHA-256
  `6f56f5535c0a0e3644fde7840f8b8860e4e1fc6ecb0da6a5af147d9eb98930c5`.

### 2026-10-28 — iOS S1q: the crash is in the ARM front end, not the audio shim
- Build-6 device log: the first attempt crashed after `output mix realized`; the second reached
  `creating main app`, `surfaceChanged`, and `language: en`. Both host backtraces are
  `TranslateOne -> A32::TranslateArm -> arm_SUB_imm` / `arm_MOV_reg -> PrependNewInst`. The host signal is
  therefore in Dynarmic's ARM translation path, after the SLES/OpenAL boundary; the new OpenSL model is
  not the crashing component.
- Build 7 records `LastTranslationSite` (pc, ARM/Thumb, IT) from [front_end.cpp](../../core/isa/front_end.cpp)
  and the current guest thread pc/cpsr in the host crash line. No behavior changed.
- **Verified:** build-7 CI run `40621247313` green on all four lanes; IPA build 7, commit `9e2ee4b`,
  2,056,945 bytes, SHA-256 `7ae53736ba11d16c93ca08cc9839ed3c059dfde491bb713e9aed91bad2563f7d`.
  Next log should name the translation PC/mode, which maps directly into the KitKat library.

### 2026-10-28 — iOS S1r: the ARM crash is the Tier I IR pool, and the fix is bounded
- The build-7 stack was decoded and reproduced against the vendored source: `PrependNewInst +44` is the
  return from `Pool::Alloc`, not the list insertion. `IR::Block` eagerly allocates
  `sizeof(Inst) * 4096` (104 × 4096 = 416 KiB) for every translated block. Tier I retained up to 8192
  blocks, so the cache could reserve about **3.2 GiB**; `Pool::AllocateNewSlab` does not check malloc,
  and the next placement-new at null is the SIGSEGV. Android Tier J avoids it because it emits and
  releases each IR block, bounded by its 64 MiB code cache.
- Bound `InterpExecutor::kBlockCacheLimit` from 8192 to 128 in [interp.h](../../core/include/rb/exec/interp.h).
  At 416 KiB per block that is about 54 MiB worst case; hot blocks remain cached and invalidation is
  unchanged. This is the actual first-frame fix candidate; a future Dynarmic patch can shrink the initial
  slab to 64 if the larger cache is needed for speed.
- **Verified:** build-8 CI run `41171450697` green on Linux/macOS/iOS compile/IPA; IPA build 8, commit
  `3a5a30b`, 2,056,945 bytes (same build layout), SHA-256
  `8a5e1b51a4644833d16613ff2815270df4aad4e2073a4475de065eb7e6664208`.
- Next device log: if the cache bound is the cause, the crash should disappear or move past
  `surfaceChanged`; the translation-site line remains in place for any different fault.

### 2026-10-31 — iOS S1s: Blocky Roads' Java surface, and build 9
- The two new device logs (`retrobug (10).log`, `(11).log`) are **build 6** (`=== RetroBug 0.1 (6) commit
  1a2dfe0 ===`), not build 8: their KitKat crashes — `PrependNewInst` after `Pool::Alloc` — are the fault
  the build-8 cache bound addresses, so KitKat has not yet run with that fix.
- In the same logs Blocky Roads' `nativeInit` stops on a Java surface the profile did not provide:
  `FindClass("com/utils/GameControllerInput")` misses, `hideLoadingWindow()V` misses on
  `Cocos2dxActivity`, `FindClass("com/utils/FileHelper")` misses, and the following
  `GetStaticMethodID(null class, …)` is the fatal host call.
- The new profile [blockyroads.cpp](../../core/host/java/blockyroads.cpp) answers every class `libgame.so`
  names, enumerated by pairing each class-name load with the method/signature strings loaded at its JNI
  call sites: GameControllerInput, FileHelper, CCMotion, HttpGet, DeviceAndSystem, InappPurchase,
  ShareInterface, GPlayService, GPLeaderboardEntry, DropboxInterface, DropboxFileItem, UnityAds, Admob.
  FileHelper is a real store under the app's files directory, mirroring the Java class: reads answer the
  catch-all's empty array, never null, and names with separators or `..` are refused. The other classes
  answer their "no backend" values (no controller, no accelerometer, signed out, billing/ads/Dropbox
  unavailable, no guest network).
- The shared Cocos2d-x profile gained the 3.x statics the library looks up: Activity
  `getAppVersion`/`getDisplayDensity`/`getApkExpansionFilename`/`deleteApkExpansionFile`/
  `showLoadingWindow`/`hideLoadingWindow`/`isHpeExperience`/`isChromeOS`,
  `Cocos2dxRenderer.setAnimationInterval`, the GLSurfaceView thread controls, `showDialogAndTerminate`, and
  the byte-array `showEditTextDialog` overload (2.x's string form stays). A method miss in the trace now
  names its class and signature, e.g. `Java: no method org/cocos2dx/lib/Cocos2dxHelper.showEditTextDialog(…)`.
- **Verified:** `rb_table_java_tests` is 226 checks (from 60): a row table of every class/method/signature
  the library names, the FileHelper round-trip and traversal refusal, the Activity answers, and a negative
  control that proves a wrong signature fails by name. Linux lane `41173617692` green
  (226/61/23 checks); full workflow `41174067978` green on Linux, macOS host, iOS arm64 compile and IPA.
  The test caught two of this session's own rows with wrong receiver signatures before the build.
- IPA build 9, commit `5eeab1f`, 2,077,870 bytes, SHA-256
  `80835ff0916605a87f4fc3b5d7c2c2b58d217fb01b79aa835b3cf8711c1d9e9b`; its `Info.plist` decodes to
  `CFBundleVersion 9`, `0.1`, `RBGitCommit 5eeab1f`, and its binary differs from the build-8 artifact.
- **Next device log:** build 9 carries both device-untested fixes. KitKat: the cache bound should let
  translation continue past the crash into `creating main app` → `surfaceChanged` → `frame N pixels:`.
  Blocky Roads: `nativeInit` should complete now that its Java surface resolves, and the first
  `nativeRender` frame should arrive. Any remaining Java miss prints class + signature, which is a row
  to add; a crash prints translation pc/mode plus the last host call.

### 2026-10-31 — iOS S1t: build 9 reaches the first GL draw; build 10 fixes host access
- Build 9 is the first build where the Tier I cache bound lets KitKat translate again: both runs pass
  `creating main app`, `surfaceChanged 2160, 1620`, `creating app`, the GATracker call, the `preload`
  group (`default/images/preloading.png`, 6,676 bytes) and `[Main] onLoadComplete: preload`. The old
  `PrependNewInst` fault is gone.
- KitKat then faults at the **first real draw**: `last host call: glDrawArrays`, translation pc a PLT
  stub in libdlgl, host frames `ThunkCaller::Call` → `GLEngine` (+101316, +749520, +576132) with a null
  frame in the driver. The `assets/ww/...` misses before it are alternate paths the engine probes; the
  `default/...` path it falls back to loads.
- Blocky Roads' `nativeInit` now completes (no Java misses, no stop) and the asset manager answers. It
  stops at Horde3D: `glShaderSource refused: string 0 is not guest memory` → `h3dInit Failed`, then
  `glBindAttribLocation refused: pointer 0xe0afda` and `glGetUniformLocation refused: 0xdfe6b2`.
- Those pointers are valid: `0xe0afda` is libgame.so's base `0xc05000` plus vaddr `0x205fda`, the rodata
  string `"aPosition"`. On a 16 KiB host page, a module placed at a 4 KiB-aligned base has file-backed
  granules whose in-place mmap offset cannot be real-page aligned, so the pages are served out of line —
  and `GuestMemory::Host` is the fastmem query, which refuses them by design. Every host callee that
  dereferenced a guest pointer used it.
- Build 10 adds `GuestMemory::HostForAccess` (the backing block for in-place and out-of-line pages) and
  uses it where a host call reads or writes guest memory: the thunk `p`/`o` arguments, the iOS and
  Android GL specials, asset reads, file-syscall buffers, OpenSL address locators and the JNI shadow
  sync. `Host`/`IsFastmem` keep their meaning for Tier J.
- The crash log now carries a 256-entry ring of host calls (name and r0–r3, written before the call) and
  the EGL context version; context creation and make-current changes are logged. KitKat's driver fault
  has no guest stack, so this ring is the evidence that names the state it died on.
- **Verified:** the Linux and macOS lanes now run `rb_guest_memory_tests` and `rb_thunk_tests` (the
  suites that exercise the accessor): Linux 206 and 43 checks, macOS 121 (6 memfd skips) and 35. The
  first macOS run of the thunk suite crashed in its first case — `Window()` hard-coded a 4 KiB
  `AddressModel`, which `Create` refuses on a 16 KiB host, and the test dereferenced the null window;
  fixed to follow `sysconf(_SC_PAGESIZE)`. Full workflow `41176629818` green on Linux, macOS, iOS arm64
  compile and IPA at `c85f9e7`.
- IPA build 10, commit `c85f9e7`, 2,079,871 bytes, SHA-256
  `09b6312524a0a0a45cdc6a5c9e6834212fd63ebba9ad0a53d891cfc184903ae0`.
- **Next device log:** Blocky should get past shader compilation and Horde3D init now that its rodata
  pointers reach the driver; KitKat is expected to still fault in the driver, but the ring will print
  the GL calls leading to it with their arguments and the crash line names the EGL context version.

### 2026-10-31 — iOS S1u: KitKat draws its LOADING screen on the iPad
- Build 10's device log is the first with a **rendered frame**: `frame 1 pixels: … 1080,810=a3c439ff …` —
  the Android-green robot at the centre of the LOADING screen, matching the screenshot. The KitKat run
  before it had `egl: createContext -> GLES1 context created`, the full boot, `surfaceChanged`, and the
  preload group, then rendered and went on to load the `default` group (all 24 sounds, textures6.json,
  textures6.png 2,121,891 bytes). The build-9 driver fault at `glDrawArrays` is gone; the out-of-line
  access fix is confirmed by the thing that matters, pixels on screen.
- Blocky Roads in the same log created a **GLES2** context, completed `nativeInit`, and shows no
  `glShaderSource`/`glBindAttribLocation` refusals — its rodata pointers now reach the driver. It opened
  `assets/data.xpk` (31,189,769 bytes) and the log ends there; the run was cut short (the user switched
  titles), not crashed: no crash header, no host fault.
- Both sessions end mid-load, so the log cannot say whether the load is slow or the process died. Build 11
  adds a 5 s monitor (`stall: still frame N`, suppressed while the app is paused), a `render: frame N spent
  T ms` line past 2 s, and a frame heartbeat every 300 frames after the sampled ones. KitKat decodes a
  2.1 MB atlas on Tier I, which is expected to take a while; the next log says whether it finishes.
- **Verified:** workflow `41178428090` green on Linux, macOS, iOS arm64 compile and IPA at `1873c2f`.
  IPA build 11, commit `1873c2f`, 2,080,719 bytes, SHA-256
  `b52bd428fc26eda1df3acce0e55ad0da984a2f1a0d46d94a111f29b728734062`.

### 2026-10-31 — iOS S1v: both titles render; KitKat's menu stalls on a dying guest thread
- Build 11 ran both titles for minutes without a host crash. **Blocky Roads drew its first real frame**:
  `render: frame 1 spent 111143 ms in the guest`, then nine samples with distinct colours
  (`4c1908`, `5d375c`, `f25f34`, `18252b`, `072025`, `01daf9`, `402e6c`, `df5c28`, `cd8d36`) — content,
  not a clear colour. The 111 s is its 31 MB `data.xpk` parse under the interpreter.
- **KitKat** rendered the LOADING screen (frame 1), then spent 100,970 ms in its second frame loading the
  `default` group (24 sounds, `textures6.json`, a 2.1 MB atlas). It never logged
  `[Resources] load complete - group: default`; from the moment that frame returned it renders **white**
  at ~30 fps (frames 300–3000 sampled pure `ffffffff`) and logs **one guest SIGSEGV at `0x29` per frame**
  — 3,088 lines in four minutes, no handler installed, so each one kills its thread. The host is healthy;
  the guest is retrying something that faults every frame.
- Neither title crashed; the user closed both. The stall monitor and slow-frame line added in build 11 did
  exactly their job: they turned "looks frozen" into "100 s inside one guest call" and "still frame N".
- Build 12 names the fault: the first occurrence of a (signal, address, pc) key logs the pc, ARM/Thumb
  mode, guest tid and the faulting thread's stack words as module+offset, and repeats are counted (first
  three, then every 300th) instead of printed. Thread spawns are sampled past the first eight instead of
  capped, so a per-frame thread loop is visible.
- **Verified:** workflow `41181395759` green on Linux, macOS, iOS arm64 compile and IPA at `940b57d`.
  IPA build 12, commit `940b57d`, 2,082,652 bytes, SHA-256
  `77789184ea002db11a25a65f64bda88e6fe9dfee3a227d56b69f63e9b8cbe3e5`.
- **Next device log:** the KitKat fault line will carry `pc 0x…` and a `libdlgl.so+0x…` caller chain, which
  maps into the engine's code; the spawn sampling says whether a thread is created per frame.

### 2026-10-31 — iOS S1w: a native's fault was only written to stderr on iOS
- KitKat's white screen is a fault **inside `dlxDraw`**, not in the render loop: the loop keeps calling the
  game's draw, the guest call faults at `0x29` (a read from a null object at offset 0x29) and returns early,
  so the frame presents white. The fault repeats once per frame because every frame calls `dlxDraw` again.
- The evidence existed all along and was thrown away: `Inbound::Fail` writes "guest native dlxDraw()
  stopped: …" plus the faulting registers and module-mapped stack words, then throws a pending
  `java.lang.Error`. On Android a real Java caller reads that error; on iOS the Java is a table with no
  caller, and the line went to **stderr**, which the app does not capture. That is why 3,088 device log
  lines named only `si_addr`.
- Build 13 gives `Inbound` the boot's log sink. The first three failures of a native print the full
  message — reason, pc/lr/sp, r0–r12 and `StackWords` (`libdlgl.so+0x…` chain) — and later ones are
  counted per native, so the per-frame fault stays readable.
- **Verified:** workflow `41182728525` green on Linux, macOS, iOS arm64 compile and IPA at `148f302`.
  IPA build 13, commit `148f302`, 2,083,612 bytes, SHA-256
  `1c98732b45338ce3dec9bf86bfb481ed50d477436020ea949bd6dafa557e82a2`.
- **Next device log:** one short KitKat run is enough — the first `guest native dlxDraw()V stopped:` line
  carries the pc and the caller chain into the engine, which is where the null object comes from.

### 2026-10-31 — iOS S1x: one run must answer everything
- The one-log-at-a-time loop was costing device cycles for single fields. Build 14 prints the whole state at
  once. On a guest fault: `=== guest fault report #N: …` followed by
  - the faulting thread's `r0…r12 sp lr pc cpsr`;
  - 2 KiB of its stack scanned into `libdlgl.so+0x…` return addresses (40 frames);
  - 32 raw stack words, and 24 bytes around the pc for offline disassembly;
  - the eight words behind every register that points into mapped memory;
  - every guest thread's pc/lr/sp, wait kind, last syscall and stack (`StallReport`);
  - every module with base/extent/data range, the **whole mapped address range list**, and threads that
    stopped earlier;
  - the platform suffix: EGL context version, a pending Java exception, and the **last 256 host calls**
    with r0–r3 (`host call: glVertexPointer 3 0x1406 0x14 0xe0afda`).
- Cadence: the first three occurrences of a fault key print in full, then one full report every five
  seconds; the stall monitor prints the same report (no faulting thread) every five seconds while frames do
  not advance — so a 100-second interpreted load names the code each thread is inside, and the next Blocky
  log shows where its 111 seconds went.
- Two defects in the reporting path itself were fixed on the way: `Inbound::Fail`'s registers/stack went to
  stderr only (build 13), and the stall monitor read the render thread's `unique_ptr` boot pointer without
  synchronisation — the process is now published through an atomic with release/acquire so the monitor's
  reads (and the host table behind them) are ordered.
- **Verified:** workflow `41183615682` green on Linux, macOS, iOS arm64 compile and IPA at `d1f56bb`.
  `rb_guest_memory_tests` is 214 checks on Linux (the new `MappedRanges` case included). IPA build 14,
  commit `d1f56bb`, 2,093,143 bytes, SHA-256
  `968b1e160c870b12c00ebaa660e1daf9c823f277ef86ebe1b5e5167353dc4dde`.

### 2026-10-31 — iOS S1y: the full report named the wall — Tier I had no VSHL; the app link was dropping rows
- Build 14's fault report answered it in one run. KitKat's first failure after the atlas load is
  `VectorLogicalShiftLeft32: no Tier I semantics yet`, pc `libc.so+0x5edc8`; the instruction is
  `vshl.i32 d16, d2, #1` at `libc.so+0x5ee72` — **jemalloc's extent lock hash** in `extent_lock2`, which
  every allocation reaches. Tier J executes it; Tier I never had it, so the first post-load malloc died
  inside the draw call, the frame presented white, and every later frame dereferenced the
  half-initialised scene (the 3,088 `SIGSEGV at 0x29` lines were the symptom, not the cause).
- The same report showed the 101-second load for what it is: 23 guest threads, most parked in the same
  `libdlgl.so+0x37a/0x37b` spin loop, `last syscall 172`, while one thread reads `AAsset_read(…, 4)` — the
  engine's job system spinning under the interpreter. No deadlock, just slow; the report names the code.
- Implemented `VectorLogicalShiftLeft{8,16,32,64}` in the Tier I interpreter (per-lane, lane-width mask,
  mirroring the existing shift-right case). A new fixture-free suite, `rb_interp_neon_tests`, assembles
  `vshl.i32`/`vshl.i16` into a guest page and checks the lanes on the Linux and macOS lanes; it caught two
  of this session's own mistakes (Thumb-2 halfword order, a harness that reset the prepared registers)
  before a device ever ran it.
- The same log also showed `Java: no method …AndroidKitKatActivity.exitApp` followed by a call with method
  id 0; the row is added.
- **A packaging defect surfaced while verifying the row**: the CMake archive contained `exitApp`, but the
  linked app did not — the app link's dead-code stripping removed it. `DEAD_CODE_STRIPPING: NO` in
  `ios/project.yml` fixes it and was verified by string-checking the archive and the app in the same run.
  Any profile row could have been dropped this way; the app now links the archive unstripped.
- **Verified:** workflow `41187414418` green on Linux, macOS, iOS arm64 compile and IPA at `5c0a718`;
  `rb_interp_neon_tests` 7 checks, `rb_guest_memory_tests` 214, `rb_thunk_tests` 43. IPA build 16, commit
  `5c0a718`, 2,168,355 bytes (the un-stripped archive adds ~75 KB), SHA-256
  `f47584467c2401554908f993e025989956e14ee49cbe99ed1a90a7833a1ae2e7`.
- **Next device log:** KitKat's allocation and draw should now complete; the menu group can build. Any
  further Tier I gap names its own opcode in the log.

### 2026-10-31 — iOS S1z: Tier I is 3.6x faster, and the cache is 16x larger
- Build 16's device log shows the games are now *correct but slow*: KitKat renders the LOADING screen and
  then spends over 2.7 minutes inside its `default` group load (the report shows 23 threads, then a lone
  main thread inside libc), and Blocky sits in engine init for the whole run. Build 11's "101 s load" only
  finished that fast because the VSHL fault aborted part of the work.
- The interpreter was hashing for every operand: `temps_`, `produced_`, `carry_` and `ge_` were
  `std::unordered_map<const Inst*, …>`, so each IR op did 4–6 hash operations. Dynarmic's `Inst::name` is
  already a dense per-block ordinal (its JIT fills it in the naming pass; this front-end path never did),
  so `isa::TranslateOne` now numbers each block 1..N and the interpreter indexes flat arrays by it.
- **Measured on the SM-X710 (arm64 host, ARM32 guest), same 28.5 M IR-step workload: 11.97 M IR-steps/s
  before, 42.97 M after — 3.6x.** `rb_exec_tier_diff_tests` (1039 checks, Tier I vs Tier J) and
  `rb_exec_interp_tests` pass on the tablet; `rb_interp_neon_tests` is 11 checks (a new cmp/bne + str/ldr
  case covers the paths the change touched).
- The block cache was memory-starved: every `IR::Block` reserved 104 B × 4096 = ~416 KiB, which is what
  held `kBlockCacheLimit` at 128 — and a full cache is cleared wholesale, so a working set larger than 128
  re-translated its hot code constantly. Patch `0007` makes the initial pool slab 64 instructions
  (~6.7 KiB; `Common::Pool` grows in further 64-slot slabs), and the cap is now 2048 (~13 MiB floor).
- New `rb_interp_bench` (fixture-free, hand-assembled loop) prints the rate on the host lanes: Linux
  36.12 M IR-steps/s, macOS 30.24 M. It is informational and fails only on a wrong result.
- **Verified:** workflow `41192209896` green on Linux, macOS, iOS arm64 compile and IPA at `c3a2788`.
  IPA build 17, commit `c3a2788`, 2,168,186 bytes, SHA-256
  `57f429b1560a4fab0288916f3887edb4dad09351695ee1b00fc82cb4f416f7cc`.
- **Expected next device log:** KitKat's atlas load and Blocky's pack parse about 3.6x faster (plus fewer
  re-translations from the larger cache), so the menu should arrive in well under a minute; steady-state
  frame time improves by the same factor.

### 2026-10-31 — iOS S2a: the M1 measures the interpreter, and KitKat's load is LodePNG
- On the iPad Air M1 (iOS 27.2) build 17 behaves: KitKat renders the LOADING screen, then spends over
  100 s in the `default` group load (20 stall reports, closed at ~114 s); Blocky renders its first frame
  in **20,681 ms with real content**, against 111,143 ms on the earlier iPad — **5.4x**, the interpreter
  change landing as measured.
- KitKat's minutes are not I/O and not the host layer: the stall ring shows the 2.1 MB atlas read and
  `AAsset_close`d within the first seconds, and the main thread then samples inside
  `libdlgl.so+0x3b6…`, which resolves to **`LodePNG_zlib_decompress`** — a byte-oriented PNG inflate run
  as guest code. Tier I is the only tier iOS can run, so this is exactly the workload the interpreter's
  speed sets the clock for.
- Build 18 adds the missing instrument: at every slice boundary (each `kSliceSteps` = 200k IR operations)
  the guest pc is tallied, and every ten seconds the session logs the top twelve sites as
  `libdlgl.so+0x…` with percentages, the implied IR-steps/s, Tier I's translation/ cached-block counters,
  and per-name host-call totals (`AAsset_read` volume, GL calls). The same build makes cache eviction
  generational — demoting the young generation instead of clearing everything, so a working set slightly
  larger than the 2048-block cap stops re-translating hot code forever — with invalidation applied to both
  generations.
- **Verified:** workflows `41192209896` (build 17) and `41199078292` (build 18) green on Linux, macOS, iOS
  arm64 compile and IPA; on arm64 phones `rb_exec_interp_tests` 1096 checks, `rb_exec_tier_diff_tests` 1039
  (Tier I vs J, cache-flush rows included), `rb_interp_neon_tests` 11. IPA build 18, commit `1d923de`,
  2,180,837 bytes, SHA-256 `4a9e230d80e5dd6d472c67e89974e86b9e896415088ebeacb0e05c96bfaaedb3`.
- **Honest ceiling:** without a JIT there is no "full speed" — an IR interpreter is inherently several
  times slower than translated native code, which is what Tier J is on Android. The lever left is
  throughput: 3.6x so far from dense value slots, next a pre-decoded operand/micro-op layer if the profile
  says the interpreter loop (not the guest algorithm) dominates, plus cheaper host-call round trips.

### 2026-10-31 — iOS S2b: an optional JIT, with a red/green indicator
- The library now carries a **JIT indicator**: green when this build can map executable memory, red when it
  cannot. The probe is the exact permission the code cache needs — an anonymous `PROT_READ|PROT_EXEC` map,
  which is what oaknut's iOS path allocates — so a build that merely claims JIT support still reads red.
- A persisted switch ("Use JIT when available", on by default) decides whether games use it. The session
  tries Tier J first when the probe passes and the switch is on; if the JIT cannot start, the reason is
  logged and the boot **retries on Tier I**, so a game always starts. JIT is never forced, and switching it
  off is a one-way opt-out (`rb_ios_set_jit_enabled`), read at the next session.
- New C surface: `rb_ios_jit_available()` (probe, cached), `rb_ios_set_jit_enabled()`,
  `rb_ios_session_jit()` (the tier a session actually booted on). The log names the tier too:
  `jit: guest code runs on Tier J (JIT)` or `… Tier I (interpreter)`, plus the reason when J was refused.
- Note for diagnostics: Dynarmic's arm64 back end installs a **Mach exception port** for `EXC_BAD_ACCESS`
  when a JIT is constructed, so on a JIT session a host bad-access fault is the driver's to report rather
  than the app's `signal` handler. Tier I sessions keep the full crash log.
- **Verified:** workflow `41200595797` green on Linux, macOS, iOS arm64 compile and IPA (the Swift target
  compiled too). IPA build 19, commit `f7f6d1b`, 2,187,001 bytes, SHA-256
  `58e6699ebbbc5a884f3b2481a492c17f71d98be5791d9d7020b2544e3ec285c5`.

### 2026-10-31 — iOS S2c: interpreter by default; the AOT note is rediscovered
- The owner changed course on JIT: the switch now starts **off** and the engine's own default matches, so a
  JIT is built only when the user turns it on and the probe passes. The red/green dot still reports
  availability. IPA build 20, commit `4645001`, 2,187,036 bytes, SHA-256
  `d19a2e104b26c4f06dba168f5770826a672d05d1aed34e267b953830d02d5b44`; workflow `41203224126` green.
- He also remembered the no-JIT fast path written down in [`docs/ios-notes.txt`](../ios-notes.txt) §4:
  **"The key idea for Android-on-iOS: AHEAD-OF-TIME translation (no JIT needed)"** — statically translate
  each 32-bit ARM `.so` to AArch64 on the PC, compile the result into the IPA as ordinary signed code, and
  keep a fast fallback interpreter for what the static pass misses. The same note records why it is parked:
  its premise is that the game's native code is **known at build time** (a per-game IPA), and the multi-week
  parts are block discovery (symbols, relocations, exidx, recorded indirect targets), the static backend,
  and the fallback engine; §3 also warns that RetroBug's current Tier I is the "naive kind" of interpreter.
- **Next (no JIT, no product change):** adopt the note's own lesson first — a pre-decoded, direct-threaded
  handler chain in place of the per-op IR switch (iSH measured 3–5x over switch dispatch; iCube rejected an
  IR engine as 1.7x slower). AOT-per-game remains the separate, larger arc and changes how games are built.

### 2026-10-31 — AOT tasks 1–5: offline translation, profiles, and the iOS hook
The no-JIT plan in [`docs/superpowers/plans/2026-10-31-aot-native-speed.md`](../superpowers/plans/2026-10-31-aot-native-speed.md)
is implemented through its iOS integration. Nothing here runs codegen on a device: the emitted code is
assembled into the app like any other source.

- **Patch 0008 (`patches/dynarmic/`).** `AddressSpace::EmitForAot` translates one IR block into a
  caller-owned `oaknut::CodeGenerator` with the JIT's own `EmitConfig`, touching no code cache or block
  map; `GenerateIR` was already public, so nothing else was needed. Byte-for-byte additive: every byte the
  JIT emits is unchanged. The two places the A32 emitter bakes a host address (coprocessor callbacks,
  vector-FP fallback helpers) have no relocation record, so the tool refuses blocks that use them.
- **`rb_aotgen` discovery.** Self-contained ELF32 reader; measured libgame 18,565 starts (export 14,047,
  init 53, reloc 1,232, exidx 14,694, veneer 1,633) and libdlgl 2,897 (1,835 / 22 / 875 / 2,201 / 6).
  Malformed input is bounded (names, tables, dynamic walk), and the test now discriminates the fixes.
- **Offline emitter + runtime.** `--emit` writes `.S` + a sorted block table; `AotExecutor` runs them with
  Dynarmic's prelude ABI, mirrors `JitExecutor`'s state and halt contracts, charges the Tier I fallback
  against the same budget, and falls back rather than entering a block under another IT/FPSCR descriptor.
  The fixture's `fib(0..20)` matches Tier I field for field (178 checks, zero fallbacks); a fallback-only
  image, a budget stop, an IT-state entry and an FPSCR.RMode entry all have discriminating tests. The
  committed fixture pair is byte-identical to a fresh emission, and emissions are deterministic.
- **Indirect-target trace.** `RB_CODEMAP` (or Android's `debug.retrobug.codemap`) records block starts from
  both tiers; the tool merges them. A fixture run contributed 4 new starts over the static map (6 records,
  2 already known), and Tier J recorded 7 over fib(10).
- **Per-game profiles.** `rb_aotgen --profile <libs-dir> --out <prefix>` emits one image per library (32 MiB
  bias slots), hashes each input, and writes the compiled `rb_aot_builtin_profile` plus a JSON manifest.
  Each library is translated in a child process: libstdc++ hits a Dynarmic front-end assertion
  (`SetRegister(PC)` on an encoding decoded from data) and is skipped by name while the rest continue.
  Measured on the KitKat set: libdlgl 34,846 blocks / 7.04 MB, libandroidkitkat 19,615 / 3.81 MB, libc
  18,590 / 3.65 MB (2,507 refused: 1,967 mid-IT, 527 coprocessor), libm 812, libz 937. `AotExecutor` now
  spans a whole profile (a branch into another library's image returns to C++ and enters that image's
  prelude); `GuestProcess` hints the loader at each bias and turns AOT off, naming the module, if one lands
  elsewhere.
- **iOS.** `ios_session` hashes each profiled library from the imported APK and runs AOT only on a full
  match, trying Tier J (when enabled) → AOT → Tier I; `rb_ios_session_engine()` reports 0/1/2. The profile
  is compiled into `librbengine.a` (`RB_AOT_PROFILE_DIR`, exposed as `RB_AOT_PROFILE`), never committed, and
  the IPA workflow takes it as an artifact input. SHA-256 is checked against the FIPS vectors on the device.
- **Gates on SM-X710:** `rb_exec_interp_tests` 1112/0, `rb_exec_tier_diff_tests` 1039/0, `rb_aot_tests`
  178/0, `rb_aotgen_tests` 79/0 + 48/0, `rb_code_map_trace_tests` 60/0, `rb_host_boot_tests` 47/0,
  `rb_sha256_tests` 5/0.
- **Open, owner-gated:** the IPA itself. The APK and its translated profile never leave the owner's
  machines, so the artifact is uploaded to a `workflow_dispatch` run; without it the app builds as a plain
  Tier I IPA. A device boot harness outside the platform host was attempted and dropped: it cannot answer
  OpenSL IIDs or the asset manager without the launcher's host layer, so the measurement belongs on the
  launcher (Android) or the M1 (iOS).
