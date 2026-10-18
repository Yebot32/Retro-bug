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

