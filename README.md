<div align="center">

# RetroBug

<img src="file_0000000033448211bbdb620eed523634.png" width="500" alt="RetroBug Banner">

**A 32-bit Android compatibility layer for modern ARM64 devices.**

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![Status](https://img.shields.io/badge/Status-Developer_Preview-orange.svg)](#current-progress)
[![Arch](https://img.shields.io/badge/Arch-ARMv7_%E2%86%92_ARM64-green.svg)](#requirements)

[Requirements](#requirements) • [Getting started](#getting-started) • [Current progress](#current-progress) • [Community](#community)

</div>

---

## About

RetroBug is an open-source compatibility layer for older 32-bit Android games on ARM64 devices. On Android, it converts supported game APKs into separately installed copies and translates their ARMv7 code. The experimental iOS app imports APKs and uses an interpreter that can run without JIT.

## Requirements

| | Android | iPhone and iPad |
| --- | --- | --- |
| Device | ARM64 (`arm64-v8a`) with Android 8.0 or later | ARM64 with iOS or iPadOS 15 or later |
| Installation | Allow RetroBug to install converted games when Android prompts | Sign the unsigned IPA before installing it |
| Game files | A compatible 32-bit ARM APK (`armeabi-v7a`) you have permission to use, plus any required game data | A compatible 32-bit ARM APK you have permission to use |

Games are supplied separately. Split APKs, games that depend on retired servers, and games that need unavailable Android APIs may fail to install or run.

## Getting started

1. Check [GitHub Releases](https://github.com/Yebot32/Retro-bug/releases) for published packages or [GitHub Actions](https://github.com/Yebot32/Retro-bug/actions) for available build artifacts.
2. On Android, select a game from the library or tap **Add APK**, then follow the conversion and installation prompts. The preview supports per-game CPU modes, save import and export, and experimental render scaling for supported games.
3. On iOS, install a signed copy of the app, use **Add APK** to import a game from Files, and tap its library entry to run it.

The Android performance overlay starts off. To see FPS, translation speed, and CPU mode in supported games, go to **Settings → Gameplay → Performance overlay** and restart the game. This setting is included in the updated v0.1.0 Android preview build.

## Current progress

**v0.1.0 is a developer preview.** A Galaxy Tab S9 compatibility sweep covered 31 Android titles and verified gameplay in four, including Android KitKat Challenge, Fruit Ninja vs. Skittles, and Super Monkey Ball Bounce. Race Day rendered tracks but hit backend DNS failures. LEGO Mighty Micros started and received sensor input, but the scene remained black behind Robin. See the [Android compatibility log](https://github.com/Yebot32/Retro-bug/blob/session/m5-m6/docs/compatibility.md) for testing notes and known issues.

The iOS preview builds on macOS and now handles APK imports and library loading in the background. Device and gameplay verification for the v0.1.0 IPA is pending.

## Build from source

Use the [development branch](https://github.com/Yebot32/Retro-bug/tree/session/m5-m6) for the Android and iOS build scripts.

- **Android** requires Windows with Git Bash, JDK 17, Android SDK Platform 36, Build Tools 36.1.0, NDK 28.2.13676358, and CMake 3.31.6. The [Android workflow](https://github.com/Yebot32/Retro-bug/blob/session/m5-m6/.github/workflows/build-apk.yml) lists the guest-image preparation steps and runs `tools/build_launcher_apk.sh`.
- **iOS** requires macOS, Xcode 16, XcodeGen, and CMake. Run `tools/build_ios_ipa.sh` to produce an unsigned IPA.

## Community

- [Discord](https://discord.gg/r4mQdVgQsT) for discussion, help, and compatibility reports.
- [GitHub Issues](https://github.com/Yebot32/Retro-bug/issues) for reproducible bugs. Include the RetroBug build, device and OS, game and version, steps to reproduce, and any relevant logs with personal information removed.
- [YouTube](https://www.youtube.com/@Yebot32) for Yebot32’s videos.
- [PayPal](https://www.paypal.com/paypalme/Yebot32) for optional support.

## License

Distributed under the **Apache License 2.0**. See `LICENSE` for more details.
