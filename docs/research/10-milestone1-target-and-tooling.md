> **Research track:** milestone-1 target selection + tooling ground truth. Written 2026-09-18
> by a research sub-agent. Sections 1–3 are recommendations backed by fetched sources;
> sections 4–5 are **facts measured on this machine** (commands run 2026-09-18, paths quoted
> verbatim); section 6 is the verification-tooling inventory. Companion:
> `07-prior-art-public.md` (prior art, pointer-aliasing/fastmem answer, flag strategy),
> `02-android-runtime-abi-surface.md` (what the runtime layer must expose).

# 10 — Milestone 1: target APK corpus, build recipe, local capability, verification tooling

## 0. Milestone 1 restated, and the two things it must prove

Milestone 1 = **one** simple 32-bit APK launched end-to-end through the *real* pipeline on the
*normal* path (host ART in the RetroBug process + ARMv7 native code under RetroBug's engine), on
Android arm64. It must therefore prove, in one artifact:

1. **APK ingestion without `pm install`** — extract `classes.dex` + `lib/armeabi-v7a/*.so`, load
   the DEX into host ART, stand up `Application`/`Activity` — required because a 32-bit-only APK
   **cannot install** on a 64-bit-only device (`INSTALL_FAILED_NO_MATCHING_ABIS: Failed to
   extract native libraries, res=-113` — [Google's own 64-bit doc](https://developer.android.com/google/play/requirements/64-bit)),
   and legacy-target APKs are separately blocked on Android 14+
   ([Obtainium#1669](https://github.com/ImranR98/Obtainium/issues/1669)).
2. **One JNI crossing in both directions** — `System.loadLibrary` replaced by RetroBug's guest
   linker; the guest's `JNI_OnLoad` run under translation; `RegisterNative`-style bindings turned
   into host-callable trampolines (the exact seam the AOSP native-bridge contract names
   `getTrampoline(handle, name, shorty, len)` / `getTrampolineForFunctionPointer` —
   [`native_bridge.h`](https://android.googlesource.com/platform/art/+/refs/heads/main/libnativebridge/include/nativebridge/native_bridge.h)).

Deliberately **out** of milestone 1: GLES, audio, Vulkan, sensors, `NativeActivity`
surface/looper, self-modifying code. Those are the tier ladder in §3.

---

## 1. Where to get test APKs legally and reproducibly

### 1.1 Ranked by legal cleanliness

| Source | Legal / ToS position | Historical versions? | 32-bit-only available? | Verdict for RetroBug |
|---|---|---|---|---|
| **Android NDK samples** ([`android/ndk-samples`](https://github.com/android/ndk-samples)) | **Apache License 2.0** (verified: `LICENSE` at repo root is "Apache License Version 2.0, January 2004") | build any past commit | you choose `APP_ABI=armeabi-v7a` | **Primary corpus.** Commit the *build script + commit pins*, not the APKs; Apache-2.0 permits redistribution with `LICENSE`/`NOTICE` retention, so checked-in artifacts are also defensible |
| **F-Droid** ([f-droid.org](https://f-droid.org/)) | each app under its own FOSS license; F-Droid exists to redistribute them | **Yes — verified working**: `https://f-droid.org/repo/<pkgName>_<versionCode>.apk` returned HTTP 200 for `org.fdroid.fdroid_1000000.apk` and `..._1023052.apk`, 404 for a made-up code; the per-package API lists every historical `versionCode`: `https://f-droid.org/api/v1/packages/org.fdroid.fdroid` → `{"versionName":"1.23.2","versionCode":1023052}, …` | yes for pre-2020 apps | **Secondary corpus** for "real world" APKs (weird targetSdk, obfuscation, `multidex`, `assets/`). Fetch by script at test time; record SHA-256 + license per entry; do **not** commit GPL APKs into the repo |
| **AOSP app samples / CTS-style samples** (`platform/samples`, `development/samples`) | Apache-2.0 | via AOSP tag checkout | you build them | good for "expected framework behaviour" tests |
| **Uptodown / APKCombo / APKPure** | grey: aggregator sites, often re-host binaries whose *terms forbid* it; Play-derived APKs are Google/copyright-holder property | yes | yes | **Avoid.** Use only if a needed app has no FOSS source and the copyright holder's terms permit redistribution — decided per-app, never bulk, never committed |

**Concrete acquisition rule for the repo:** `tests/corpus/manifest.json` holding
`{url, sha256, license, sourceRepo, abi, targetSdk, capabilities[]}` + a downloader script;
CI materialises the APKs into a cache dir. Nothing binary in git except (optionally) the
Apache-2.0 NDK samples RetroBug builds itself.

### 1.2 Provenance checks worth automating per corpus entry

`aapt2 dump badging` (build-tools present locally, §5) for `targetSdk`/`uses-feature`/launchable
activity; `llvm-readelf -h/-d` on each `lib/armeabi-v7a/*.so` for `Machine: ARM`, `Flags: …
has Swan/NEON`, and the `DT_NEEDED` closure (that closure **is** RetroBug's required-exports list —
see §4.3); `dexdump -h` for DEX version (old APKs are DEX 035; host ART on Android ≥10 accepts
035–041 — verify on target); signature state (`apksigner verify` — legacy debug-signed APKs use
SHA-1-only signature schemes that Android 11+ refuses *on install*; RetroBug does its own
signature-free load, but record it so failures are attributable).

---

## 2. What a good milestone corpus looks like (capabilities, in the order they break things)

Ordered by *what RetroBug subsystem each one stresses first* — this is the ladder, not a
to-do list for milestone 1:

| Tier | Capability exercised | RetroBug subsystem under test |
|---|---|---|
| T0 | pure DEX, no native code | APK ingestion, `DexClassLoader`, Activity lifecycle, resources from a foreign APK |
| T1 | one JNI function, string round-trip | guest ELF loader, `JNI_OnLoad`, `RegisterNatives` interception, first `JNIEnv*` wrapper |
| T2 | native→Java callbacks (`CallStaticIntMethod`, `GetStaticFieldID`) | reverse trampolines, `GetMethodID`/`GetFieldID` caching, vararg `Call*Method` |
| T3 | `NativeActivity` + `ANativeWindow` pixel buffer + input | `libandroid.so` surface, `android_app` struct layout (32-bit!), input event pump |
| T4 | file IO + `assets/` + `AAssetManager` + `SharedPreferences` | guest↔host pointer paths, `struct stat` 32-bit rewrite, low-arena bounce buffers |
| T5 | threads, atomics, TLS, `pthread_create` from guest | translated-thread lifecycle, `TLV`/`__libc_get_tls`, signal masks, `LDREX/STREX` → `LDAXR/STLXR` |
| T6 | audio (OpenSL ES) or GLES2 | callback-from-host-thread→guest, `libOpenSLES`/`libEGL` stub ABI (doc 03) |
| T7 | C++ exceptions through a JNI frame / `setjmp`+`longjmp` | unwinder across the translated/host boundary (hardest single item) |
| T8 | guest emits code at runtime and executes it | SMC invalidation + `PR_SET_MDWE`-clean code arena (doc 09) |
| T9 | Vulkan, extension-heavy GL | deferred (doc 03); arm32 Vulkan is *available* in the NDK stubs (§4.3) but rare in real apps |

---

## 3. Recommended corpus (12 APKs), with the first target chosen

All NDK-sample entries are Apache-2.0 and are built with `APP_ABI=armeabi-v7a` **only** (drop
`arm64-v8a` from the ABI list) — verified sample directory names from the repo root:
`hello-jni hello-jniCallback bitmap-plasma native-activity native-audio audio-echo gles3jni
hello-gl2 hello-vulkan hello-oboe native-codec native-media native-midi sensor-graph
san-angeles teapots exceptions orderfile vectorization nn-samples prefab cmake camera
unit-test sanitizers endless-tunnel`
(<https://github.com/android/ndk-samples>).

| # | APK | Source / license | Capability (why this one) | Tier | Effort |
|---|---|---|---|---|---|
| 1 | **`hello-jni`** | ndk-samples, Apache-2.0 | `System.loadLibrary("hello-jni")` + one `jstring` return; **no GL, no audio, no NativeActivity**. The smallest possible "real" JNI crossing. | T1 | **Milestone 1** |
| 2 | `hello-jniCallback` | ndk-samples, Apache-2.0 | native calls *back* into Java (`CallStaticIntMethod`, cached `jclass/jmethodID`) → trampoline + reverse-trampoline + ref table in one go | T2 | M1b |
| 3 | hand-written "pure DEX" app (aapt2/d8 pipeline of §4.4) | our own, Apache-2.0 | proves ingestion + lifecycle with zero native code; the control group when something breaks | T0 | M1 pre-req |
| 4 | hand-written threads/atomics `.so` (pthreads, `__atomic_*`, TLS, `LDREX/STREX` loops, deep recursion) | our own | memory model + thread plumbing without app complexity; doubles as the unit-test corpus | T5 | M2 |
| 5 | `bitmap-plasma` | ndk-samples, Apache-2.0 | **first `NativeActivity`**: `ANativeWindow_lock`/pixels, `ALooper`, input; software rendering so no GL dependency | T3 | M2 |
| 6 | `native-activity` (a.k.a. `android-ndk-samples` `NativeActivity` sample) | ndk-samples, Apache-2.0 | full `android_app` struct + callbacks + asset manager; the 32-bit struct-layout test that matters | T3/T4 | M2 |
| 7 | `sensor-graph` | ndk-samples, Apache-2.0 | Java-side `SensorManager` + JNI `jfloatArray` critical sections → `GetPrimitiveArrayCritical` bounce-buffer path | T2/T4 | M3 |
| 8 | `native-audio` | ndk-samples, Apache-2.0 | `libOpenSLES.so` (stub present for 32-bit, §4.3), audio callback arriving **on a RetroBug-created thread** | T6 | M3 |
| 9 | `audio-echo` | ndk-samples, Apache-2.0 | full-duplex capture+playback, high-rate callbacks, two `.so`s, real-time pressure | T6 | M3 |
| 10 | `hello-gl2` | ndk-samples, Apache-2.0 | first GLES2: `libEGL.so`+`libGLESv2.so` stubs, `eglGetCurrentContext`, shader upload from guest memory | T6 | M4 |
| 11 | `gles3jni` | ndk-samples, Apache-2.0 | GLES3 via JNI + `eglGetProcAddress`/extension query → the "weak symbol / proc address" surface | T6 | M4 |
| 12 | `exceptions` | ndk-samples, Apache-2.0 | C++ exceptions thrown across a JNI frame → unwinder-across-boundary; the classic DBT wall | T7 | M5 |
| 13 | `san-angeles` | ndk-samples, Apache-2.0 | legacy fixed-point C, pure-GLES1-era rendering, tight loops → the first *performance* benchmark (JIT vs interpreter delta) | T6 | M5 |
| 14 | `hello-vulkan` **or** `teapots` | ndk-samples, Apache-2.0 | Vulkan device/queue/`vkMapMemory` on a 32-bit guest (`libvulkan.so` stub exists at API ≥24, §4.3); deferred until host path is stable | T9 | later |
| 15 | one real-world FOSS app with `armeabi-v7a`-only history, e.g. an old F-Droid entry chosen by `abi` scan | F-Droid (`/repo/<pkg>_<vc>.apk`, per-app FOSS license) | `multidex`, obfuscation, `ApplicationInfo` quirks, weird `targetSdk`, `extractNativeLibs=false`, and (for some engines) runtime code emission | T0–T8 mix | M4+ |
| 16 | synthetic SMC/JIT `.so` (mmap RW→write ARM code→`mprotect RX`→`__clear_cache`) | our own | T8 alone; **no third-party app is needed** and no shipped app should be relied on for this | T8 | M5 |

**Recommended first milestone APK: `hello-jni`, built `armeabi-v7a`-only.**
Reasons: it is (a) license-clean and locally reproducible (§4), (b) the smallest artifact that
still exercises DEX-on-host-ART + guest-ELF-under-translation + a two-way pointer crossing
(`jstring` UTF-16 in guest memory), (c) independent of graphics/audio/input so that a failure is
unambiguously the engine's, and (d) its `DT_NEEDED` closure is exactly `liblog.so, libc.so,
libm.so, libdl.so` — four libraries, which is a milestone-sized job.

---

## 4. Build-recipe facts (all measured on this machine)

Roots used below: `SDK=C:/Users/yebot32/AppData/Local/Android/Sdk`,
`NDK29=$SDK/ndk/29.0.14206865`, `NDK28=$SDK/ndk/28.2.13676358`,
`SYS=$NDK29/toolchains/llvm/prebuilt/windows-x86_64`.

### 4.1 Toolchain identity (verified)

* NDK **r29.0.14206865** (`Pkg.Revision = 29.0.14206865`, `Pkg.Desc = Android NDK`) and **r28.2.13676358** both installed.
* `$SYS/bin/clang.exe -v` → `Android (13989888, based on r563880c) clang version 21.0.0`, host `x86_64-w64-windows-gnu`.
* Per-API driver wrappers exist: `armv7a-linux-androideabi{21..35}-clang`, `aarch64-linux-android{21..35}-clang` (i.e. **32-bit ARM is fully supported by both NDKs**, and the wrapper range stops at API 35).

### 4.2 What `armeabi-v7a` means, exactly (verified from NDK metadata, not docs)

`$NDK29/meta/abis.json`:
```json
"armeabi-v7a": { "bitness": 32, "default": true, "deprecated": false,
                 "proc": "armv7-a", "arch": "arm",
                 "triple": "arm-linux-androideabi",
                 "llvm_triple": "armv7-none-linux-androideabi",
                 "min_os_version": 21 }
```
Same in `build/cmake/abis.cmake:6-12`. **`armeabi-v7a` is not deprecated in either installed
NDK**, and the minimum API is 21 → build corpus APKs with `minSdkVersion 21`.

Verified flag/ABI consequences (this is what RetroBug's decoder and ABI layer must accept):

| Property | Measured value | Where measured | RetroBug implication |
|---|---|---|---|
| default ISA mode | **Thumb-2 (T32)**: `-march=armv7-a` **plus `-mthumb`** unless `CMAKE_ANDROID_ARM_MODE` | `$NDK29/build/cmake/flags.cmake:102-109`; `.fnstart`/`.save` CFI emitted by default | the T32 (16/32-bit mixed, `.syntax unified`) decoder is the *primary* path, not the A32 one — decode order matters for milestone 1 |
| force ARM mode | `-DANDROID_ARM_MODE=arm` / `APP_ARM_MODE=arm` | `android.toolchain.cmake:130-145` | corpus must include ARM-mode builds too (`-marm`) so both states get coverage |
| float ABI | **`softfp`**: probe of `--target=armv7-none-linux-androideabi21` defines `__VFP_FP__ 1`, `__ARM_FP 0xc`, `__ARM_PCS 1`, `__THUMB_INTERWORK__ 1`, **no `__SOFTFP__` and no `__ARM_PCS_VFP`** (grep count 0) | `clang -E -dM` | **scalar float/double args travel in `s0-s15`/core regs per softfp**: RetroBug's JNI-argument marshaller must implement softfp, and hard-float (`__ARM_PCS_VFP`) binaries are out-of-contract → detect and reject via `EF_ARM_ABI_VER`/Tag_ABI_VFP_args |
| NEON | on by default: `__ARM_NEON 1`, `__ARM_NEON_FP 0x4`, `.fpu neon`; `exports.cmake:27 set(ANDROID_ARM_NEON TRUE)`; `build-binary.mk:251` warns users who override `-mfpu` | macro probe + `$NDK29/build/cmake/exports.cmake` | NEON (ASIMD) is mandatory in milestone 1's decode set, not optional |
| 16 KB pages | irrelevant to the 32-bit guest, and the NDK proves it: `-Wl,-z,max-page-size=4096` is applied **only** `if(ANDROID_ABI STREQUAL arm64-v8a OR … x86_64)` (`flags.cmake:35-42`, `build-binary.mk:135-140`) | NDK build scripts | guest is 4 KiB-granular by definition; the *host* (arm64) must be 16 KB-clean — see §4.4 and §5 (the only installed image is a `page_size_16kb` one) |
| STL | `APP_STL := c++_static` or `c++_shared`; `libc++_shared.so` for arm ships at `$SYS/sysroot/usr/lib/arm-linux-androideabi/libc++_shared.so` (+ static `libc++_static.a`, `libc++abi.a`) | directory listing | build half the corpus `c++_shared` (extra 32-bit `.so` to translate, ~1 MB of real ARM code = free decoder fuzz) and half `c++_static` |
| hardening defaults | `-D_FORTIFY_SOURCE=2`, `-fstack-protector-strong`, `-funwind-tables`, `-fno-omit-frame-pointer`-style sets, `-Wl,--no-undefined` | `flags.cmake` / `_ANDROID_NDK_INIT_CFLAGS` | `-funwind-tables` everywhere ⇒ ARM exception-index tables (`.ARM.extab/.ARM.exidx`) are in every corpus `.so`: RetroBug's linker must keep them reachable for the unwinder (tier T7) |

### 4.3 The NDK's **32-bit linker stub libraries** — a legally clean, complete ABI surface (verified)

Path (exists): `$NDK29/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/arm-linux-androideabi/<api>/`
with per-API subdirectories **21 … 35** and a top-level `libc++_shared.so`, `libc.a`, `libm.a`,
`libdl.a`, `libz.a`, `libstdc++.a`, `libcompiler_rt-extras.a`.

Stub `.so` set per API level (enumerated with `ls`, 2026-09-18):

* **API 21 (16):** `libEGL libGLESv1_CM libGLESv2 libGLESv3 libOpenMAXAL libOpenSLES libandroid libc libc++ libdl libjnigraphics liblog libm libmediandk libstdc++ libz`
* **API 24 (18):** + `libcamera2ndk`, `libvulkan`
* **API 26 (21):** + `libaaudio`, `libnativewindow`, `libsync`
* **API 29 (24):** + `libamidi`, `libbinder_ndk`, `libneuralnetworks`
* **API 35 (26):** + `libicu`, `libnativehelper` — **and NDK r28's `arm-linux-androideabi/35/` directory has the same 26 stubs** (`ls | wc -l` = 26 in both), identical in count to `aarch64-linux-android/35/` (26).

Verified properties of one stub (`llvm-readelf` on `.../arm-linux-androideabi/21/libEGL.so`):
`file format elf32-littlearm`, `Class: ELF32`, `Machine: ARM`, `Type: DYN`, `SONAME: libEGL.so`,
`FLAGS: BIND_NOW`, and a full `.dynsym`; `llvm-objdump -d --triple=thumbv7a-none-linux-androideabi`
prints `eglBindAPI: bx lr` etc. — i.e. they are **empty, exported-symbol-precise 32-bit ELFs**.

**Why this is the best possible answer to "which symbols must RetroBug provide":** it is
AOSP/Apache-2.0-derived machine data for the exact `armeabi-v7a` ABI at every API level, so
RetroBug's guest-`libc`/`libEGL`/`libOpenSLES` export tables can be **generated from the stubs**
(`llvm-readelf --dyn-syms` per level → diff → per-API export list) instead of transcribed from
documentation, and its symbol-completeness tests can assert "every `DT_NEEDED` symbol of every
corpus `.so` resolves in RetroBug's tables". Do the same for the 64-bit directory to get the
"API appeared at level N" mapping for free. Also verified for reference: a real 32-bit shipped
`.so`'s closure — `libc++_shared.so` (arm) `DT_NEEDED` = `libc.so, libm.so, libdl.so`,
`SONAME libc++_shared.so`, `BIND_NOW`.

### 4.4 Building an APK on this machine **without Gradle** (all pieces verified present)

`$SDK/build-tools/37.0.0/` contains: `aapt2.exe`, `aapt.exe`, `aidl.exe`, `d8.bat`,
`dexdump.exe`, `apksigner.bat`, `zipalign.exe`, `lld.exe`, `arm-linux-androideabi-ld.exe`,
`renderscript/`, `split-select.exe`, `retrace.bat`, `llvm-rs-cc.exe`; plus
`$SDK/cmdline-tools/latest/bin/`: `apkanalyzer.bat`, `avdmanager.bat`, `lint.bat`, `sdkmanager.bat`,
`d8.bat`, `r8.bat`, `retrace.bat`, `profgen.bat`, `resourceshrinker.bat`, `screenshot2.bat`.
`$NDK29/` has `ndk-build.cmd`, `ndk-gdb.cmd`, `ndk-lldb.cmd`, `ndk-stack.cmd`, `ndk-which.cmd`,
`$NDK29/simpleperf/`, `$NDK29/shader-tools/`, `$NDK29/meta/`, `$NDK29/sources/`, and SDK `cmake/`
has **3.30.3** and **3.31.6**.

Recipe (no Gradle, fully scriptable, ~4 steps):

1. native: `$NDK29/ndk-build.cmd APP_ABI=armeabi-v7a APP_PLATFORM=android-21 APP_STL=c++_static NDK_PROJECT_PATH=…`
   — or CMake: `$SDK/cmake/3.31.6/bin/cmake.exe -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK29/build/cmake/android.toolchain.cmake -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-21`
   (Ninja ships inside `$SDK/cmake/3.31.6/bin`; a Makefile generator also exists).
2. java: JDK 21 `javac --release 11 -classpath $SDK/platforms/android-36/android.jar` → `d8.bat --lib android.jar --output . classes/**.class` → `classes.dex`.
3. package: `aapt2.exe compile` → `aapt2 link -I $SDK/platforms/android-36/android.jar --manifest AndroidManifest.xml --min-sdk-version 21 --target-sdk-version 3x -o unsigned.apk`, add `classes.dex` + `lib/armeabi-v7a/*.so` (with `zipalign -p 4`), then `apksigner sign` with a debug keystore (`keytool` from JDK 21).
4. **Gradle is present in spirit**: `~/.gradle/wrapper/dists` already holds `gradle-8.11.1-bin`,
   `gradle-9.0.0-bin`, `gradle-9.1.0-bin`, and `~/.gradle/caches/modules-2` (505 MB) contains
   `com.android.tools.build` at **AGP 8.10.1 and 8.13.0** — so any project that ships its own
   `gradlew` wrapper can build, but `gradle` itself is **not on PATH** (verified with `which`).

---

## 5. Can we build AND run/test locally here? — measured findings

Environment: `PROCESSOR_ARCHITECTURE=AMD64`, `uname -m` → `x86_64` (Git Bash MINGW64_NT-10.0-26200),
**24 logical CPUs**, Windows 10.0.26200 → **this is an x86-64 PC, not an ARM Windows machine.**

| Question | Answer (evidence) |
|---|---|
| Can we **build** a 32-bit ARM Android APK? | **Yes.** NDK r28.2 + r29.0 with clang 21, `armv7a-linux-androideabi21-` wrappers, cmake 3.30/3.31, JDK 21, build-tools 35/36.1/37 (aapt2/d8/zipalign/apksigner), `ndk-build.cmd`. Disassembly/inspection tools present (`llvm-objdump.exe`, `llvm-readelf.exe`). |
| Is there an **emulator + AVD**? | **Yes.** `$SDK/emulator/emulator.exe` present; `emulator -accel-check` → "**AEHD (version 2.2) is installed and usable**"; `emulator -list-avds` → `Medium_Phone`; `~/.android/avd/Medium_Phone.avd/config.ini`: `abi.type=x86_64`, `hw.cpu.arch=x86_64`, `PlayStore.enabled=true`, 1080x2400/420dpi, 2048 MB RAM, 10 GB data. |
| Which image, and what can it run? | `system-images/android-37.1/google_apis_playstore_ps16k/x86_64` (`AndroidVersion.ApiLevel=37.1`, `ExtensionLevel=23`, `SystemImage.TagId=google_apis_playstore,page_size_16kb`). Its `build.prop`: `abilist=x86_64,arm64-v8a`, **`abilist32=` (empty)**, `ro.dalvik.vm.native.bridge=libndk_translation.so`, `ro.dalvik.vm.isa.arm64=x86_64`, `ro.enable.native.bridge.exec=1`; `package.xml`: `<abis>x86_64</abis> <translatedAbis>arm64-v8a</translatedAbis>`. → runs **x86_64 and arm64-v8a** APKs; **cannot run `armeabi-v7a`** at all. |
| Any device attached? | **No.** `adb devices -l` → empty list (adb 1.0.41 / 37.0.0-14910828 started its daemon fine). |
| Anything else usable to execute ARM32 code? | **No.** No `wsl.exe` distro ("The Windows Subsystem for Linux is not installed"), no `docker`, no `qemu-arm`/`qemu-system-arm`, no `python` (only the Microsoft Store stub → "Python was not found"), no `apktool`/`jadx`, no `gh`, no `gradle`. `git` is available. |
| Did we boot the emulator? | **No — the launch was blocked by tooling policy in this session**, so "bootable" is inferred from `-accel-check` + image/AVD integrity, not observed. |

**Bottom line (the key question):** *build — yes, fully; run — yes, but only x86_64/arm64-v8a
APKs, and **not** anything 32-bit ARM.* That has a sharp consequence for milestone 1: the local
emulator cannot serve as the milestone-1 host, because RetroBug's own shell must be an
**arm64** APK whose *translated* guest is 32-bit ARM; running RetroBug's AArch64 code generator
on top of `libndk_translation` (arm64→x86_64) would mean translating a translator — slow, and
ndk_translation is not a JIT-safe target. Therefore:

* **Milestone 1 must be validated on a physical 64-bit-only Android arm64 device** (any
  Snapdragon/Tensor/Exynos-or-Dimensity device with `abilist32` empty is the ideal host — it is
  the exact configuration RetroBug exists for; the Pixel 7-class and later, and per
  [64-bit doc](https://developer.android.com/google/play/requirements/64-bit) the 64-bit-only
  Pixel 4a(5G)/5/6 factory images, are the reference fleet).
* **The local PC is still the right place for the translator itself**, via a host-side build of
  the engine: (i) interpreter/decoder unit tests compiled for `x86_64` running ARM32 test blobs;
  (ii) the AArch64 emitter exercised by emitting to memory and *disassembling* it with
  `$SYS/bin/llvm-objdump.exe -d --triple=aarch64` (no execution needed to catch 90 % of emitter
  bugs); (iii) differential runs against `qemu-arm` (GPL, separate process = clean boundary) on
  a Linux CI worker.
* **Do not add a "run on the emulator" step to the plan.** Instead add a device-farm requirement
  (one arm64-only phone + `adb`) and a CI lane on an arm64 Linux runner (§6.4).
* Incidental but useful: the installed image is a **16 KB page** build, so as soon as RetroBug's
  shell is testable anywhere on it, the host-side page-size assumptions get exercised by default
  (`page_size_16kb` tag; NDK applies 16 KB-clean flags only to `arm64-v8a`/`x86_64`, §4.2).

---

## 6. Verification tooling for the Phase-3 review passes

### 6.1 Already installed here (paths verified)

| Tool | Path | Use |
|---|---|---|
| `llvm-objdump.exe` | `$SYS/bin/llvm-objdump.exe` | disassemble ARM32/Thumb/A64 (`-d --triple=armv7-none-linux-androideabi`, `--mach=` for raw byte blobs) — the decoder oracle for hand-written vectors |
| `llvm-readelf.exe` | `$SYS/bin/llvm-readelf.exe` | `-h -d --dyn-syms -A` (`-A` prints `.ARM.attributes`: `Tag_CPU_arch`, `Tag_ABI_VFP_args`, `Tag_THUMB_ISA_use`) → machine-checkable "is this guest binary in-contract?" (§4.2) |
| `dexdump.exe`, `apkanalyzer.bat`, `aapt2.exe`, `apksigner.bat`, `zipalign.exe`, `d8.bat`, `r8.bat`, `retrace.bat` | `$SDK/build-tools/37.0.0`, `$SDK/cmdline-tools/latest/bin` | APK/DEX inspection, signature + alignment checks, mapping retrace for guest stack symbols |
| `ndk-stack.cmd` | `$NDK29/ndk-stack.cmd` (+ `$NDK29/prebuilt/windows-x86_64/bin/ndk-stack.cmd`) | symbolise guest backtraces **if** RetroBug emits an ARM-shaped tombstone; feed it a fake `DEBUG` log |
| `simpleperf` | `$NDK29/simpleperf/bin/windows/x86_64/simpleperf.exe` (host) and `.../bin/android/{arm,arm64,x86,x86_64}/simpleperf` (device), plus `run_simpleperf_on_device.py`, `simpleperf_report_lib.py` | profile the translator on-device; the presence of an **arm 32-bit** `simpleperf` binary is a useful reminder that NDK tooling still assumes 32-bit hosts exist. ⚠ needs Python; this machine has only the Store stub (§5) → install Python or use `adb shell simpleperf` + `--json` and parse elsewhere |
| `cmake` + Ninja, `ndk-build.cmd` | `$SDK/cmake/3.31.6`, `$NDK29/build/` | build corpus + RetroBug |
| `shader-tools`, `spirv-*` | `$NDK29/shader-tools/windows-x86_64` | GLES/Vulkan corpus prep (doc 03) |
| `profgen.bat`, `screenshot2`, `lint.bat` | `$SDK/cmdline-tools/latest/bin` | perf-profile generation, headless screenshots for a "did a pixel appear" milestone-1 assertion |

### 6.2 Disassembly-as-oracle pipeline (no new dependencies)

For every decoded instruction: RetroBug's decoder → compare against `llvm-objdump` output for the
same bytes; for every *emitted* AArch64 block: `llvm-objdump -d` the bytes back and check the
invariants (no `MSR/MRS` outside the flag-sync protocol, no unmasked load outside the guest
arena, correct `adrp/add` pairs). Cheap, deterministic, runs entirely on this x86-64 box.

### 6.3 Test-vector corpora RetroBug can reuse (with licenses)

| Corpus | What it gives | License / how to use |
|---|---|---|
| **QEMU `tests/tcg/arm/`** (verified listing: `hello-arm.c`, `pcalign-a32.c`, `commpage.c`, `fcvt.c`, `float_convd.ref`, `float_convs.ref`, `float_madds.ref`, `semicall.h`, `system/`) | real ARM32 (a32) programs **with expected-output `.ref` files**, incl. PC-alignment and float-conversion edge cases | QEMU is GPL-2.0+ ⇒ run as a **separate process/oracle**, never link; the `.ref` expectation files are plain text data. `pcalign-a32.c` is directly about the ARM/Thumb bit in `PC`, a classic A32 bug source |
| **FEX-Emu `unittests/ASM/**` and `unittests/32Bit_ASM/**`** (verified in repo tree) | a mature *harness pattern*: hand-written `.asm` → assembled → run under emulator → diff flags/results; includes a `FEX_bugs/` directory of regression-per-opcode files | **MIT** (`SPDX-License-Identifier: MIT` verified in FEX sources) → reusable structure and ideas; content is x86 so only the harness transfers |
| **LLVM `llvm/test/MC/ARM/`** | thousands of ARM/Thumb assembly + expected encoding pairs | Apache-2.0 **with LLVM exception** ⇒ safe to reuse as test data |
| **Capstone / Unicorn test suites** | ARM encoding tables and edge-case byte blobs | Capstone is BSD-style (**re-verify** — the GitHub license field came back empty here, listed as `NOASSERTION`-adjacent); Unicorn is **GPL-2.0 (verified via API)** ⇒ oracle-only, separate process |
| **ARM MRA / ASL** | machine-readable architecture: the authoritative decode/semantics source from which RetroBug's *expectations* can be generated locally | ARM's MRA/ASL outputs are under Arm's own license with redistribution constraints ⇒ **generate vectors locally, ship only RetroBug-authored expectations**; ask the licensing agent before any derived artifact is committed |
| **NDK 32-bit linker stubs** (§4.3) | the ABI/API surface RetroBug must export, per API level | AOSP-derived, Apache-2.0, already on disk |
| gcc `gas/testsuite/gas/arm/` | assembler-level ARM encodings | GPLv3+ **and** Fedora/exceptions — treat like QEMU: local oracle use only, do not vendor |

### 6.4 Differential-testing harness (design, backed by the sources above)

1. **Two engines, one truth.** Run each guest program under (a) the interpreter (reference
   semantics, always correct), (b) the DBT. Compare a per-instruction trace
   (`regs, NZCV, IT-state, mem-writes`) and the program's exit output. The interpreter is the
   oracle for the DBT; both are checked against…
2. **…an external oracle.** `qemu-arm` (GPL, subprocess, `-strace`/plugin for traces) and — if a
   device is available — a real 32-bit Android/Linux ARMv7 userland. Any disagreement is a bug in
   *both* RetroBug engines, which is the class of bug the (a)/(b) comparison cannot see.
3. **Randomised encoding fuzz, constrained.** Generate only *legal, non-privileged* A32/T32
   encodings (from the MRA-derived decode tree, not random bytes), each instruction executed in a
   one-instruction block with a poisoned register set (`r13`/`r15`/`NZCV`/`FPSCR` canaries), and
   compare `NZCV`/`IT`/memory-delta against the oracle. This is precisely how RetroBug proves the
   §3 flag protocol of doc 07.
4. **ABI conformance suite.** For every symbol in the §4.3 stub tables: `dlsym` it through
   RetroBug's guest linker, call it with a synthetic argument vector, and check the host call was
   made with the expected marshalled values (this turns the runtime-surface doc into a test).
5. **Golden performance set.** `san-angeles` + `hello-gl2` + a synthetic `memcpy`/`fmadd`/`LDP`
   loop benchmark, run interpreter-only vs DBT-only vs fastmem-on/off (three independent switches,
   per doc 07 §7). Store the numbers per device model in the repo so regressions are attributable
   — the `optimization_flags`-style feature enum from dynarmic makes this reportable.
6. **CI shape.** GitHub now offers free `ubuntu-24.04-arm` runners
   ([announcement thread](https://news.ycombinator.com/item?id=42728015),
   [GitHub blog 2026-01-29: arm64 runners for private repos](https://github.blog/changelog/2026-01-29-arm64-standard-runners-are-now-available-in-private-repositories/)),
   and `qemu-user`/`binfmt` multi-arch in Docker is the standard way to *execute* armv7 test
   binaries on an x86-64 runner ([GitLab forum: qemu-user-static in runners](https://forum.gitlab.com/t/can-i-use-qemu-user-static-in-github-runner/85110),
   [Emulating ARM with QEMU on Debian/Ubuntu](https://gist.github.com/3107fdd62a87607d7cc7b1368d84fc52)).
   Whether a given arm64 runner's kernel still exposes AArch32-at-EL0 is **unverified** — plan the
   lane as "qemu-arm user-mode for the oracles, native arm64 for RetroBug's own unit tests".
7. **Crash attribution.** Every tombstone must carry `guest_pc`, `host_pc`, block id, and the
   translation-log key, replayable with `llvm-objdump` + `ndk-stack`-style symbolisation;
   dynarmic's per-block `DoNotFastmemMarker{LocationDescriptor, index}` (
   [fastmem.h](https://github.com/azahar-emu/dynarmic/blob/master/src/dynarmic/backend/arm64/fastmem.h))
   shows the block identity needs to be first-class in the metadata from day 1.

---

## Open questions / unverified

1. **No device or emulator session was executed here** (`adb devices` empty; the AVD launch was
   blocked by session policy). "AEHD usable + AVD exists + image config" is static evidence; a
   boot test is still needed to confirm the AVD actually starts and to capture
   `ro.product.cpu.abilist*` from a live shell.
2. **F-Droid per-app `abi` filtering** — historical APKs are confirmed fetchable (§1.1), but which
   *specific* archived apps are `armeabi-v7a`-only needs a scripted scan of `index.xml`/
   `archive enabled` repos; the `repo/archive/` path itself returned 404, so version retention
   appears to be inside `repo/` (verify with `fdroidserver` docs before relying on it).
3. **DEX version compatibility**: whether current host ART (API 36/37) still accepts DEX 035 from
   an old guest APK when loaded via `DexClassLoader` inside a 64-bit app — expected yes
   (`dalvik.system` supports old DEX), but it is the single most likely "milestone 1 blocker"
   and must be measured first.
4. **`extractNativeLibs=false` / `useLegacyPackaging`** guests: uncompressed-in-APK `.so` must be
   mapped straight from the APK file — needs a `mmap(fd, offset)` path into the guest arena;
   unverified whether Android's linker config affects RetroBug's own loader (it shouldn't).
5. **ndk_translation as a *host* for RetroBug**: tempting (run RetroBug's arm64 shell on the
   x86_64 emulator) but unquantified; assume unusable for JIT-heavy code until measured, and
   note the emulator offers **no** arm32 translation at all (§5).
6. **Whether `libndk_translation.so` could be extended to an arm32 guest** (i.e. is Google's
   translator structurally able to host a 32-bit guest?) — not investigated; would change the
   "system-integration" roadmap, and its AOSP location now 404s (`platform/external/ndk_translation`),
   so the source location needs re-finding under `frameworks/libs/native_bridge_support`.
7. **Python/simpleperf reporting** needs a real Python install on this machine (only the Store
   stub is present) or an on-device-only workflow.
8. **Capstone's exact license** (GitHub `spdx_id` came back empty; historically BSD-3-clause with
   contributions under other terms) — confirm before vendoring, and confirm ARM MRA/ASL terms
   with the licensing agent before generating committed vectors.
9. **`hello-vulkan` on a 32-bit guest**: the `libvulkan.so` 32-bit stub exists at API ≥24 (§4.3),
   but whether modern Android advertises Vulkan for a 32-bit process and whether any of the
   sample's `vkGetInstanceProcAddr` patterns survive translation — untested.

## Sources

* [Android NDK samples (Apache-2.0)](https://github.com/android/ndk-samples) · [LICENSE](https://raw.githubusercontent.com/android/ndk-samples/main/LICENSE) · [Support 64-bit architectures](https://developer.android.com/google/play/requirements/64-bit) · [Target API level requirements](https://developer.android.com/google/play/requirements/target-sdk) · [Obtainium#1669 (targetSdk<23 install restriction)](https://github.com/ImranR98/Obtainium/issues/1669) · [react-native#54150](https://github.com/react/react-native/issues/54150)
* [F-Droid](https://f-droid.org/) · [`org.fdroid.fdroid` package API](https://f-droid.org/api/v1/packages/org.fdroid.fdroid) · [F-Droid: Running on old Android versions](https://f-droid.org/docs/Running_on_old_Android_versions/)
* [QEMU `tests/tcg/arm`](https://github.com/qemu/qemu/tree/master/tests/tcg/arm) · [FEX-Emu (MIT)](https://github.com/FEX-Emu/FEX) · [unicorn (GPL-2.0)](https://github.com/unicorn-engine/unicorn) · [capstone](https://github.com/aquynh/capstone) · [dynarmic fork with `fastmem.h`/`emit_arm64_a32.cpp`](https://github.com/azahar-emu/dynarmic) · [LLVM MC tests](https://github.com/llvm/llvm-project)
* AOSP: [`native_bridge.h`](https://android.googlesource.com/platform/art/+/refs/heads/main/libnativebridge/include/nativebridge/native_bridge.h) · [`binary_translation` (Berberis)](https://android.googlesource.com/platform/frameworks/libs/binary_translation/+/refs/heads/main/README.md) · [`native_bridge_support`](https://android.googlesource.com/platform/frameworks/libs/native_bridge_support/)
* Local measurements (2026-09-18): `$SDK/ndk/29.0.14206865/{meta/abis.json, build/cmake/{abis,flags,exports,android.toolchain}.cmake, build/core/{build-binary,definitions}.mk, toolchains/llvm/prebuilt/windows-x86_64/{bin/*, sysroot/usr/lib/arm-linux-androideabi/**}}` · `$SDK/build-tools/37.0.0/*` · `$SDK/system-images/android-37.1/google_apis_playstore_ps16k/x86_64/{build.prop,source.properties,package.xml}` · `$SDK/platform-tools/adb.exe` · `$SDK/emulator/emulator.exe -accel-check` · `~/.android/avd/Medium_Phone.avd/config.ini` · `~/.gradle/wrapper/dists` · `~/.gradle/caches/modules-2/files-2.1/com.android.tools.build`
* CI/oracle references: [GitHub arm64 runners (HN)](https://news.ycombinator.com/item?id=42728015) · [GitHub blog: arm64 standard runners](https://github.blog/changelog/2026-01-29-arm64-standard-runners-are-now-available-in-private-repositories/) · [qemu-user-static in runners (GitLab forum)](https://forum.gitlab.com/t/can-i-use-qemu-user-static-in-github-runner/85110) · [Emulating ARM with QEMU on Debian/Ubuntu (gist)](https://gist.github.com/3107fdd62a87607d7cc7b1368d84fc52) · [appstraction#54 (emulator dropped armeabi-v7a after API 30)](https://github.com/tweaselORG/appstraction/issues/54)
