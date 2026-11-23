# Compatibility table — every 32-bit game on the Tab S9 (M5)

## v0.1.1 iOS developer preview

Android KitKat Challenge reached gameplay in device testing, and classic Gumball reached its menu. The
release adds a portrait play area for classic Gumball and further Unity and Halfbrick startup fixes.
Gameplay on the final IPA remains unverified. Legacy FMOD sound output is incomplete. Exact APK variants
matter, and the Android results below come from a separate set of device tests.

## v0.1.1 Android development on the Galaxy Tab S9

Build 528 (runtime 11) fixes Sonic Racing Transformed’s intro video provider registration. The rebuilt APK
declares both the authority used by the DEX literal and the one assembled from the converted package name.
The earlier copy reported `No content provider` even though its expansion was installed. Play Protect requested
an online scan for the update, so this fix was verified in the conversion report and remains untested in-game.

Sonic & SEGA All-Stars Racing 1.0.1 starts and looks for `main.20.com.sega.ssasr.rb.obb`. Neither the original
nor the converted copy has its expansion on the Tab S9. Build 528 also caps the converted copy at target SDK 30
to prevent the legacy `PendingIntent` crash recorded in the original compatibility survey.

Build 526 (runtime 9) keeps the converted copies of Oreo Twist, Rolling Sky, and Angry Birds Go at target SDK 30
for their older push SDKs. Rolling Sky also gets replacements for two removed Android empty-array fields.
Oreo Twist 1.5.6 reached a timed round. Rolling Sky 2.3.9 reached the Cloud tutorial and responded to swipes.
Angry Birds Go 1.10.1 installed as a separate ARM64 copy and opened its activity but remained on a black screen.
The existing native 1.8.7 install reached the Daily Event dialog, giving a working control on the same device
with a different game version.

Build 527 (runtime 10) adds ARM `lseek` for files supplied by the runtime. Teeny Titans 2 1.1.10 updated in place
and stopped reporting the unsupported seek, but stayed on the Robin loading screen. Its new activity log records
host calls alongside graphics counters. In the captured window, host calls continued with zero EGL buffer swaps
recorded by the bridge. The ARM64 syscall suite passed 263 checks.

Build 522 preserves vertex-buffer and index-buffer offsets when their values overlap mapped guest addresses.
The Tab S9 rendered the expected pixels using both overlapping offsets. The GLES and ABI suites passed 438 and
43 checks. It also logs each successful file-open path once. The recorded Mighty Micros startup had 5,353
successful open lines covering 48 paths, which would produce 48 lines under that rule.

The installed generation 6 Mighty Micros copy reached the garage and rendered its animated scene. The score
panel was blank in four consecutive captures. Gameplay testing with generation 7 is needed to measure the
effect on that HUD issue, the reported flicker in both games, and frame rate.

Build 520 forwards `AKeyEvent_getKeyCode`. Pressing Back in the installed older Mighty Micros copy
faulted at a missing import, which the ARM Unity call site identifies as this function. The new runtime
also records the pre-`sigsuspend` mask in the waking handler's frame so nested Mono signal returns
restore the correct mask. The tablet passed 738 guest-thread, 249 syscall, and 430 graphics/host-slot
checks. The updated launcher was installed in place.
The older Mighty Micros copy showed Robin on a black background in Fast mode, and Safe mode was still
on a black screen after 40 seconds. The tablet was returned to the original Fast setting.

The original Race Day 1.3.2 package on the tablet exits when its Unity JNI proxy sees Android's
three-argument `ServiceConnection.onServiceConnected`. The existing RetroBug copy stays active and
logs DNS failures during sign-in. Race Day contains an offline practice flow, from selecting a game
mode and level to starting a local race, but that menu was not reached in the launch test.

Build 518 fixes the GLES bridge's mapped-buffer tracking when Unity switches between buffers on the same binding
target. GLES version strings and integer version queries agree on the existing ES 3.1 cap. Index-based capability
queries now share guest string pages, and each forwarded call updates its counter without taking the registry mutex.
The Tab S9's driver returned 118 GLES extension strings, which fit on one guest page. The graphics and ABI suites
passed 427 and 43 checks respectively. The allocation result measures graphics initialization, with gameplay
performance still to be measured.

Build 517 shows **Available** for an installed copy that Android can open and **Not available** when a copy is
missing. **Rebuild copy** is a separate action under each game's settings. Conversion details now retain Android's
final installer verdict.

Turbo FAST and Oreo contain duplicate `<uses-sdk>` entries. The converter now merges them. Android's build tools
read both rebuilt APKs as minSdk 24 and targetSdk 33, and both APK signatures verified. Play Protect still refused
installation for both with `INSTALL_FAILED_VERIFICATION_FAILURE`. Race Day and Mighty Micros both rebuilt
successfully, but their new copies reached Play Protect's online scan prompt. The scans were declined and the
previously installed copies were kept.

Build 515 upgrades the launcher in place. Its converted runtime restores ARM's legacy `sigreturn` frames for
handlers installed without `SA_SIGINFO`. RetroBug now logs the first occurrence of a syscall refusal with the
guest resume PC, link register, and thread ID when available. The ARM64 syscall and guest-thread suites passed
249 and 726 checks respectively. The guest-thread suite exercises a real ARMv7 `sigreturn` in the interpreter and JIT.

The new game runs used earlier converted copies. Race Day still requested `api.racedaygame.com`, for which a
separate DNS lookup returned no address. Its log also recorded repeated refusals of legacy `sigreturn`. Mighty
Micros delivered a sensor callback and remained on a black frame at the startup checkpoint. The original Mighty
Micros rendered its garage on the same tablet. Among Us exited after
a SIGILL in `libunity.so` at `+0xf5be4`, a different stop from its earlier EOS crash. Play Protect requested an
online scan for the updated Mighty Micros copy, so the previous copy was retained. Gameplay with runtime generation
4 remains to be verified.

## v0.1.0 Android developer preview

The later Tab S9 sweep covered 31 titles and verified gameplay in Android KitKat Challenge, Fruit Ninja vs. Skittles,
Super Monkey Ball Bounce, and Torture the Murderer: Electric Chair Edition. The remaining titles include games that
reach menus, games blocked by missing content or services, runtime failures, and APKs blocked during installation.
These results cover Android. iOS compatibility depends separately on the game's Java and platform API requirements.

Race Day renders tracks and reports DNS failures for its backend. LEGO Mighty Micros starts and receives sensor
events, but the scene remains black behind Robin. Among Us gets past an EGL frame-timing crash and later fails in
its EOS library. Android Play Protect blocked Oreo and Turbo FAST on the test device.

## Original Android compatibility survey

Built 2026-10-18 from the M5 batch in `build/m5/` (one folder per game: `repack.txt`, `logcat.txt`, `shot1/2.png`,
`final.txt`), plus the re-runs noted below. Device: Samsung Tab S9 (SM-X710, 32-bit capable), RetroBug launcher
`session/m5-m6` @ 2026-10-20 (C26 signal delivery + the ALooper thunks + `rt_sigsuspend`). Each game was converted
from its installed original at its own targetSdk, installed as `<pkg>.rb`, launched, left ~60 s, then the process
state and the first fault were recorded. Rows re-run on 2026-10-20 note it.

**Read the "Evidence" column before trusting a row.** The batch ran while the tablet was in use: several screenshots
show an unrelated app (a classroom video) and two runs were killed by ANRs that the user's own app caused. Those rows
are marked *contaminated* and need a clean re-run.

Tally: 30 games — **12 stayed alive**, **13 died during boot or play**, **4 could not be installed**, **1 could not be
converted** (fixed since: `8b3d22e`, proven on Among Us — 497 MB copy, no OOM).

Engine split: **16 non-Unity, 14 Unity.** Non-Unity: ~9 reach a visible screen, 4–5 playable. Unity: **1 reaches
gameplay — MadTruck, 2026-10-20** (menu, level map, goal screen, level 1 race with working input). The class's wall
was **C26 (no signal delivery)**: Mono's GC suspends a thread with `tkill(tid, SIGPWR)` and aborts when it cannot be
delivered. An earlier reading that "no signal is ever sent" was a scan for `tgkill` (268) that missed `tkill` (238).
The path from there: C26 delivery and nested SIGPWR/SIGXCPU, the `ALooper_pollAll` thunks, `rt_sigsuspend` (179),
and mask-aware futex interrupts.

**2026-10-21 update (C40–C44).** Four engine gaps were found and fixed, each with a device lane and a green gate:
`tgkill(tgid, tid, sig)` was decoded as `tkill(tid, sig)`, so the tid landed in the signal field and **every
Boehm GC suspend aborted with EINVAL** (TT2, then CD2) — the fix alone takes TT2 to its title and CD2 to its
GDPR screen; the **OES-spelled GL 3D entry points** (`glTexImage3DOES`) were missing from the host thunk table,
so Unity stored NULL and `blx`'d it (CD2, ABE, Rolling Sky) — the fix takes all three to a rendered screen;
Mono's **synchronous SIGSEGV delivery** was missing, so a null in JIT-compiled managed code killed the process
instead of becoming a handled `NullReferenceException` (Cars 3); and Dynarmic's A32 front end had no
**VADDHN/VRADDHN/VSUBHN/VRSUBHN** (a real `vaddhn.i16` inside libunity stopped Cars 3 as "unpredictable").
Every Unity game that now renders then waits in the same **Unity startup condvar** (C46) — the next shared wall.
Gate: 23/23 suites on the SM-X710 (tier-diff 1039, guest thread 692, monitor 233; advisory libc 147, APK 1376).

| # | Game | Package | Engine | Result | Where it stops | Evidence |
|---|---|---|---|---|---|---|
| 1 | Angry Birds Go | com.rovio.angrybirdsgo | native | not installed | version downgrade (repack 226 < installed 261) | install log |
| 2 | Angry Birds Go+ | com.rovio.angrybirdsgp | native | died | boot ANR, killed in background | logcat (contaminated) |
| 3 | Angry Birds Epic | com.rovio.gold | Unity (il2cpp) | runs (title) | re-run 2026-10-24: title screen with its 3D background and its own data-consent dialog. The dialog is gated on "Checking internet connection...", which never resolves; taps reach the window but are ignored until then (C48 is not an input defect) | `build/m5/com.rovio.gold/c46-abe.png`; C41/C46 done |
| 4 | Blocky Roads | com.crescentmoongames.blocky_roads | native | **runs** | — (gameplay on screen) | screenshot |
| 5 | Car Game | com.bertheussen.cargame | Unity | not installed | Play Protect verification failure | install log |
| 6 | Cars 3 (Lightning) | com.disney.cars3_goo | Unity (Mono) | runs (black) | re-run 2026-10-24: the unpatched original no longer stalls after C46 — it boots through Crittercism/Kochava/IAP/AES and its bundle check at ~64 M guest instr/s, then sits on black frames while Swrve answers 403 and its IAP list fails (its frames do reach SurfaceFlinger). Remaining wall is its online bootstrap (C37 family) | `build/m5/com.disney.cars3_goo/c46-cars3.png`, `c46-cars3b.png`; C37 open |
| 7 | Crash Drive 2 | com.spilgames.a10.google.crashdrive2 | Unity (IL2CPP) | **runs (menu)** | re-run 2026-10-24 with C46 fixed: past the GDPR dialog, through the Google Play Games picker (cancelled, no account action) and its own rating dialog to the **main menu and the "Welcome Back / enter your name" onboarding** | `build/m5/com.spilgames.a10.google.crashdrive2/c46-cd2e.png`, `c46-cd2-menu.png`; C46 done |
| 8 | Derby Quest | com.tapsy.derbyquesthd | Unity + NativeActivity | died | `ANativeActivity_onCreate` undefined — tile now says so (C31, 0.1.236) | logcat; tile |
| 9 | FC All-Stars | com.turner.fcallstars | Unity + NativeActivity | died | `ANativeActivity_onCreate` undefined — tile now says so (C31, 0.1.236) | logcat; tile |
| 10 | Fruit Ninja vs Skittles | com.halfbrick.fruitninjavsskittles | native | **runs** | — playable | screenshot + played |
| 11 | Gumball Lite | com.twentysixdigital.gumballlite | native | **runs** | — playable | screenshot + played |
| 12 | Gumball's Amazing Party | com.turner.gumballsamazingparty | Unity | died | 2026-10-25: the `libgvradio.so` constructor (guest 0x24c900) jumps to a stack address and faults — a bad function pointer in its obfuscated DRM lib, not the missing-opcode class (C50) | `build/m5/com.turner.gumballsamazingparty/logcat.txt` |
| 13 | Gameloft Cars | com.gameloft.android.ANMP.GloftCAHM | native | died | `ASensorManager_createEventQueue` refuses a guest looper callback | logcat |
| 14 | KitKat Challenge | com.doubleleft.androidkitkat | native | **runs** | — playable | screenshot + played |
| 15 | LEGO Mighty Micros | com.lego.superheroes.dccomicsmightymicros | Unity + NativeActivity | died | `ANativeActivity_onCreate` undefined — tile now says so (C31, 0.1.236) | logcat; tile |
| 16 | Looney Tunes Dash | com.zynga.looney | native | **runs** | — age gate on screen | screenshot |
| 17 | MadTruck | com.spilgames.mtc | Unity (Mono) | **runs (3D)** | — menu, Egypt level map, goal screen and level 1 race render and accept input (2026-10-20); Unity logs non-fatal "Failed to wait on a semaphore (EINTR)" during GC and a `fork` ENOSYS from its crash reporter | screenshots `build/m5/com.spilgames.mtc/mtc-sig2.png` (menu), `mtc-play.png` (level map), `mtc-level1.png` (goals), `mtc-race.png`/`mtc-race2.png` (race); the path: C26 delivery, nested signals, ALooper thunks, `rt_sigsuspend`, mask-aware futex interrupts |
| 18 | Oreo | com.pikpok.oreo | native | not installed | Play Protect verification failure | install log |
| 19 | Rabbids Runner | com.ubisoft.rabbidsrunner | native | **runs** | — splash; legacy-storage permission refused | screenshot |
| 20 | Rolling Sky | com.turbochilli.rollingsky | Unity | **runs (level select)** | re-run 2026-10-24: C46 (guest tids) and C47 (the repack now rewrites the DEX authority literal in place with the manifest rename, digests refreshed) — it reaches its **level-select screen** (Cloud, 0%, 0/10) | `build/m5/com.turbochilli.rollingsky/c47-rs3.png`; C41/C46/C47 done |
| 21 | Sega All-Stars Racing | com.sega.ssasr | native | died | `DemoActivity` fails to start | logcat |
| 22 | SMB Bounce | com.sega.smbbounce | native | **runs** | — title screen with its OBB | screenshot |
| 23 | Sonic Transformed | com.sega.sonic.transformed | native | **runs** | — title screen (needs its 851 MB OBB) | screenshot |
| 24 | Tap Tap Revenge 4 | com.tapulous.taptaprevenge4 | native | died | foreground `PushService` start refused | logcat |
| 25 | Teeny Titans 2 | com.turner.ttgfigures2 | Unity (2018.4, IL2CPP) | **runs (intro)** | re-run 2026-10-24: C46 fixed — `UnityPlayer.nativeRender` gave a *guest* tid to `Process.setThreadPriority`, ART threw and the pending exception left the engine stalled. With tids translated the consent screen renders, `GOT IT!` works, and the city intro scene runs at ~53 M guest instr/s | `build/m5/com.turner.ttgfigures2/tt2-tid.png`, `tt2-tid2..6.png`; C46 done |
| 26 | Torture the Murderer | com.ningunosentertainment.TortureTheMurdererElectric | Unity + NativeActivity | died | `ANativeActivity_onCreate` undefined — tile now says so (C31, 0.1.236) | logcat; tile |
| 27 | Turbo FAST | com.pikpok.turbo | native | not installed | Play Protect verification failure | install log |
| 28 | Nutty Fluffies | com.UBI.A90.WW | Unity | died | re-run 2026-10-18 21:46: `JNI_OnLoad` runs 147,666,044 steps (was stopped at 4,000,000) and faults on an unmapped guest read at 0xc0d900b3, pc 0x2410bc — a real memory fault, no longer the budget | logcat |
| 29 | Where's My Mickey | com.disney.wheresmymickey_goo | native | **runs** | — age gate on screen | screenshot |
| 30 | Among Us | com.innersloth.spacemafia | Unity (il2cpp) | died (engine, further) | re-run 2026-10-25: the EOS crypto probes are now delivered as SIGILL (undefined/UNPREDICTABLE encodings reach the guest handler), so `libEOSSDK.so` finishes every constructor and the renamed Unity player boots. It then dies in the render thread reading `0xbfffffe4` with SIGSEGV blocked (C51) | `build/m5/com.innersloth.spacemafia/au1.png`; C49 done, C51 open |

## Causes, grouped by frequency

| Cause | Games | Tracked as |
|---|---|---|
| NativeActivity games (`ANativeActivity_onCreate`) | 4 | C31 — the tile says so since 0.1.236; running them is still open |
| Unity native init (0x0 fetch, `exit(127)`, unmapped read at 0xc0d900b3, obfuscated ctor) | 5 | per-game (the step budget, C27, is no longer one of them) |
| Thunk gaps (sensor looper callback, thread fetch 0x0) | 2 | C29/C30 |
| Java/Android behaviour (reflection, foreground service, activity start, ANR) | 4 | per-game |
| Install blocked by the device (Play Protect / downgrade) | 4 | needs the human |
| Converter OOM | 1 | fixed in `8b3d22e`, proven |

## What to do next, in order

1. Re-run the contaminated rows (2, 6, 25 and any duplicate screenshot) with the tablet idle; replace their verdicts.
2. ~~The four NativeActivity games look like engine bugs~~ — done 2026-10-18 (C31's cheap half, 0.1.236): the
   launcher activity's own class chain decides, and the four tiles read "Can't play here" with the reason on tap.
   Running them is the expensive half: ANativeActivityCallbacks and the guest-side activity loop.
3. ~~C27 (the 4M-step budget on `JNI_OnLoad`)~~ — done 2026-10-18 (`kInitBudget`). Row 28 now runs 147M steps
   and stops on a real unmapped read; row 3 was never budget-bound (its `JNI_OnLoad` returns in 35 steps). The
   budget was hiding one fault, not four.
4. **Unity's wall is signal delivery, and it is now measured on two games.** Step 1 read eight Unity boots for
   `tgkill` (268), found none, and retired C26; that scan missed **`tkill` (238)**, which is what Mono actually
   suspends a thread with. With C34 landed (`36ce9ac`) Cars 3 dies on nothing but `tkill(tid, 30)` — SIGPWR, the GC
   thread-suspend — followed by SIGABRT and `exit(127)` when delivery is refused; MadTruck reaches the identical
   pair. So **C26 is step one**, and the work is a guest thread taking a delivered signal into the handler it
   installed, not a per-game null pointer. Angry Birds Epic's `libunity.so+0x597b3c` null table entry (row 3) is
   still open behind it, and MadTruck's Java never calling `nativeRender` remains its own per-game issue.
