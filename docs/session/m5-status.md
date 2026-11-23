# M5 status — where the session stopped (2026-10-18, 17:40)

Branch `session/m5-m6` at `8b3d22e` (27 commits ahead of `main`, working tree clean except the untracked
`NEXT-AI-HANDOFF.txt`). Tablet `R52Y106BECZ` awake, unlocked, RetroBug launcher in focus. OnePlus not connected.

## Done before the stop

- **M6** (patches, mods, aspect, render scale) implemented and device-verified on Gumball (worklog, 2026-10-18).
- **Converter OOM fix** (`8b3d22e`): entry copies stream instead of buffering whole; libraries read into
  exactly-sized arrays; `LargeEntryTest` runs the converter in a 96 MB JVM. **Committed, not installed** — the
  launcher on the tablet is the 16:08 build; the fix landed at 16:15, after the batch started.
- **M5 batch**: all 30 installed 32-bit games converted and launched on the tablet; results in `build/m5/<pkg>/`
  (`final.txt`, `logcat.txt`, `repack.txt`, `shot1/2.png`), last line `M5B-ALL-DONE` at 16:41.

## M5 tally (30 games attempted)

| Outcome | Count | Notes |
|---|---|---|
| Installed, process alive at the end of the window | 11 | 9 of them are non-Unity; 4–5 confirmed on a real screen |
| Installed, died during boot or gameplay | 14 | causes below |
| Install blocked by the device | 4 | 3 × Play Protect `INSTALL_FAILED_VERIFICATION_FAILURE` (Oreo, Turbo, Car Game); ABGo = version downgrade 226 < 261 |
| Conversion failed | 1 | Among Us OOM (fixed in `8b3d22e`, needs an on-device re-run) |

Engine split: **16 non-Unity, 14 Unity.** Every Unity game that got as far as running either died in native init
or stayed alive without reaching gameplay — the known structural gap (Mono needs signals, C26; plus JIT).
Non-Unity: ~9 of 16 reach a visible screen, 4–5 are playable.

## Failure taxonomy (14 deaths)

| Cause | Games | Where it is |
|---|---|---|
| NativeActivity games: `undefined symbol: ANativeActivity_onCreate` | Derby Quest, FC All-Stars, LEGO Super Heroes, Torture the Murderer | C31, deferred; tile should say "not supported" |
| Unity native init | UBI A90 (`JNI_OnLoad` over the 4M-step budget, C27); Angry Birds Gold (`nativeRender` fetched 0x0); Cars 3 + TTG Figures 2 (guest `exit(127)`); Gumball's Amazing Party (undefined instruction in `libgvradio.so`'s constructor) | the last one is where the session stopped |
| Thunk gaps | Gameloft Cars (`ASensorManager_createEventQueue` refuses a guest `ALooper` callback, C29); Crash Drive 2 (thread fetches 0x0) | M5 evidence for C29/C30 |
| Java/Android behaviour | Rolling Sky (`NoSuchFieldError libcore.util.EmptyArray.INT`); Tap Tap Revenge 4 (foreground `PushService`); Sega SSASR (`DemoActivity` start); Angry Birds Go+ (boot ANR) | per-game; some are targetSdk-33 rules |

## Two things the next session must know

1. **The batch screenshots are contaminated.** Several `shot2.png` files show a classroom lesson being watched on
   the tablet (Cars 3, Angry Birds Gold; duplicate groups across 12 games). The tablet was in use while the batch
   ran, which also explains the Angry Birds Go+ `bg anr`. Crash evidence from `logcat`/`rb-report` is still valid;
   "alive=yes" and screenshots for those games are not. Re-run the affected games with the tablet left alone.
2. **`docs/session/backlog.md` is stale.** Rows already fixed by the M6 work: C10 (OOM — `8b3d22e`), C19 ("coming
   with M6"), C24 (settings parsing — `93d258c`), C25 (dlopen/dlsym — `44096c0`). A4 is listed `doing`; A5/M6 is
   listed `todo` but is done and device-verified.

## Next, in order

1. Build + install the launcher at `8b3d22e`; re-convert Among Us on the tablet — the OOM case cannot be
   reproduced on the host, so this is the only real proof of the fix.
2. Re-run the contaminated games (Cars 3, Angry Birds Gold, Angry Birds Go+, and the ones whose screenshots are
   duplicates) with the tablet idle; then write `docs/compatibility.md` — the M5 deliverable.
3. Fix by frequency, cheapest first: the NativeActivity tile message (4 games), C27's step budget, `exit(127)`
   (2 games), the EmptyArray reflection failure.
4. Reconcile `backlog.md`, then A6 polish and A7 release prep (D1–D7: versionCode, `d8 --release`, icon,
   manifest typo, CI notes).
