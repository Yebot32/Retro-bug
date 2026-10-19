# Compatibility table — every 32-bit game on the Tab S9 (M5)

Built 2026-10-18 from the M5 batch in `build/m5/` (one folder per game: `repack.txt`, `logcat.txt`, `shot1/2.png`,
`final.txt`), plus the re-runs noted below. Device: Samsung Tab S9 (SM-X710, 32-bit capable), RetroBug launcher
`session/m5-m6` @ `8b3d22e`. Each game was converted from its installed original at its own targetSdk, installed as
`<pkg>.rb`, launched, left ~60 s, then the process state and the first fault were recorded.

**Read the "Evidence" column before trusting a row.** The batch ran while the tablet was in use: several screenshots
show an unrelated app (a classroom video) and two runs were killed by ANRs that the user's own app caused. Those rows
are marked *contaminated* and need a clean re-run.

Tally: 30 games — **11 stayed alive**, **14 died during boot or play**, **4 could not be installed**, **1 could not be
converted** (fixed since: `8b3d22e`, proven on Among Us — 497 MB copy, no OOM).

Engine split: **16 non-Unity, 14 Unity.** Non-Unity: ~9 reach a visible screen, 4–5 playable. Unity: **0 reach
gameplay** — they boot, then stop in native init or stay alive without drawing. Measured 2026-10-19, the class's
single confirmed wall is **C26 (no signal delivery)**: Mono's GC suspends a thread with `tkill(tid, SIGPWR)` and
aborts when it cannot be delivered. An earlier reading that "no signal is ever sent" was a scan for `tgkill` (268)
that missed `tkill` (238); two games now name the same call.

| # | Game | Package | Engine | Result | Where it stops | Evidence |
|---|---|---|---|---|---|---|
| 1 | Angry Birds Go | com.rovio.angrybirdsgo | native | not installed | version downgrade (repack 226 < installed 261) | install log |
| 2 | Angry Birds Go+ | com.rovio.angrybirdsgp | native | died | boot ANR, killed in background | logcat (contaminated) |
| 3 | Angry Birds Epic | com.rovio.gold | Unity (il2cpp) | died | `nativeRender` calls a null function pointer: `ldr lr, [r4, #0xcc]; blx lr` at `libunity.so+0x597b3c` | logcat 2026-10-18 23:01 with the new stop report: boot completes (370 constructors), GL context is real (Unity logs the Adreno extension list), then the null call. The 12 unanswered `libunity.so` imports are answered since C29's sensor half landed, and the two weak `__google_potentially_blocking_region_*` are no longer 0 |
| 4 | Blocky Roads | com.crescentmoongames.blocky_roads | native | **runs** | — (gameplay on screen) | screenshot |
| 5 | Car Game | com.bertheussen.cargame | Unity | not installed | Play Protect verification failure | install log |
| 6 | Cars 3 (Lightning) | com.disney.cars3_goo | Unity (Mono) | died | re-run 2026-10-19 05:05: boot completes (316 constructors, real GL context), every syscall refusal gone, then `tkill(tid, 30)` — SIGPWR, Mono's GC suspend — is refused, the runtime aborts (SIGABRT) and calls `exit(127)` | logcat; C34 landed, C26 is now the wall |
| 7 | Crash Drive 2 | com.spilgames.a10.google.crashdrive2 | Unity | ran, then died | guest thread fetched 0x0 | logcat |
| 8 | Derby Quest | com.tapsy.derbyquesthd | Unity + NativeActivity | died | `ANativeActivity_onCreate` undefined — tile now says so (C31, 0.1.236) | logcat; tile |
| 9 | FC All-Stars | com.turner.fcallstars | Unity + NativeActivity | died | `ANativeActivity_onCreate` undefined — tile now says so (C31, 0.1.236) | logcat; tile |
| 10 | Fruit Ninja vs Skittles | com.halfbrick.fruitninjavsskittles | native | **runs** | — playable | screenshot + played |
| 11 | Gumball Lite | com.twentysixdigital.gumballlite | native | **runs** | — playable | screenshot + played |
| 12 | Gumball's Amazing Party | com.turner.gumballsamazingparty | Unity | died | undefined instruction in `libgvradio.so` ctor (obfuscated/DRM lib) | logcat |
| 13 | Gameloft Cars | com.gameloft.android.ANMP.GloftCAHM | native | died | `ASensorManager_createEventQueue` refuses a guest looper callback | logcat |
| 14 | KitKat Challenge | com.doubleleft.androidkitkat | native | **runs** | — playable | screenshot + played |
| 15 | LEGO Mighty Micros | com.lego.superheroes.dccomicsmightymicros | Unity + NativeActivity | died | `ANativeActivity_onCreate` undefined — tile now says so (C31, 0.1.236) | logcat; tile |
| 16 | Looney Tunes Dash | com.zynga.looney | native | **runs** | — age gate on screen | screenshot |
| 17 | MadTruck | com.spilgames.mtc | Unity (Mono) | died | re-run 2026-10-19 05:11: boots clean, draws its own mod dialog and holds it; on Close it reaches the same `tkill(tid, 30)` (SIGPWR) refusal as row 6, then SIGABRT and `exit(127)`. Its `libunity.so` is down to 2 unanswered imports (`ANativeWindow_lock`/`unlockAndPost`) | logcat + screenshot; the dialog is the mod's own ("Credits for mod to Tiger"), not an engine fault |
| 18 | Oreo | com.pikpok.oreo | native | not installed | Play Protect verification failure | install log |
| 19 | Rabbids Runner | com.ubisoft.rabbidsrunner | native | **runs** | — splash; legacy-storage permission refused | screenshot |
| 20 | Rolling Sky | com.turbochilli.rollingsky | Unity | died | `NoSuchFieldError libcore.util.EmptyArray.INT` | logcat |
| 21 | Sega All-Stars Racing | com.sega.ssasr | native | died | `DemoActivity` fails to start | logcat |
| 22 | SMB Bounce | com.sega.smbbounce | native | **runs** | — title screen with its OBB | screenshot |
| 23 | Sonic Transformed | com.sega.sonic.transformed | native | **runs** | — title screen (needs its 851 MB OBB) | screenshot |
| 24 | Tap Tap Revenge 4 | com.tapulous.taptaprevenge4 | native | died | foreground `PushService` start refused | logcat |
| 25 | Teeny Titans 2 | com.turner.ttgfigures2 | Unity | died | guest native called `exit(127)` | logcat (screenshot contaminated) |
| 26 | Torture the Murderer | com.ningunosentertainment.TortureTheMurdererElectric | Unity + NativeActivity | died | `ANativeActivity_onCreate` undefined — tile now says so (C31, 0.1.236) | logcat; tile |
| 27 | Turbo FAST | com.pikpok.turbo | native | not installed | Play Protect verification failure | install log |
| 28 | Nutty Fluffies | com.UBI.A90.WW | Unity | died | re-run 2026-10-18 21:46: `JNI_OnLoad` runs 147,666,044 steps (was stopped at 4,000,000) and faults on an unmapped guest read at 0xc0d900b3, pc 0x2410bc — a real memory fault, no longer the budget | logcat |
| 29 | Where's My Mickey | com.disney.wheresmymickey_goo | native | **runs** | — age gate on screen | screenshot |
| 30 | Among Us | com.innersloth.spacemafia | Unity (il2cpp) | converted, untested | conversion used to OOM; now converts (497 MB) | repack log 20:13 |

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
