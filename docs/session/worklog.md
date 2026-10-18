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
- Decision: offline patches neutralize dead online features (ads, analytics, login, update checks, shutdown notices)
  but never bypass purchase or licence verification (Google Play LVL / in-app billing checks are left alone).

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
