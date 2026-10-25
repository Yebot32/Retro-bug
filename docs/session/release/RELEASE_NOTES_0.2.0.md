# RetroBug 0.2.0 — release notes

Built 2026-10-25 from `session/m5-m6`. This is the first build meant to be handed to someone else.

## What it is

RetroBug is a launcher that runs **32-bit (armeabi-v7a) Android games on 64-bit-only phones**. It lists the
32-bit games installed on the device, converts a game into a playable copy that installs *beside* the
original (package suffix `.rb`), and runs that copy under a 32-bit ARM runtime — an interpreter plus a
translator (tiers I and J) with a host bridge for JNI, GLES, EGL, libc and the file system. The original
app is never modified or removed.

## Build

| | |
|---|---|
| APK | `build/launcher-apk/rbguest-launcher.apk` (2,659,673 bytes) |
| versionCode / versionName | `264` / `0.2.0` |
| minSdk / targetSdk | 26 / 34 |
| ABI | arm64-v8a launcher, 32-bit guest bytes carried as assets |
| SHA-256 | `7e268510a307aff82f1220f614f9dc42cd8969f0933bfd68b7bfc580262f6ae8` |

Built with `RB_RELEASE=1 RB_VERSION_NAME=0.2.0 bash tools/build_launcher_apk.sh`. Release means: not
debuggable, and the `-e` diagnostic intents are refused. A dev build (`bash tools/build_launcher_apk.sh`)
keeps them for field diagnosis. A rebuild is not byte-identical (ZIP entry timestamps), so check the hash
of the file you actually ship rather than of a previous build.

## Install

1. Copy the APK to the phone and open it (or `adb install -r -g rbguest-launcher.apk`).
2. Grant "install unknown apps" when Android asks — the launcher needs it to install converted copies.
3. Open **RetroBug**. The library lists every installed 32-bit game with a state: *Ready*, *Update
   available* (a converted copy exists and the original changed), *Can't play here* (32-bit native app
   the runtime cannot host), or *Not converted*. Tap a game, choose **Convert**, then **Play**.

Converted copies are normal apps under `<package>.rb`. Removing one from the launcher uses Android's own
uninstall prompt; the original is untouched.

## Where it stands (device-verified on a Galaxy Tab S9, SM-X710)

Working end to end: **Teeny Titans 2** (intro city), **Crash Drive 2** (main menu, name entry), **Angry Birds
Epic** (title + consent dialog), **Rolling Sky** (level select), **MadTruck** (menu), plus the earlier
titles in `docs/compatibility.md` (SMB Bounce, FN Skittles, Sonic Racing Transformed with its OBB, …).

Known limits, by name:

* **NativeActivity titles** (Derby Quest, FC All-Stars, LEGO Mighty Micros, Torture the Murderer) are marked
  *Can't play here* — the runtime has no `ANativeActivity` host yet (C31).
* **Among Us** converts and boots its Unity player but dies in the render thread (C51).
* **Cars 3** boots at ~64 M guest instr/s and stays on black frames while its online bootstrap fails (C37).
* **Gumball's Amazing Party** dies in its DRM library (C50); **Nutty Fluffies** on a bad guest read.
* Games whose checks need Google Play services (licensing, Play Games sign-in) may stop at that check.
* No network is required for the launcher itself; individual games may need their own connection.

## Signing

Local builds sign with the repository's test keystore (`build/keys/rb-test.keystore`, password in the build
script) unless `RB_KEYSTORE`, `RB_KEYSTORE_PASS` and `RB_KEY_ALIAS` are set. **Publish only with your own
key**: CI falls back to the test key when `RB_KEYSTORE_B64` is absent and says so in the run log (D4), and
a throwaway key cannot update an installed copy.

## What changed since 0.1

The M5/M6 arc: the JNI bridge (nested writes with a real exception check, thread-state access, fd reads,
virtual calls), GLES 1/2/3 thunks incl. OES 3D aliases, EGL window bridge, SysV shared memory, in-memory
byte patches/mod loader, save import/export, resolution scaling, the game menu/library UI, and — this
session — guest-tid translation for Java thread APIs (C46), a DEX authority rewrite at repack (C47), and
SIGILL delivery for encodings the emulated CPU does not define (C49).

## Files

* `docs/compatibility.md` — every tested title and what it does.
* `docs/session/backlog.md` — open items, each with where and how it was measured.
* `NEXT-AI-HANDOFF.txt` — the shortest path into the engine for whoever picks this up next.
