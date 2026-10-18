# SpongeBob Game Station (com.BlueArk.SBGS 3.4.0) — result: blocked

## What was done
- APK, OBB and cache zip copied from `/storage/1A7E-2F6D/game roms/Old apk/` to `build/games/sbgs/` (originals
  untouched). The cache zip only holds the same OBB.
- The game is Unity 5 / Mono (libmain, libunity, libmono), armeabi-v7a + x86.
- Installing the APK as-is is refused by Google Play Protect on the Tab S9 ("built for an older version of
  Android"); that was not bypassed. Nothing was left installed.
- A RetroBug diagnostic copy (`.rb`, run natively, since this tablet still runs 32-bit code) was used to see the
  game's own behaviour. On the way, two general RetroBug fixes came out of it (both committed, both useful for other
  games): libraries whose ELF section-header fields are zeroed are now repaired so Android loads them (also hit
  libgpg.so), and the launcher no longer dies when a copy's process crashes while the Data tab asks it something.
- The copy then reaches the game's start-up, and the game's protection SDK ("G-Presto", bundled in this APK)
  stops it with: *"[Error_1160] Illicit activity detected. Game will shutdown in 8 seconds … Protected by
  G-Presto."* and exits.

## Root cause
The APK carries a commercial anti-tamper system (G-Presto by Bishopsoft: `libATG_L.so`, `assets/ATG_E.sec`, and
encrypted C# assemblies). It treats any re-signed or converted copy as tampering. Every RetroBug copy is re-signed
(it has to be: the package is renamed and the native code is replaced by RetroBug's runtime), so the protection
refuses to run.

## Why it stops here
Getting past this would mean defeating the game's copy protection. That is out of bounds for this project and
was not attempted. The offline-error question (server shutdown, dates, etc.) could not be reached: the game never
gets past the protection's check in a converted copy.

Also relevant for later: under RetroBug's emulator this game additionally needs Unity support (dynamic loading of
its sibling libraries, and signal delivery for Mono's garbage collector), which RetroBug does not have yet
(backlog C25/C26). That part is engine work and is tracked on its own.

## What could unblock it (needs you)
- A build of the game without the G-Presto wrapper, which you are entitled to use (for example the original
  publisher's release, if you have one), would avoid the protection entirely; RetroBug could then convert it.

## Files left
- `build/games/sbgs/` (local, git-ignored): the copied APK/OBB and diagnostic outputs.
- No SpongeBob package or OBB folder remains on the Tab S9; the test OBB copy pushed to Download was removed.
