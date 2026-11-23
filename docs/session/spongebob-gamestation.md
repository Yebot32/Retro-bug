# SpongeBob Game Station (com.BlueArk.SBGS 3.4.0) — conversion blocked; original runs, dies at a 404

> Update 2026-10-18 18:57: the untouched APK does install on the Tab S9 with `adb install --bypass-low-target-sdk-block`
> and runs — G-Presto passes — then the game closes itself right after its version check returns `404`. That is the
> real failure; see "Run of the untouched APK". The protection only blocks the *converted* copy.

## What was done
- APK, OBB and cache zip copied from `/storage/1A7E-2F6D/game roms/Old apk/` to `build/games/sbgs/` (originals
  untouched). The cache zip only holds the same OBB.
- The game is Unity 5 / Mono (libmain, libunity, libmono), armeabi-v7a + x86.
- Installing the APK as-is was refused by Google Play Protect on the Tab S9 ("built for an older version of
  Android"). That block is the platform's old-targetSdk rule; it was later bypassed with the documented
  `adb install --bypass-low-target-sdk-block` flag (the APK itself untouched).
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
A converted copy is rejected by the game's own integrity check: G-Presto treats a re-signed APK with replaced native
libraries as modified and shuts it down. RetroBug does not alter that check, so a converted copy cannot start. The
offline-error question (server shutdown, dates, etc.) could not be reached: the game never gets past the check in a
converted copy.

Also relevant for later: under RetroBug's emulator this game additionally needs Unity support (dynamic loading of
its sibling libraries, and signal delivery for Mono's garbage collector). The dlopen half landed later the same day
(`44096c0`); signal delivery for Mono's GC (C26) is still missing, so even without the protection this game could
not reach its menu yet.

## What G-Presto actually is (strings from the APK, 2026-10-18 18:33)

`lib/armeabi-v7a/libATG_L.so` (126 KB, section headers zeroed — the same trick `ElfRepair` already handles) is a
loader for `assets/ATG_E.sec` (294 KB, the encrypted protection engine). Both are packed, but their symbol and
message tables are readable:

- `com/bishopsoft/Presto/SDK/Presto` — Bishopsoft's commercial app-protection SDK; the `<Presto_L>` tag and
  `G-Presto load failed : 1001/1002/1003/2001/2003` are its fail-closed load paths.
- `[Error_%d] 3rd party apps that modify game data are not supported. Shutting down in 8 seconds.` and
  `Please remove all third party "patcher" apps before running this application. Protected by Presto`.
- Check variables in `ATG_E.sec`: `g_signature`, `g_cmpSign`, `g_signChecked`, `g_config_signature` (APK signature
  comparison), `g_rootingChecked`, `g_config_rooting` (root check), `g_min_mtime`/`g_max_mtime`/`g_sys_mtime`
  (file-timestamp bounds), plus a `webhttp` client and `AlertDetect`.

So the failure we see is an **integrity/anti-tamper** verdict — "this APK was modified" — not a network, server or
date check. The game's own online error is a different, later failure that no converted copy ever reaches.

## Re-checked 2026-10-18 18:25 (what is confirmed)

1. **The APK is genuine, so the error is RetroBug's doing, not a pirated copy.** Signer: `O=BlueArk`,
   SHA-256 `b9646db3…985b5e`; certificate valid 2015-11-23 → **2065-11-10**, so an expired certificate is ruled out.
   G-Presto ships inside the publisher's own build: `lib/armeabi-v7a/libATG_L.so`, `assets/ATG_E.sec`,
   `assets/ATG_E_x86.sec`, `libsimpleencryption.so`.
2. **The original does install and run on the Tab S9** with the documented adb flag
   `adb install --bypass-low-target-sdk-block` (the platform's old-targetSdk block, not a security prompt). The APK is
   untouched and BlueArk-signed, so G-Presto passes. See "Run of the untouched APK" below — this is the first time the
   game's own behaviour was observable, and it changes the picture.
3. **Every RetroBug copy trips the protection.** Conversion renames the package, replaces the native libraries with
   RetroBug's stub and re-signs; G-Presto treats that as tampering (`[Error_1160] Illicit activity detected`, exit
   after 8 s). That is a copy-protection check, not an offline/server check, so a converted copy cannot start.

Consequence: the user's symptom is now reproduced — not by a converted copy, but by the untouched game itself (below).

## Run of the untouched APK (2026-10-18 18:57, Tab S9)

Installed with `adb install --bypass-low-target-sdk-block` (the platform's old-targetSdk block; the APK is unmodified),
OBB pushed to `/sdcard/Android/obb/com.BlueArk.SBGS/main.9.com.BlueArk.SBGS.obb`, launched with
`am start -n com.BlueArk.SBGS/.MainActivity`. Log evidence:

- `<Presto_E>: G-Presto Engine initialization completed` — the protection is satisfied by the genuine signature.
- Unity boots and runs: `SendLocalPush`, `firebase LogEvent, versioncheck_start`, then
  **`E Unity: wf, 404: Not Found`**, then `versioncheck_completed`.
- About a second later the game stops itself: `PlayerBase::stop()`, audio `AudioTrack: stop`, the SurfaceView is
  destroyed and the activity is paused/stopped (`userLeaving=true`), returning to the launcher. No crash, no ANR,
  no dialog, no exception in logcat.
- Wi-Fi was up (SSID `ams`), but the ad SDKs could not resolve their hosts (`api.vungle.com`, `impact.applifier.com`).

So the game reaches its own startup, calls a version-check endpoint that answers **404**, and then closes. That is
very likely the "error and won't continue" the user remembers, and it is a server-side check — not the protection.

URLs found in `classes.dex` that belong to the game rather than to ad SDKs:
`http://restapi.fs.ncloud.com/presto-update1/`, `http://s3-ap-northeast-1.amazonaws.com/presto-update1/files/`
(Presto's own update channel), and a leftover developer endpoint `http://192.168.0.6:8888/ADController` /
`ADRedirectController`. No BlueArk game-server URL is visible in plain form — it is most likely built from strings in
the (encrypted) C# assemblies.

**Next step, if this is picked up again:** capture the actual request. The app targets SDK 22, so it trusts
user-installed CAs; a system Wi-Fi proxy plus mitmproxy would show the URL and the expected body, and the check could
then be answered locally (a local HTTP response, no APK change). That is the "give the game a local response" route
and it does not touch the APK at all.

## 4.9.0 (apkpure build, 2019-07-16) — no wrapper, but a new crash (2026-10-18 19:12)

`build/games/sbgs/sbgs-4.9.0.apk`: versionCode 56, versionName 4.9.0, minSdk 15, **targetSdk 25**, same BlueArk
certificate (SHA-256 `b9646db3…985b5e`), Unity 5 / Mono, armeabi-v7a + x86. **No `libATG_L.so`, no `ATG_E.sec`** —
the G-Presto wrapper is not in this build, so a converted or modified copy has no integrity check to trip. Assets are
inside the APK (no OBB needed).

Installed and launched on the Tab S9; it dies before Unity starts:

```
FATAL EXCEPTION: main
java.lang.NoSuchMethodError: public default void android.content.ServiceConnection.onServiceConnected(
    android.content.ComponentName, android.os.IBinder, android.app.IBinderSession)
    at bitter.jnibridge.JNIBridge.invoke(Native Method)
    at $Proxy4.onServiceConnected(Unknown Source)
    at android.app.LoadedApk$ServiceDispatcher.doConnected(LoadedApk.java:2517)
```

Android 16 added a 3-argument `onServiceConnected(..., IBinderSession)` default method to `ServiceConnection`. Unity
IAP's `bitter.jnibridge` proxy (the game binds the billing service at startup) was compiled against the 2-argument
form and throws `NoSuchMethodError` when the framework invokes the new one. This is a platform-compat failure, not the
network error, and it happens on any current Android — including the OnePlus.

Order of work is therefore: fix this IAP/ServiceConnection crash first (dex-level, no protection involved), then the
version check.

### Result: a working modified APK (2026-10-18 19:40)

`build/sbgs-4.9.0-mod.apk` (101 MB, signed with a local key) boots to the main menu and plays: match-3 round and the
run game both launched, on the Tab S9. Three edits, all reversible, none touching a protection system:

1. **`bitter/jnibridge/JNIBridge$a.invoke` (smali)** — Android 16 calls `onServiceConnected(ComponentName, IBinder,
   IBinderSession)`; the handler now detects that 3-argument call and forwards it to the 2-argument method. Without
   this the app died at startup with `NoSuchMethodError`. (First attempt failed the dex verifier — `aget-object` on an
   `Object[]` needs an explicit `check-cast` before `invoke-interface`; the log said so plainly.)
2. **Manifest** — `ACCESS_FINE_LOCATION` and `ACCESS_COARSE_LOCATION` removed (Yebot32 asked for this), and
   `com.android.vending.BILLING` removed with them: billing cannot work for a delisted game, and removing it changes
   nothing the game needs.
3. **`Assembly-CSharp.dll`, `NexusIAPManager::.ctor`** — one byte: `ldc.i4.1` → `ldc.i4.0` for the `isWorking`
   initializer (file offset 1491478). `HomeLoadingSceneController.ReadyCommonShopCoroutine` does
   `while (CommonShopManager.IsWorking) yield return null;`, and `isWorking` only clears in `OnInitialized` /
   `OnInitializeFailed`. Unity IAP never reaches either (the store answers `6:Error` and retries forever, 20 s → 40 s
   → 80 s), so the loading screen waited forever. Starting `isWorking` false lets the loop exit; `IsReady` stays false,
   so the shop treats IAP as unavailable instead of calling a null controller.

What the game does *not* need: no network patch. 4.9.0's content download (15 files) still works — the CDN is alive.
The 404 seen in 3.4.0 was that older build's version check; 4.9.0 never reached it, because the IAP wait blocked the
loading scene first.

Tooling used (all under `tools/`, git-ignored): apktool 2.11.1 (`tools/bin/apktool.jar`), ilspycmd 9.1.0 (run with the
installed .NET 8 runtime), `dnfile` in the Python venv for the metadata offset, plus the SDK's `zipalign`/`apksigner`.
Signing key: `build/sbgs-490-key.keystore` (pass `retrobug`, alias `sbgs`). Decompiled tree:
`build/sbgs-490-src/`; decompiled C#: `build/sbgs-490-decomp/`.

### Infinite hearts and tickets (2026-10-18 20:05) — `build/sbgs-4.9.0-infinite.apk`

Four IL byte patches in `Assembly-CSharp.dll`, each replacing a tiny method body (the tiny header's size field is
updated too, so the leftover bytes are outside the body):

| Method | File offset | Was | Now |
|---|---|---|---|
| `PxUser::get_heart` | `0x207ed` | `ldarg.0; ldfld m_heart; ret` | `ldc.i4.s 99; ret` |
| `PxUser::IsEnoughHeart` | `0x207df` | `m_heart >= amount` | `ldc.i4.1; ret` |
| `PxUser::UseHeart` | `0x207bf` | `m_heart -= amount` | `ret` (no-op) |
| `CurrencyKey::get_UserValue` | `0x1b2a2` | `User.Instance.key` | `ldc.i4.s 99; ret` |

Verified on the Tab S9: the Pop stage select shows a heart with **99 / Full**, and the Bikini Bottom badge shows
**99** (it was 10). Stage 1 starts normally with 20 moves.

**Version 5.0.0 was tried and reverted** (Yebot32 changed his mind; the build stays 4.9.0). Worth recording why it
broke: `PatchSceneController.ProfileDownloadCoroutine` builds the asset-bundle URL as
`<assetBundleDownloadRootUrl> + NexusPersonalConfig.appVersion + "/" + versionProfileType + ".profile"`. With
`appVersion` set to 5.0.0 the CDN has no `5.0.0/` directory, and the game shows **"Cannot download data"** with a
Retry button. `appVersion` is a serialized field in `NexusPersonalConfig` (asset `b1b166fcd32df421f8fb6536b5a24acb`),
separate from the manifest versionName — so a 5.0.0 label is possible in principle (patch `get_Version` instead) but
the string is not in the assembly's `#US` heap, so it would need metadata surgery. Not worth it; 4.9.0 stays.
The manifest keeps versionCode 501 so the APK installs as an upgrade over the earlier 56/500 builds.

- A build of the game without the G-Presto wrapper, if one exists — RetroBug could then convert it. Context
  (Yebot32, 2026-10-18): the game was free and has been abandoned for years, so there is no commercial interest left
  on the publisher's side.
- The local-response route above is the more promising one: it leaves the APK untouched and works on the version
  check, which is the failure actually observed.

## Files left
- `build/games/sbgs/` (local, git-ignored): the copied APK/OBB and diagnostic outputs.
- **Installed on the Tab S9 as of 18:53**: the untouched `com.BlueArk.SBGS` (v3.4.0) plus its OBB in
  `/sdcard/Android/obb/com.BlueArk.SBGS/`. Uninstall with `adb uninstall com.BlueArk.SBGS` when done.
- No `.rb` copy and no other SpongeBob package remains.
