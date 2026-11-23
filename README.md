<div align="center">

# RetroBug

<img src="file_0000000033448211bbdb620eed523634.png" width="500" alt="RetroBug Banner">

**A 32-bit Android compatibility layer for modern ARM64 devices.**

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![Status](https://img.shields.io/badge/Status-Developer_Preview-orange.svg)](#current-progress)
[![Arch](https://img.shields.io/badge/Arch-ARMv7_%E2%86%92_ARM64-green.svg)](#requirements)

[Requirements](#requirements) • [Getting started](#getting-started) • [Current progress](#current-progress) • [Community](#community)

[Download RetroBug v0.1.1](https://github.com/Yebot32/Retro-bug/releases/tag/v0.1.1)

</div>

---

## About

RetroBug is an open-source compatibility layer for older 32-bit Android games on ARM64 devices. On Android, it converts supported game APKs into separately installed copies and translates their ARMv7 code. On iOS, it imports APKs, provides part of the Android API, and runs ARM code through JIT, a matching ahead-of-time profile, or the interpreter.

## Requirements

| | Android | iPhone and iPad |
| --- | --- | --- |
| Device | ARM64 (`arm64-v8a`) with Android 8.0 or later | ARM64 with iOS or iPadOS 15 or later |
| Installation | Allow RetroBug to install converted games when Android prompts | Sign the unsigned IPA before installing it |
| Game files | A compatible 32-bit ARM APK (`armeabi-v7a`) you have permission to use, plus any required game data | A supported 32-bit ARM APK and any required OBB files with their original Android filenames |

Games are supplied separately. Split APKs, games that depend on retired servers, and games that need unavailable Android APIs may fail to install or run.

## Getting started

1. Download the [signed Android APK](https://github.com/Yebot32/Retro-bug/releases/download/v0.1.1/RetroBug-v0.1.1-Android.apk) or the [unsigned iOS IPA](https://github.com/Yebot32/Retro-bug/releases/download/v0.1.1/RetroBug-v0.1.1-iOS.ipa).
2. On Android, select a game from the library or tap **Add APK**, then follow the conversion and installation prompts. The preview supports per-game CPU modes, save import and export, and experimental render scaling for supported games.
3. On iOS, sign and sideload the IPA, then use **Add games → Choose APK or OBB** to import from Files. Tap a game to run it. Swipe right from the left edge to return to the library.

The performance overlay starts off on both platforms. On Android, enable **Settings → Gameplay → Performance overlay** and restart the game. Both apps check GitHub for newer official releases and show the release notes when an update is available for your platform.

## Current progress

**v0.1.1 is a developer preview.** Android adds compatibility fixes for APK conversion, expansion files, Unity graphics, and legacy Android APIs. Testing on the Galaxy Tab S9 reached a timed round in Oreo Twist and the tutorial in Rolling Sky. Verified results depend on the game APK version. See the [compatibility log](docs/compatibility.md) for details and outstanding issues.

**iOS is experimental.** Android KitKat reached gameplay in earlier device testing and classic Gumball reached its menu. Unity and Halfbrick startup fixes are included, with gameplay still unverified on the final build. The ARM interpreter works without JIT, and the bundled ahead-of-time profiles accelerate exact matches. Settings controls runtime JIT and render scale. Legacy FMOD sound output is incomplete.

## Build from source

- **Android** requires Windows with Git Bash, JDK 17, Android SDK Platform 36, Build Tools 36.1.0, NDK 28.2.13676358, and CMake 3.31.6. The [Android workflow](.github/workflows/build-apk.yml) lists the guest-image preparation steps and runs `tools/build_launcher_apk.sh`.
- **iOS** requires macOS, Xcode 16, XcodeGen, and CMake. Run `tools/build_ios_ipa.sh` to produce an unsigned IPA.

## Community

- [Yebot Studios on Discord](https://discord.gg/hPefjfkkVs) for emulation, game development, RetroBug support, and compatibility reports.
- [GitHub Issues](https://github.com/Yebot32/Retro-bug/issues) for reproducible bugs. Include the RetroBug build, device and OS, game and version, steps to reproduce, and any relevant logs with personal information removed.
- [YouTube](https://www.youtube.com/@Yebot32) for Yebot32’s videos.
- [PayPal](https://www.paypal.com/paypalme/Yebot32) for optional support.

## License

Distributed under the **Apache License 2.0**. See `LICENSE` for more details.
