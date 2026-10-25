# Compatibility table — every 32-bit game on the Tab S9 (M5)

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
| 30 | Among Us | com.innersloth.spacemafia | Unity (il2cpp) | died (engine) | re-run 2026-10-25: converts (497 MB) and installs, then dies before Unity on `UnsatisfiedLinkError: NativeLoader.load` — because `libEOSSDK.so`'s constructor executes ARMv8-A A32 crypto (`sha1c.32`/`sha256h.32`/`vmull.p64`), which dynarmic cannot decode, so the boot refuses the process (C49) | `build/m5/com.innersloth.spacemafia/au1.png`; C49 open |

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
