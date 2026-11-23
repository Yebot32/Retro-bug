# Needs you (Yebot32)

Things only you can do or decide. Everything else was decided and logged in worklog.md.

- **SpongeBob Game Station is blocked by its built-in anti-tamper (G-Presto).** Any converted copy is detected as
  tampering and shuts down, so a converted copy cannot start; I stopped there. If you own a build of the
  game without that wrapper, put it in the game roms folder and RetroBug can try it. Details:
  `docs/session/spongebob-gamestation.md`.
- The original SpongeBob APK is now installed on the Tab S9 (untouched, via `adb install
  --bypass-low-target-sdk-block`) and runs until its version check returns 404 — see the game's doc. Uninstall with
  `adb uninstall com.BlueArk.SBGS` when you want it gone.
