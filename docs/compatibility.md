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
gameplay** — they boot, then stop in native init or stay alive without drawing. That is the C26 gap (Mono's GC needs
signals) plus JIT-shaped code, not a per-game bug.

| # | Game | Package | Engine | Result | Where it stops | Evidence |
|---|---|---|---|---|---|---|
| 1 | Angry Birds Go | com.rovio.angrybirdsgo | native | not installed | version downgrade (repack 226 < installed 261) | install log |
| 2 | Angry Birds Go+ | com.rovio.angrybirdsgp | native | died | boot ANR, killed in background | logcat (contaminated) |
| 3 | Angry Birds Epic | com.rovio.gold | Unity | died | `nativeRender` fetched address 0x0 | logcat |
| 4 | Blocky Roads | com.crescentmoongames.blocky_roads | native | **runs** | — (gameplay on screen) | screenshot |
| 5 | Car Game | com.bertheussen.cargame | Unity | not installed | Play Protect verification failure | install log |
| 6 | Cars 3 (Lightning) | com.disney.cars3_goo | Unity | died | guest native called `exit(127)` | logcat (screenshot contaminated) |
| 7 | Crash Drive 2 | com.spilgames.a10.google.crashdrive2 | Unity | ran, then died | guest thread fetched 0x0 | logcat |
| 8 | Derby Quest | com.tapsy.derbyquesthd | Unity + NativeActivity | died | `ANativeActivity_onCreate` undefined | logcat |
| 9 | FC All-Stars | com.turner.fcallstars | Unity + NativeActivity | died | `ANativeActivity_onCreate` undefined | logcat |
| 10 | Fruit Ninja vs Skittles | com.halfbrick.fruitninjavsskittles | native | **runs** | — playable | screenshot + played |
| 11 | Gumball Lite | com.twentysixdigital.gumballlite | native | **runs** | — playable | screenshot + played |
| 12 | Gumball's Amazing Party | com.turner.gumballsamazingparty | Unity | died | undefined instruction in `libgvradio.so` ctor (obfuscated/DRM lib) | logcat |
| 13 | Gameloft Cars | com.gameloft.android.ANMP.GloftCAHM | native | died | `ASensorManager_createEventQueue` refuses a guest looper callback | logcat |
| 14 | KitKat Challenge | com.doubleleft.androidkitkat | native | **runs** | — playable | screenshot + played |
| 15 | LEGO Mighty Micros | com.lego.superheroes.dccomicsmightymicros | Unity + NativeActivity | died | `ANativeActivity_onCreate` undefined | logcat |
| 16 | Looney Tunes Dash | com.zynga.looney | native | **runs** | — age gate on screen | screenshot |
| 17 | MadTruck | com.spilgames.mtc | Unity | ran, black screen | nothing drawn after boot | screenshot |
| 18 | Oreo | com.pikpok.oreo | native | not installed | Play Protect verification failure | install log |
| 19 | Rabbids Runner | com.ubisoft.rabbidsrunner | native | **runs** | — splash; legacy-storage permission refused | screenshot |
| 20 | Rolling Sky | com.turbochilli.rollingsky | Unity | died | `NoSuchFieldError libcore.util.EmptyArray.INT` | logcat |
| 21 | Sega All-Stars Racing | com.sega.ssasr | native | died | `DemoActivity` fails to start | logcat |
| 22 | SMB Bounce | com.sega.smbbounce | native | **runs** | — title screen with its OBB | screenshot |
| 23 | Sonic Transformed | com.sega.sonic.transformed | native | **runs** | — title screen (needs its 851 MB OBB) | screenshot |
| 24 | Tap Tap Revenge 4 | com.tapulous.taptaprevenge4 | native | died | foreground `PushService` start refused | logcat |
| 25 | Teeny Titans 2 | com.turner.ttgfigures2 | Unity | died | guest native called `exit(127)` | logcat (screenshot contaminated) |
| 26 | Torture the Murderer | com.ningunosentertainment.TortureTheMurdererElectric | Unity + NativeActivity | died | `ANativeActivity_onCreate` undefined | logcat |
| 27 | Turbo FAST | com.pikpok.turbo | native | not installed | Play Protect verification failure | install log |
| 28 | Nutty Fluffies | com.UBI.A90.WW | Unity | died | `JNI_OnLoad` exceeded the 4M-step budget | logcat |
| 29 | Where's My Mickey | com.disney.wheresmymickey_goo | native | **runs** | — age gate on screen | screenshot |
| 30 | Among Us | com.innersloth.spacemafia | Unity (il2cpp) | converted, untested | conversion used to OOM; now converts (497 MB) | repack log 20:13 |

## Causes, grouped by frequency

| Cause | Games | Tracked as |
|---|---|---|
| NativeActivity games (`ANativeActivity_onCreate`) | 4 | C31 — tile should say "not supported" |
| Unity native init (budget, 0x0 fetch, `exit(127)`, obfuscated ctor) | 5 | C27 + per-game |
| Thunk gaps (sensor looper callback, thread fetch 0x0) | 2 | C29/C30 |
| Java/Android behaviour (reflection, foreground service, activity start, ANR) | 4 | per-game |
| Install blocked by the device (Play Protect / downgrade) | 4 | needs the human |
| Converter OOM | 1 | fixed in `8b3d22e`, proven |

## What to do next, in order

1. Re-run the contaminated rows (2, 6, 25 and any duplicate screenshot) with the tablet idle; replace their verdicts.
2. The four NativeActivity games are one fix: a clear "not supported yet" tile, or the `ANativeActivity_onCreate`
   stub. The tile message is the cheap half and stops these looking like engine bugs.
3. C27 (the 4M-step budget on `JNI_OnLoad`) is a single-cause Unity failure; raise or remove the budget for
   initialisation and re-test rows 3, 6, 25, 28.
4. Unity as a class stays blocked on C26 (Mono GC signals) — that is the engine milestone, not this table.
