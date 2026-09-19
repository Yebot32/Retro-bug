> **Research track:** public prior art. Written 2026-09-18 by a research sub-agent.
> Method: web search + page/source fetches (all cited inline), plus local probing of this
> machine's Android SDK/NDK where a prior-art claim was testable (the emulator system image's
> native-bridge configuration). Nothing here is implemented; this is evidence + transfer
> lessons. Cross-references: `01-cpu-emulation-armv7-on-arm64.md` (CPU strategy),
> `02-android-runtime-abi-surface.md` (the runtime/API surface), `08-prior-work-local-audit.md`
> (the developer's own repos on disk), `09-android-executable-memory-jit-permissions.md` (JIT
> permissions). Claims we could **not** verify are collected in
> [Open questions / unverified](#open-questions--unverified) at the end.

# 07 — Public prior art: 32-bit code on 64-bit hosts, and what it teaches RetroBug

## TL;DR — the eight things that actually change RetroBug's design

| # | Lesson | Source of the lesson |
|---|---|---|
| L1 | RetroBug's market is created by **three independent Google/Apple deadlines** (Play 64-bit native requirement 2019-08-01; 64-bit-only hardware/images from Pixel 7-era; Play target-API wall on 2026-08-31), and it keeps growing — but the *guest* APKs will also be blocked by Android 14's legacy-target install rule, so **RetroBug must ingest APKs itself, not rely on `pm install`**. | [Google blog 2019-01](https://android-developers.googleblog.com/2019/01/get-your-apps-ready-for-64-bit.html), [developer.android.com/64-bit](https://developer.android.com/google/play/requirements/64-bit), [target-API policy](https://developer.android.com/google/play/requirements/target-sdk) |
| L2 | Every shipping product that put a *32-bit* guest in a *64-bit-capable* host either (a) made the **whole process 32-bit** (Houdini/Android-x86, Windows WoW, `CONFIG_COMPAT`), or (b) **refused to mix** bitness in one process (Apple: "the system prevents you from mixing `arm64` code and `x86_64` code in the same process"). **Nobody** solved it the way RetroBug proposes (64-bit host runtime + 32-bit emulated native code in one process) — RetroBug is alone in that design space, and therefore owns the pointer problem (L3). | [Apple Rosetta doc](https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment.md), [FEX wiki](https://wiki.fex-emu.com/index.php/Development:32Bit_Syscall_Woes) |
| L3 | The pointer-aliasing answer: **a low 4 GiB identity-mapped guest arena is necessary but not sufficient.** Guest-owned memory must be forced below 4 GiB (FEX, box86, QEMU `guest_base`/`reserved_va` all do a variant of this); but host objects that the guest must touch (`jobject`s, `jstring`s, `jbyteArray`s, host `JNIEnv*`, `ANativeWindow*`) are **not** in that arena, so every pointer crossing needs either a 32-bit **handle table** or a **bounce buffer in the low arena**. Detail in [§4](#4-guest-memory-model-fastmem-and-the-pointer-aliasing-problem). | FEX wiki, box86 wrapping article, QEMU `include/user/guest-base.h` |
| L4 | Flags: for ARMv7→AArch64 **do not copy x86 translators' laziness**. Use the host's NZCV field directly (bit order is compatible — dynarmic moves guest `cpsr_nzcv` into host NZCV with a single `MSR`), keep *per-flag-granular* IR ops so unneeded flag writes are eliminated, and reserve a helper path for the few ops whose flags can't be expressed (`ADC`/`SBC` chains, flag-setting shifts/rotates, `UMULL`). Evidence: FEX's dedicated `RedundantFlagCalculationElimination` pass and dynarmic's `SetCpsrNZ`/`SetCpsrNZC`/`GetCFlag` opcode split. Detail in [§3](#3-condition-flag-strategy-what-existing-translators-actually-do). | [dynarmic `emit_arm64_a32.cpp`](https://github.com/azahar-emu/dynarmic/blob/master/src/dynarmic/backend/arm64/emit_arm64_a32.cpp), [FEX passes](https://github.com/FEX-Emu/FEX) |
| L5 | **Implement the AOSP native-bridge contract anyway**, even though an app cannot register one: `native_bridge.h` v1–v8 is a complete, battle-tested specification of exactly the seams RetroBug must handle (`getTrampoline(handle, name, shorty, len)`, `getTrampolineForFunctionPointer`, `isNativeBridgeFunctionPointer`, `getSignalHandler`, namespaces, `preZygoteFork`). Ship it as an internal interface; reuse it later for a rooted/AOSP build. | [AOSP `native_bridge.h`](https://android.googlesource.com/platform/art/+/refs/heads/main/libnativebridge/include/nativebridge/native_bridge.h) |
| L6 | **Berberis is the template, and it is Apache-2.0 AOSP code**: same shape (Android app's native code, DBT, native bridge integration, host+device unit tests, static "tiny" translator for AOT). Read it before writing a line of RetroBug. Its guest is riscv64→x86_64, i.e. **64-bit→64-bit**, so it does *not* contain the answer to L3. | [AOSP binary_translation README](https://android.googlesource.com/platform/frameworks/libs/binary_translation/+/refs/heads/main/README.md) |
| L7 | `fastmem` is where the speed is, and it is also where the bugs are: a published exploit write-up of Dolphin's fixed-base fastmem shows the aliasing failure mode in the wild ("mmap cannot return an error… we will simply pray"). Use the ART-style W^X-clean allocation from doc 09 + guard pages + a *verified* base, and never silently fall back. | [dougallj, "Exploiting Dolphin"](https://dougallj.wordpress.com/2016-11-13/exploiting-dolphin-part-1/) |
| L8 | **Nobody has shipped ARMv7→ARM64 Android app translation.** The closest live neighbour is `unidbg` (Apache-2.0, 5.2k stars, last push 2026-09-08): "Support ARM32 and ARM64", "Support dynarmic fast backend", emulates Android native libraries with a fake JNI/env — but it is an *analysis* tool on a JVM host, not a launcher. RetroBug's niche is real and empty. | [unidbg repo](https://github.com/zhkl0228/unidbg) |

---

## 1. Google's own "32-bit on 64-bit" work, and exactly what was removed

### 1.1 The historical mechanism (what RetroBug is replacing)

Android on arm64 devices historically ran 32-bit apps **natively, not translated**: an
Armv8/Aarch64 SoC whose cores implement AArch32-at-EL0, an arm64 kernel with `CONFIG_COMPAT`
(the kernel config that "adds support for running 32-bit (compat) binaries on a 64-bit
kernel" — [lkddb CONFIG_COMPAT](https://cateee.net/lkddb/web-lkddb/COMPAT.html),
[Mixing 64/32-bit userspace in arm64 linux](https://stackoverflow.com/questions/28334715/mixing-64-32-bit-userspace-in-arm64-linux-using-config-compat)),
and **two zygotes** selected by `ro.zygote=zygote64_32`: a 64-bit primary
(`app_process64`) and a 32-bit secondary (`app_process32`), with 32-bit copies of every
platform library in `/system/lib`. The architectural gate is
`ID_AA64PFR0_EL1.EL0` (bits [3:0]) — see doc 01 §1.1 for the ARM ARM citations.

Two independent pieces of evidence confirm that the *userspace*, not the kernel, is what
vendors removed first:

* A widely used Magisk module literally named **"Magisk Zygote64_32: Enabling 32-bit Support
  For Apps"** (XDA, Nov 2022) exists to flip the property back on —
  <https://xdaforums.com/t/magisk-magisk-zygote64_32-enabling-32-bit-support-for-apps.4521029/>.
  If the kernel lacked AArch32-at-EL0, a property flip could not work.
* Google's own 64-bit guidance says Pixel 7/7 Pro "were launched with an Android 13 system
  image that includes **32-bit system files and 32-bit system libraries to handle extreme
  compatibility cases**" while being "the first Android phones launched with support for
  64-bit-only apps" —
  [developer.android.com/google/play/requirements/64-bit](https://developer.android.com/google/play/requirements/64-bit)
  (which links the Oct-2022 announcement
  [android-developers.googleblog.com/2022/10/64-bit-only-devices.html](https://android-developers.googleblog.com/2022/10/64-bit-only-devices.html)).

**Consequence for RetroBug:** the property/kernel/`abilist32` situation is *per device and
per build*, and the removal order was: 32-bit zygote and `/system/lib` first, `CONFIG_COMPAT`
later (only when the SoC's weakest core dropped AArch32, e.g. Cortex-A510/X2→A715/X3→X4/A720
generation — [Arm support KA006014 "32/64 bit ARM Execution State support (Aarch32/AArch64)"](https://support.arm.com/documentation/ka006014/1-0/),
[HN: "Quite a lot of modern Arm 64 bit processors have dropped 32 bit"](https://news.ycombinator.com/item?id=32473577)).
So a **fast path** ("just run A32 natively, RetroBug only supplies the missing
`/system/lib` and a 32-bit zygote-equivalent") is theoretically possible on the *older*
half of the fleet — but it is precisely the fleet that still has 32-bit support. Treat it as
a probe, never as the foundation (same conclusion as doc 01's strategy A/G).

### 1.2 The dated policy timeline that creates the market

| Date | Requirement / change | Source |
|---|---|---|
| 2019-08-01 | Verbatim: "**Starting August 1, 2019: All new apps and app updates that include native code are required to provide 64-bit versions in addition to 32-bit versions when publishing to Google Play.**" | [Google Developers Blog, "Get your apps ready for the 64-bit requirement", 2019-01-15](https://android-developers.googleblog.com/2019/01/get-your-apps-ready-for-64-bit.html) |
| 2017-12 | Earlier framing: "Applications published on Google Play must support 64-bit architecture", motivated by security/performance | [developer.android.com 64-bit page](https://developer.android.com/google/play/requirements/64-bit) (links the 2017-12 post) |
| 2022-10 | "Pixel 7 and Pixel 7 Pro are the first Android phones launched with support for **64-bit-only apps**" | [android-developers.googleblog.com/2022/10/64-bit-only-devices.html](https://android-developers.googleblog.com/2022/10/64-bit-only-devices.html) (as cited by the 64-bit requirements page) |
| (pre-2023) | Google published **64-bit-only factory images** for Pixel 4a (5G), Pixel 5, Pixel 6, Pixel 6 Pro so developers could test "no 32-bit userspace" configurations | [64-bit requirements page, testing section](https://developer.android.com/google/play/requirements/64-bit) |
| 2024 | Android 14 era: legacy-target install restrictions reported in the wild (apps with very low `targetSdkVersion` refused by `pm install`) | [Obtainium issue #1669 "Support for installing apk with target SDK lower than 23 (restriction…)"](https://github.com/ImranR98/Obtainium/issues/1669) — *exact official wording not captured, see open questions* |
| **2026-08-31** | New apps/updates must target **Android 16 (API 36)** (API 35 for Wear/Auto, API 34 for TV/XR); extension available to **2026-11-01** | [Meet Google Play's target API level requirement](https://developer.android.com/google/play/requirements/target-sdk) |
| ongoing | "Apps that do not meet these target API levels … will only be available on devices running Android OS versions that are the same as or lower than the app's target API level" and "become unavailable to **new users**" | same page + [support.google.com answer 11926878](https://support.google.com/googleplay/android-developer/answer/11926878?hl=en) |
| NDK | x86 (32-bit) dropped as a target; `armeabi-v7a` still listed `"deprecated": false` | [android/ndk issue #1772 "Drop 32-bit x86 support"](https://github.com/android/ndk/issues/1772); local `ndk/29.0.14206865/meta/abis.json` |

The **two-axis** nature matters: the ABI axis (no `armeabi-v7a` loader on device) kills the
*native* half of a guest APK, while the target-API axis (Play availability + Android-14-style
install gates) kills *installation and framework behaviour* of old APKs even on devices that
could run them. RetroBug removes the first axis; it must *also* tolerate the second, because
its guest APKs will have `targetSdkVersion` values from 8 to 28.

### 1.3 Is the failure today an ABI-list failure, and how to prove it on a device

The observable failure is at **install/extract time**, not exec time. Google's own doc shows
the string:

```
adb: failed to install YOUR_APK_FILE.apk: Failure [INSTALL_FAILED_NO_MATCHING_ABIS:
Failed to extract native libraries, res=-113]
```

([64-bit requirements page](https://developer.android.com/google/play/requirements/64-bit)),
and real-world reports match: React Native users hitting a 32-bit-only APK on 64-bit-only
hardware get `couldn't find DSO to load: libreactnative.so` with the maintainers' answer
"You cannot do that" ([react/react-native#54150, 2025-10-14](https://github.com/react/react-native/issues/54150));
Google's own LiteRT tracks dropping `armeabi-v7a`
([google-ai-edge/LiteRT#3926](https://github.com/google-ai-edge/LiteRT/issues/3926)).

**Testable claims to run on each candidate host device (none was attached here — `adb devices`
returned an empty list, see doc 10 §5):**

1. `getprop ro.product.cpu.abilist32` → empty on 64-bit-only builds; non-empty (`armeabi-v7a`)
   where 32-bit userspace survives. This is *userspace* truth, not kernel truth.
2. `getprop ro.zygote` → `zygote64` vs `zygote64_32`.
3. `getprop ro.dalvik.vm.native.bridge` → present only on translated-ABI builds (§1.4).
4. **Kernel truth**: push a static `EM_ARM` ELF to `/data/local/tmp`, `chmod 755`, exec it.
   `Exec format error` ⇒ no `CONFIG_COMPAT`/AArch32-at-EL0; `Permission denied` (SELinux
   `app_data_file`/`shell_data_file` execute) ⇒ kernel *can* do it, policy says no; runs ⇒
   hardware path exists. (Do this via `adb shell`, not from an app: apps are barred from
   exec'ing files in their own storage — §5.2.)
5. `cat /proc/cpuinfo | grep Features` for `asimdhp`/absence of `cpuid`-driven AArch32 hints,
   and read `ID_AA64PFR0_EL1.EL0` indirectly (a kernel without compat never advertises
   `abilist32`).

### 1.4 What Google ships *today* for "foreign ABI" execution — measured on this machine

This machine's only Android system image is
`system-images/android-37.1/google_apis_playstore_ps16k/x86_64`. Its `build.prop` (read
locally, 2026-09-18) is a compact statement of the current state of the art:

```
ro.system.product.cpu.abilist=x86_64,arm64-v8a
ro.system.product.cpu.abilist32=
ro.system.product.cpu.abilist64=x86_64,arm64-v8a
ro.product.cpu.abi=x86_64
ro.dalvik.vm.native.bridge=libndk_translation.so
ro.dalvik.vm.isa.arm64=x86_64
ro.enable.native.bridge.exec=1
```

and its `package.xml` declares `<abis>x86_64</abis>` with
`<translatedAbis>arm64-v8a</translatedAbis>`.

Four facts to take away:

* Google's *shipping* translator (**`libndk_translation.so`**) handles **one** guest ISA here:
  `arm64-v8a` → `x86_64`. `abilist32` is empty: **no 32-bit guest of any ISA is offered.**
* Translated ABIs are surfaced to `PackageManagerService` by *appending to
  `ro.*.cpu.abilist`* (and via `translatedAbis` metadata) — that is the mechanism by which a
  bridge makes APKs installable at all. RetroBug cannot use it: `abilist` and
  `ro.dalvik.vm.native.bridge` are read-only system properties consumed by zygote before any
  app code runs.
* The image tag is `page_size_16kb` (`Pkg.Desc=System Image Page Size 16k x86_64 with Google
  Play.`) — the *host* side of RetroBug will be tested on 16 KiB pages while the *guest* is
  4 KiB (doc 10 §4.4).
* Historically the emulator **did** translate 32-bit ARM: "Android 11 x86_64 emulators
  supported `arm64-v8a` **and** `armeabi-v7a`; Android 12/13 removed `armeabi-v7a` from the
  ABI list" ([tweaselORG/appstraction#54](https://github.com/tweaselORG/appstraction/issues/54)).
  So Google built arm32-on-x86_64 translation, shipped it, and then deleted the 32-bit
  half — the exact hole RetroBug targets, now unfilled by anyone.

---

## 2. Native bridges and shipping binary translators

### 2.1 The Android native-bridge contract (read the header, it is the spec)

From `platform/art`'s
[`libnativebridge/include/nativebridge/native_bridge.h`](https://android.googlesource.com/platform/art/+/refs/heads/main/libnativebridge/include/nativebridge/native_bridge.h)
(fetched 2026-09-18; line numbers from the raw file):

| Interface fn | Purpose | RetroBug mapping |
|---|---|---|
| `initialize(runtime_cbs, private_dir)` | bridge loads, gets callbacks back | guest process bootstrap |
| `loadLibrary(libpath, flag)` → handle | bridge, not the host linker, loads the guest ELF | RetroBug's own guest linker (doc 02) |
| `getTrampoline(handle, name, shorty, len)` | **given a guest symbol and its JNI "shorty" signature, return a host-callable entry point** | the JNI bridge stub — the single most important idea in the file |
| `getTrampolineWithJNICallType` (≥v7) | same, with kStatic/kVirtual/kInterface call type | needed: ART's JNI calling conventions differ per kind |
| `getTrampolineForFunctionPointer(method, …)` | reverse direction: host has a raw function pointer from the guest | `RegisterNatives`, `pthread_create`, `qsort` comparators, `AEvent` callbacks |
| `isNativeBridgeFunctionPointer(method)` (v8) | "is this pointer actually a translated guest entry?" | ART needs it for stack walking / `JNIEnv` decisions |
| `isSupported(libpath)` / `isPathSupported(path)` | per-file yes/no | RetroBug per-`.so` gate |
| `getAppEnv(instruction_set)` → `NativeBridgeRuntimeValues` | bridge tells the runtime which ABI/ISA it presents | why `abilist` grows `arm64-v8a` on the emulator |
| `getSignalHandler(signal)` | bridge sees faults first | **mandatory** for RetroBug: SEGV from translated code must be attributed (fastmem guard page vs. guest crash vs. SMC) |
| `createNamespace` / `linkNamespaces` / `loadLibraryExt` / `getVendorNamespace` / `getExportedNamespace` | linker-namespace parity | guest `dlopen` isolation, `ClassloaderNamespace` parity |
| `preZygoteFork()` | bridge must quiesce before fork | translated-code cache + thread state across zygote fork |
| `unloadLibrary` / `getError` | handle lifecycle and `dlerror()` text | old apps print/parse these |
| `NativeBridgeRuntimeCallbacks.getMethodShorty / getNativeMethodCount / getNativeMethods` | the runtime *tells* the bridge which natives a class needs | RetroBug's lazy linking loop |

The file also notes `getTrampoline` (v1) is **deprecated** in favour of
`getTrampolineWithJNICallType` — i.e. even Google needed two revisions of the JNI-crossing
contract before it was right. Budget for the same churn; keep the seam narrow.

**Why an app cannot use this:** the bridge is `dlopen`ed by zygote from
`/system/lib(64)` or `/vendor/lib(64)` based on a read-only property, and its `abilist`
injection happens at build time (§1.4). RetroBug's *normal path* therefore re-implements the
same seams **inside** its own process; the contract stays useful as an internal interface and
as the design of a future rooted/AOSP variant.

### 2.2 Intel Houdini — ARM-on-x86 Android (the canonical "host ART + emulated native libs")

* Public technical deep-dive: **"Sleight of ARM: Demystifying Intel Houdini"**, Brian Hong,
  DEF CON 29 — [PDF](https://media.defcon.org/DEF%20CON%2029/DEF%20CON%2029%20presentations/Brian%20Hong%20-%20Sleight%20of%20ARM%20-%20%20Demystifying%20Intel%20Houdini.pdf),
  [video](https://www.youtube.com/watch?v=kdd8dSifxvU). *The PDF fetch failed from this
  machine; treat its specifics as unverified and re-read offline.*
* Integration walkthroughs (how a ROM adds it): [Android-x86 集成 houdini 所做的修改](https://blog.csdn.net/Roland_Sun/article/details/50132175),
  [Adding ARM native bridge to the AOSP11 x86 emulator](https://chgans.design.blog/2021-05-23/adding-arm-native-bridge-to-the-aosp11-x86-emulator/),
  [libhoudini install notes](https://stackoverflow.com/questions/49634762/how-to-install-libhoudini-on-a-custom-android-x86-rig).
* Life and death: Intel's own support threads
  ([2014 "ARM Binary Translation On The Intel x86 Platform — Still Supported?"](https://community.intel.com/t5/Software-Archive/ARM-Binary-Translation-On-The-Intel-x86-Platform-Still-Supported/td-p/1024981),
  [2015 "ARM translator issue in Android-X86"](https://community.intel.com/t5/Software-Archive/ARM-translator-issue-in-Android-X86-platform/m-p/1034394))
  and the maintenance reality: people still hand-port it
  ([tony-cloud/houdini9](https://github.com/tony-cloud/houdini9)); it broke on newer Androids
  ([redroid-doc#261 "Native Bridge (Houdini) on Android 12"](https://github.com/remote-android/redroid-doc/issues/261))
  and today only the 64-bit path survives in Google's images (§1.4). Google dropped it because
  **x86 Android died as a product category** (Atom abandoned; the surviving uses are emulator
  images and containers — `redroid`, Android-x86, FydeOS), not because the translator was
  technically wrong.
* **The architecturally decisive point (inference, flagged):** for a *32-bit* ARM guest,
  Houdini's host process was a 32-bit x86 process (`app_process32`, `/system/lib`), so the
  guest and the host's native runtime shared one 32-bit address space and pointer truncation
  never arose. That is why Houdini needed no `fastmem` story and why RetroBug, which keeps a
  64-bit ART process, cannot simply copy Houdini's shape.
* Transferable anyway: the two things every Houdini-descendant product converges on are
  (i) **only native code is translated** — Java/DEX runs on the host runtime, and
  (ii) **every JNI crossing is a generated trampoline with a signature in hand** (`shorty`).
  RetroBug's "normal path" is exactly this shape (doc 02), so the trampoline layer, not the
  translator, is the compatibility surface.

### 2.3 Apple Rosetta 2 — AOT + JIT, whole-process translation, and the explicit refusal to mix

Verified statements from
[About the Rosetta translation environment (markdown endpoint)](https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment.md):

* Scope: "a translation process that allows users to run Mac apps that contain **`x86_64`
  instructions** on Apple silicon" — 32-bit Intel code is out of scope entirely (also
  [InfoQ](https://www.infoq.com/news/2020/11/rosetta-2-translation/): "No support for 32-bit
  code or virtualization").
* **"The system prevents you from mixing `arm64` code and `x86_64` code in the same process.
  Rosetta translation applies to an entire process, including all code modules that the
  process loads dynamically."** — the opposite trade-off to RetroBug's; note that Apple could
  afford it because both sides are 64-bit and same-sized.
* Capability boundary: translates "most Intel-based apps, **including apps that contain
  just-in-time (JIT) compilers**"; does *not* translate kernel extensions or x86 VM apps;
  "translates all `x86_64` instructions, including ones from the AVX and AVX2 instruction set,
  but it doesn't support the execution of AVX512 vector instructions" → precedent for
  RetroBug declaring a *small explicit* unsupported-instruction list (see §3.4).
* Detection API: `sysctlbyname("sysctl.proc_translated", …)` → RetroBug needs the same
  capability bit for its own diagnostics and for per-app profiles.
* Lifecycle policy (useful for RetroBug's roadmap framing): "Rosetta … will be available
  through **macOS 27** … Beyond this timeframe, we will keep a subset of Rosetta functionality
  aimed at supporting **older unmaintained gaming titles**"; and "macOS 27 directly integrates
  support for Intel binary translation, without needing to install Rosetta", enabling "Intel
  Linux binaries running in ARM virtual machines". Translation layers are now *in-product*
  infrastructure, not stopgaps — good news for RetroBug's premise.
* Mechanism, per [InfoQ's write-up of Intel's analysis](https://www.infoq.com/news/2020/11/rosetta-2-translation/):
  **AOT translation at install/first-launch**, with **JIT translation for binaries that
  themselves emit code**, driven by page faults; and Apple added **hardware TSO support in M1
  specifically so x86 memory ordering could be honoured cheaply**. Two lessons: (a) build the
  AOT/cache path from day 1 (see §2.7 for RetroBug's cache design), (b) *memory model* work is
  a first-order cost of cross-ISA DBT — for ARMv7→AArch64 RetroBug is lucky (both are
  weakly-ordered, and AArch64's default is weaker than ARMv7's, so RetroBug needs
  *no* fence-insertion for guest loads/stores, only attention to `DMB`/`DSB` mapping and to
  exclusive-loads/stores `LDREX/STREX` → `LDAXR/STLXR`) — but verify with the ATC'24 paper
  [Harmonizing Memory Consistency in Cross-ISA Binary Translation](https://www.usenix.org/system/files/atc24-gao-chen.pdf).
  Rosetta cache-path specifics (`/var/db/oah/…`, `oahd`, `aotctl`, `xtld`) were **not**
  verified from a fetchable page (see open questions); only the *pattern* — a system daemon
  owning a persistent translated-code store, populated at install time, keyed by the on-disk
  binary — is what RetroBug should mirror per-app under its own `app data` dir.

### 2.4 Microsoft Arm64EC / Arm64X — "thunk what you must, emulate what you can't"

Primary docs: [Arm64EC for Windows 11 apps on Arm](https://learn.microsoft.com/en-us/windows/arm/arm64ec),
[Arm64X PE files](https://learn.microsoft.com/en-us/windows/arm/arm64x-pe),
NVIDIA's comparison [The Differences Between ARM64EC, ARM64, and ARM64X](https://docs.nvidia.com/rtx-spark/rtx-spark-porting-guide/0.1.0/portingandemulation/arm64ecvsarm64.html),
Arm's own talk [Arm64EC — ABI for Mixing x64 and Arm64](https://developer.arm.com/additional-resources/video-tutorials/devhub/arm64ec-abi-for-mixing-x64-and-arm64),
and a third-party deep-dive [ARM64 Boot Camp: ARM64EC and ARM64X Explained](http://www.emulators.com/docs/abc_arm64ec_explained.htm) (fetch failed here).

The transferable idea (documented at title level: an *ABI* — not an emulator — that lets
**native ARM64 code and emulated x64 code live in one binary and one process**, with a PE
"hybrid" format so the loader knows which parts are which) is directly applicable to
RetroBug's wish-list item **"emulate only what you must"**: where RetroBug can express an
ARMv7 operation in a *different, host-native* form (e.g. compile a hot ARMv7 routine as a
hand-written AArch64 replacement, or run the guest's `libc`/`libm` calls as host code through
wrappers rather than translating them), it should, and the *decision boundary* must be
recorded in the binary/metadata rather than discovered at runtime. RetroBug's analogue of an
Arm64X "hybrid image" is a **per-`.so` symbol table of wrapped-vs-translated dispositions**,
which is exactly what box86/box64's wrapped-library system is (§2.6). Detailed mechanics of
Arm64EC (register-usage differences, `__ospcall`/EC-prefixed symbols, load-config flags)
are **not** verified here — see open questions.

### 2.5 Google Berberis — the closest architecture, in the same license family

From the AOSP README at
[`platform/frameworks/libs/binary_translation`](https://android.googlesource.com/platform/frameworks/libs/binary_translation/+/refs/heads/main/README.md)
(fetched verbatim):

* "Dynamic binary translator to run Android apps with **riscv64 native code on x86_64 devices
  or emulators**." Same product shape as RetroBug: an Android *app's* native code, translated
  at runtime, no VM.
* Extensions: "Supported extensions include Zb\* (bit manipulation) and **most of Zv
  (vector)**. Some less commonly used vector instructions are not yet implemented, but
  **Android CTS and some Android apps run** with the current set of implemented instructions."
  → the honest release posture RetroBug should copy: publish the implemented subset, gate on
  CTS-like self-tests, and *say* which instructions are missing.
* Workflow facts RetroBug should steal: `lunch sdk_phone64_x86_64_riscv64-trunk_staging-eng`,
  `m berberis_all`, `mmm frameworks/libs/binary_translation`; a
  `berberis_program_runner_riscv64` host binary to run *one* translated program
  (`berberis_hello_world_static.native_bridge/x86_64/…`) with no device attached;
  `berberis_run_host_tests` / `berberis_host_tests` gtest binary; on-device
  `nativetest64`. **A host-runnable translator + host unit tests is the difference between
  iterating in minutes and iterating in emulator boots** — RetroBug has no reason to be
  Android-only in its dev loop (doc 10 §5–6 shows why this machine in particular needs it).
* Directory contents observed at repo root: `android_api/`, `assembler/`, `backend/`,
  `base/`, `berberis_config.mk`, `native_bridge/`-style targets — i.e. the same layering
  doc 01 recommends (frontend / IR+backend / guest-API layer / bridge glue).
* Public mailing list `berberis-discuss@googlegroups.com`; Google talk
  "Berberis: Dynamic Binary Translation from RISC-V to X86_64 on Android"
  ([RISC-V Summit 2024](https://riscvsummit2024.sched.com/event/1iYuJ/berberis-dynamic-binary-translation-from-risc-v-to-x8664-on-android-lev-rumyantsev-jeremiah-griffin-google),
  [video](https://www.youtube.com/watch?v=HjhzXZqjFrU)); the broader AOSP home for bridge
  support is [`frameworks/libs/native_bridge_support`](https://android.googlesource.com/platform/frameworks/libs/native_bridge_support/+/refs/heads/main/?format=JSON)
  (HTTP 200 confirmed; `platform/external/ndk_translation` now 404 — it was folded/relocated).
* **Caveat that makes RetroBug different:** both Berberis and `ndk_translation` translate
  64-bit guests, so guest pointers and host pointers are the same width and they can simply
  *share* the address space. The moment RetroBug's guest is 32-bit, that shortcut dies (§4).

### 2.6 box86 / box64 / FEX-Emu — 32-bit guests inside 64-bit host processes (the real analogues)

* **box86** (MIT per [repo metadata](https://api.github.com/repos/ptitSeb/box86)) runs
  x86-32 ELF binaries inside an ARM (often 64-bit) process — structurally RetroBug's problem,
  with Linux instead of Android above it. Its key public design doc is
  [A deep dive into library wrapping](https://box86.org/2021/08/a-deep-dive-into-library-wrapping/):
  per-library "wrapped" (host implementation) vs "emulated" (interpreted/translated guest
  implementation) dispositions, and wrappers that "**unpack the arguments and give them all
  to the native function**" — i.e. explicit argument-by-argument marshalling on every crossing
  plus a **bridge** table for guest⇄host function pointers (see also
  [Deciding "Wrapped" vs. "Emulated"](https://hackmd.io/@devaraja/rkXpJfGxgl) and
  [box86/docs/USAGE.md](https://github.com/ptitSeb/box86/blob/master/docs/USAGE.md) for the
  env-var-driven per-app compatibility knobs — the precedent for RetroBug's per-app profiles).
* **FEX-Emu** (MIT; verified in-file `SPDX-License-Identifier: MIT` on
  [`OpcodeDispatcher/Flags.cpp`](https://github.com/FEX-Emu/FEX/blob/main/FEXCore/Source/Interface/Core/OpcodeDispatcher/Flags.cpp)
  and
  [`RedundantFlagCalculationElimination.cpp`](https://github.com/FEX-Emu/FEX/blob/main/FEXCore/Source/Interface/IR/Passes/RedundantFlagCalculationElimination.cpp))
  answers the address-space question explicitly on its wiki,
  [Development:32Bit Syscall Woes](https://wiki.fex-emu.com/index.php/Development:32Bit_Syscall_Woes):
  "FEX runs as 64-bit, requiring struct rewriting"; "**32bit mmap must return pointers in the
  lower 32bit memory space**"; FEX "maps `[0x1'0000'0000, 0x1'0000'0000'0000)` range on
  application startup" so the kernel cannot hand the guest a >4 GiB address. **Two design
  options are therefore proven in production: reserve the low 4 GiB, or reserve everything
  above it.** RetroBug should do both: claim [0, 4 GiB) for the guest arena *and*
  pre-reserve above so no allocation escapes (see §4.3).
* **box64** ([repo](https://github.com/ptitSeb/box64)) is the 64-bit sibling; useful to RetroBug
  mainly for its *dynarec* switches (e.g. `DYNAREC_LIGHTLY`-style tiers and
  per-app env overrides) rather than memory model.

### 2.7 Commercial/other translators found (thin or negative results)

* **Transitive** (transitive-studio.com) and **CodeWeavers CrossOver**: positioned as
  "run Windows/x86 software on ARM/Mac" products; **no public technical documentation of
  their translation or memory model surfaced in these searches** — treat as non-informative
  prior art, not a shortcut.
* **ExaGear** (Eltech/NTS; ARM32-and-MIPS→x86 Linux userland DBT, incl. an Android variant
  used by some x86 tablets and by DEEPin/SparkyStick) is the classic *shipped* 32-bit-guest
  DBT and was reportedly discontinued ~2023; **could not verify from a fetchable source in
  this pass** — flagged in open questions, worth one follow-up because its "process-local
  32-bit arena + wrapped libc" design is the same family as box86.
* **`unidbg`** ([Apache-2.0](https://github.com/zhkl0228/unidbg), 5216 stars, last push
  2026-09-08): "Allows you to emulate an Android native library, and an experimental iOS
  emulation"; README bullets verified: "Support **ARM32 and ARM64**", "unicorn backend …
  gdb stub, instruction trace, memory read/write trace", "Support **dynarmic** fast backend",
  "Support **Apple M1 hypervisor**, the fastest ARM64 backend", "Support **Linux KVM backend**",
  "Memory leak detection for emulated native code with guest backtrace and **host stack
  trace**". This is the closest *live*, permissively-licensed code that loads a guest `.so`,
  fakes `JNIEnv`, and runs ARM32 — it is RetroBug's nearest neighbour and a good source of
  "which Android symbols do apps actually demand" ground truth (its stub libs), though its
  memory model is Unicorn-style (guest memory is *not* host memory) which is the thing
  RetroBug must *not* copy (§4).

---

## 3. Condition-flag strategy: what existing translators actually do

### 3.1 Evidence

1. **dynarmic's A32 frontend + AArch64 backend** (0BSD; fork inspected:
   [`emit_arm64_a32.cpp`](https://github.com/azahar-emu/dynarmic/blob/master/src/dynarmic/backend/arm64/emit_arm64_a32.cpp))
   stores guest flags as one 32-bit field in the JIT state and syncs it to the **host** flags
   register in one instruction at block boundaries:
   ```
   27:    // TODO: Flags in host flags
   28:    code.LDR(Wscratch0, Xstate, offsetof(A32JitState, cpsr_nzcv));
   29:    code.MSR(oaknut::SystemReg::NZCV, Xscratch0);
   ```
   and reads it back via `MRS` inside `A32GetCpsr` emission
   (`static_assert(offsetof(A32JitState, cpsr_nzcv) + sizeof(u32) == offsetof(A32JitState, cpsr_q));`).
   Two facts fall out: **(a)** guest `CPSR.NZCV` (bits 31..28) and host `NZCV` (bits 31..28)
   are moved with *no* bit shuffling ⇒ **the flag layouts are bit-compatible**; **(b)** even a
   mature, heavily-used ARM32→ARM64 recompiler still treats "keep flags in host flags" as a
   **TODO** — i.e. today it pays `LDR/MSR` in, `MRS/STR` out per block and computes flags in
   GPRs inside the block. That is RetroBug's opportunity, not its problem.
2. **dynarmic's IR granularity** ([`ir/opcodes.inc`](https://github.com/azahar-emu/dynarmic/blob/master/src/dynarmic/ir/opcodes.inc))
   is per-flag-subset, not "the whole CPSR":
   `SetCpsrNZCV`, `SetCpsrNZCVRaw`, `SetCpsrNZCVQ`, **`SetCpsrNZ`**, **`SetCpsrNZC`**,
   **`GetCFlag`**, `GetCpsr`/`SetCpsr`, `OrQFlag`, `Get/SetGEFlags`,
   `SetGEFlagsCompressed`. This exists so that a dead-code pass can drop the flags an
   instruction never needed: `ADDS` followed by an unrelated `MOV` only needs N and Z.
3. **FEX-Emu (x86→AArch64)** keeps x86's 6 condition flags as *separate raw fields in CPU
   state* (`RFLAG_CF_RAW_LOC`, `RFLAG_PF_RAW_LOC`, …, with `PF` and `CF` deliberately stored
   **inverted** "so hoist the invert"), computes them **deferred**, and materialises them only
   when an instruction actually reads EFLAGS — `CalculateDeferredFlags()` — plus a whole IR
   pass that *removes* needless flag work:
   [`RedundantFlagCalculationElimination.cpp`](https://github.com/FEX-Emu/FEX/blob/main/FEXCore/Source/Interface/IR/Passes/RedundantFlagCalculationElimination.cpp)
   models per-instruction `Read`/`Write` flag sets (`FLAG_NZCV`, `FLAG_ALL`) and a
   `CanEliminate` bit meaning "the instruction can be eliminated if its flag writes can all be
   eliminated".
4. **QEMU's ARM target** is the counter-example to "ARM needs lazy flags": its AArch32 CPU
   state caches the flags **eagerly as separate fields** —
   [`target/arm/cpu.h`](https://raw.githubusercontent.com/qemu/qemu/master/target/arm/cpu.h)
   lines 286-291:
   ```
   /* cpsr flag cache for faster execution */
   uint32_t CF;  /* 0 or 1 */
   uint32_t VF;  /* V is the bit 31. All other bits are undefined */
   uint32_t NF;  /* N is bit 31. All other bits are undefined. */
   uint32_t ZF;  /* Z set if zero. */
   ```
   (`GE`, `QF`, `condexec_bits` — the IT-state — sit next to them; `uncached_cpsr` is about
   CPSR *mode bits*, not flags; the old `UncachedCondcode` lazy scheme is gone from the
   current tree — greps of `target/arm/tcg/cpu.h`/`translate.c` find no such symbol.)

### 3.2 What RetroBug should do (recommendation)

**Primary: host-NZCV-resident flags, with a materialise/discard protocol — not x86-style
laziness.**

1. Invariant: while executing a translated block, guest NZCV lives in the host `NZCV` field.
   ARMv7 `ADDS/SUBS/CMP/CMN/NEG/LSLS…` with `S=1` map 1:1 onto AArch64 `ADDS/SUBS/CMP/CMN/NEG/
   LSLV…` **for 32-bit operands**, including carry semantics; conditional branches map to
   `B.cond` with the *same* 4-bit condition encoding (`EQ/NE/CS/CC/MI/PL/VS/VC/HI/LS/GE/LT/GT/LE`
   ↔ `0b0000…0b1111`), which is the biggest single win over x86-target translators and should
   be measured first.
2. Keep the CPSR's other bits (mode, IT-state, `GE`, `Q`, `EPSR`) in state memory, never in
   host flags — dynarmic's split (its `cpsr_nzcv` field is adjacent to `cpsr_q` and
   `cpsr_jaifm`, and `A32GetCpsr` loads them as a pair) is the model.
3. Adopt dynarmic's per-subset IR ops (`SetCpsrNZ`, `SetCpsrNZC`, `GetCFlag`) and implement
   FEX's `RedundantFlagCalculationElimination` analogue over RetroBug's IR: an op that only
   writes flags and whose result is dead is removable; an op whose flag writes are unreadable
   by the rest of the block shrinks to its cheaper subset. On AArch64 the concrete payoff is
   dropping `MSR/MRS` and false-dependency chains, and it also lets the register allocator
   keep `r0-r12` live across flag-setting ops.
4. **Lazy (helper-computed) only for the ~handful of ops whose flags don't map**: `ADC/SBC/MC
   K/MLA`-style carry chains where the guest's C is needed *and* produced mid-block,
   flag-setting shifts/rotates with carry-out (`LSR #n` C = bit n-1 with the RRX special case),
   `UMULL/SMULL/SMLAL` (64-bit result, guest N/Z from the 64-bit value but ARMv7 defines N/Z
   on bits<31:0> for `S`=1 multiply-long), `SBC`-saturating `Q` flag, and VFP `FPSCR` NZCV
   (`FCSE` is *not* host flags — keep FP flags in state, they have different NaN semantics;
   doc 01 §4's VFP point stands).
5. Explicit non-support list, ARMv7-only: Jazelle (`JAZZ`), ThumbEE, Security Extensions
   (TrustZone `monitor` mode), and the `MRC/MCR` coprocessor space beyond VFP/neon — Rosetta's
   "AVX512 unsupported, check `hw.optional.*`" pattern is the precedent for *stating* it
   rather than half-implementing it.
6. Do **not** attempt to keep guest flags in host flags across a call into host code
   (JNI wrapper, signal handler, translator exit). The host C ABI clobbers NZCV nowhere
   *formally*, but `BL`/epilogue code and any compiler-generated `ADDS` will — so the protocol
   is: on every translator exit, `MRS Xstate_cpsr_nzcv, NZCV` into state; on entry,
   `MSR NZCV, W…`. Same for `LDREX/STREX` state (§ open questions).
7. **No published cross-vendor benchmark numbers for ARM-flag laziness were found in this
   pass** — FEX/dynarmic/QEMU give design evidence, not speedups. Treat "flags in host NZCV"
   as a hypothesis with strong design support and *measure it* as the first RetroBug
   micro-benchmark (doc 10 §6 harness).

---

## 4. Guest memory model, `fastmem`, and the pointer-aliasing problem

> This is the core design question of the brief. Short answer: **yes, RetroBug needs a 4 GiB
> low, identity-mapped guest arena, and it is almost certainly obtainable on Android/arm64
> (unproven on iOS); and no, it does not by itself solve pointer aliasing — the host→guest
> direction still needs a handle table.**

### 4.1 The problem, precisely

RetroBug's normal path = host ART (64-bit, its own heap/globals/JNI env) + emulated ARMv7
native code, one process. Therefore:

* **guest→host direction:** the guest's computed pointers (`r0 + 4`, `PLT` stubs,
  `memcpy` targets, `dlsym` results) are 32-bit values that must be *valid host addresses*,
  or every single guest memory access needs a translation table (unidbg's model, which is
  why analysis emulators are far slower than launchers).
* **host→guest direction:** host ART will hand RetroBug 64-bit pointers — `JNIEnv*`, `jobject`
  (an ART indirect reference), `jclass`, `jmethodID`, `jstring` internals, `ANativeWindow*`,
  `AAssetManager*`, the result of host `malloc` inside a wrapped `libc` call — and the guest
  wants to put them in a 32-bit register. Truncation is only sound if the pointer's low 32
  bits re-identify the object.
* **both directions at once** through `JNI` (the guest stores a `jobject` it received, then
  passes it back 10 minutes later).

### 4.2 How the existing systems handle it (evidence)

| System | Guest | Host | Mechanism | What RetroBug can take |
|---|---|---|---|---|
| Houdini / Android-x86 | ARM32 | x86-32 process | same bitness ⇒ no truncation (inference, §2.2) | nothing — unavailable to RetroBug |
| Windows WoW / `CONFIG_COMPAT` | 32-bit | 32-bit view of a 64-bit kernel | kernel gives the process a **32-bit address space** | the *concept* of "guest-visible VA is a subset", enforced by the MMU |
| Rosetta 2 | x86-64 | arm64 | **whole-process translation, mixing forbidden** ([Apple](https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment.md)) | AOT cache + on-demand install pattern |
| ndk_translation, Berberis | arm64 / riscv64 | x86-64 / arm64 | 64-bit both sides; **guest and host share the address space directly**, no base offset | JNI-trampoline + guest-linker + namespace design (Apache-2.0 source) |
| box86 | x86-32 | arm32/arm64 process | guest below 4 GiB + **wrapped libraries** with explicit per-argument marshalling + **bridge** tables for fn pointers | the wrapped-vs-translated disposition table; bridge table design |
| FEX-Emu 32-bit | x86-32 | arm64/x86-64 | "32bit mmap must return pointers in the lower 32bit memory space"; pre-reserves `[0x1'0000'0000, 0x1'0000'0000'0000)` so the kernel can't violate it ([wiki](https://wiki.fex-emu.com/index.php/Development:32Bit_Syscall_Woes)) | the **high-reserve** trick (belt and braces) |
| QEMU linux-user | any | larger | `guest_base` bias (`-B`) + `reserved_va` (`-R`), i.e. either offset guest addresses or pre-reserve the whole guest VA — [`include/user/guest-base.h`](https://raw.githubusercontent.com/qemu/qemu/master/include/user/guest-base.h) (`extern uintptr_t guest_base; extern bool have_guest_base;`) | the fallback design (offset mode) and the fact that **MAP_FIXED_NOREPLACE is not guaranteed** ([patch](https://patchew.org/QEMU/20210131061930.14554-1-vfazio@xes-inc.com/)) |
| Dolphin | PPC | x86-64/arm64 | fixed host base for the guest RAM block (Wii memory mapped at `0x2300000000`) + unchecked `fastmem` | speed pattern **and** its exploitability ([write-up](https://dougallj.wordpress.com/2016-11-13/exploiting-dolphin-part-1/)) |
| dynarmic (arm64 backend) | A32/A64 | arm64 | `FastmemManager` + `exception_handler.SupportsFastmem()`: **drop the bounds check, catch SIGSEGV**, and recompile a block *without* fastmem if the fault can't be attributed (`MarkDoNotFastmem(marker)`) — [fastmem.h](https://github.com/azahar-emu/dynarmic/blob/master/src/dynarmic/backend/arm64/fastmem.h) | the exact fault-attribution + per-block-ban policy RetroBug needs |

### 4.3 Concrete recommendation for RetroBug's guest address space

1. **Layout.** At RetroBug-process start (before ART has loaded many libraries, ideally in the
   app's `nativeInit` from a fresh zygote fork), reserve the guest world:
   * Attempt `mmap(0, 0x100000000, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE)`
     → **guest VA = host VA, identity, no bias.** If `MAP_FIXED_NOREPLACE` is missing
     (pre-4.17 kernel — QEMU had to handle this), emulate it: plain `mmap` at a fixed address
     and verify the returned pointer equals the request, else `munmap` and degrade.
   * Additionally pre-reserve `[4 GiB, 1 << VA_BITS)` (`VA_BITS` is 39 on most Android arm64
     kernels, 48 with LPA2/Apple) so that *no* later kernel allocation can land where the guest
     might expect it (FEX's trick), and because 39-bit-VA hosts are a real compatibility case
     ([example bug: "linux_arm64 binary fails on 39-bit ARM64 kernels due to TCMalloc"](https://github.com/google-antigravity/antigravity-cli/issues/64)).
   * Never `MAP_FIXED` blindly over region 1 — that is how you silently unmap ART's heap.
     Reserve-then-fill (`MAP_FIXED` *inside your own reservation* only) is the safe discipline.
2. **Feasibility.** The kernel's default `mmap_base` on arm64 39-bit VA puts the top of the
   stack / main mmap region near the top of the 512 GiB range and allocates **downward**, so
   `[0, 4 GiB)` is normally almost entirely free apart from the executable + vdso +
   `mmap_min_addr` (4 KiB or 64 KiB). A 4 GiB `PROT_NONE|MAP_NORESERVE` reservation costs VA
   space, not RSS or commit — this is exactly the shape FEX and QEMU rely on. **Verification
   item:** run the reservation probe on each target device/emulator and record success/failure
   in RetroBug's device report; the iOS variant is *unproven* (open question), and on iOS
   RetroBug must be prepared to fall back to (3).
3. **Fallback: biased mode.** If a contiguous 4 GiB cannot be reserved, keep
   `guest = host - GUEST_BASE` with `GUEST_BASE` chosen so that *guest-owned* memory is
   reachable in 32 bits, and set a hard rule: any pointer produced by *host* code that must be
   visible to the guest is allocated from a **low arena inside the guest reservation**
   (bump allocator, `PROT_READ|WRITE` only). This is the only mode that also works on iOS where
   the reservation may fail, and it is QEMU's `guest_base` model.
4. **`jobject`/`jclass`/`jstring`/`jbyteArray` — the hard part.** Do not hand raw host
   pointers to the guest. ART itself uses **indirect references** for these (its
   `IndirectReferenceTable` hands out table-encoded handles rather than raw heap pointers), so
   RetroBug's clean move is a **32-bit handle table of its own**: guest sees `0x00NNNNNN`-style
   handles; every JNI entry point RetroBug exposes translates handle→host object. This makes
   the host→guest direction bitness-safe by construction and gives RetroBug a free place to
   enforce "guest may only touch objects it was given". Verify ART's exact encoding on
   64-bit (the joined cookie/index layout in `runtime/indirect_reference_table.h`) — we could
   not fetch it in this pass; **do not** assume a 64-bit `jobject` truncates losslessly.
5. **Bounce/arena path for raw host buffers.** For crossings where a *real* pointer must be
   in a guest register (`jboolean* GetPrimitiveArrayCritical`, `ANativeWindow_Buffer` pixels,
   `struct iovec`, `dl_phdr_info`, `stat` structs, `va_list`), allocate a low-arena shadow
   buffer, copy in/out on `Release*`, and record the pairing — box86's wrapping discipline.
   Also: **struct layouts differ** (guest `off_t`/`time_t`/`size_t` are 32-bit; `stat`,
   `siginfo_t`, `pthread_attr_t`, `jmp_buf` all differ from host) — FEX's "FEX runs as 64-bit,
   requiring struct rewriting" is the warning.
6. **`fastmem` + guard pages.** With identity mapping, guest loads/stores become plain host
   accesses with **no bounds check** — accept that only inside the reservation, with
   `PROT_NONE` guard pages at the arena edges and around each guest `.so`'s unmapped gaps, and
   adopt dynarmic's policy verbatim: the `SIGSEGV` handler distinguishes (a) translator
   metadata page (unhandled guest page → map/prefault and resume), (b) genuinely-invalid guest
   access → deliver `SIGSEGV` to the guest with ARM-shaped `si_code`/fault address, (c)
   anything else → chain to ART/native-crash-handler; blocks that repeatedly produce (b) near
   JIT metadata get `MarkDoNotFastmem`'d (i.e. recompiled with explicit range checks).
   Because Android apps run with ART's own handlers and `getSignalHandler`-style chaining exists
   in the native-bridge contract (§2.1), RetroBug's handler must be **installed once, be
   async-signal-safe, and chain to whatever was there before** — including the guest's own
   handlers (many games install `SIGSEGV` handlers themselves; they must receive guest-shaped
   context: a 32-bit `ucontext`, not the host's).
7. **Self-modifying / JIT'd guest code.** Guest code that writes then executes (JS engines,
   Unity's IL2CPP-less Mono JIT, `mmap(PROT_EXEC)` stubs, ARM `IC IVAU`) requires: writable
   page tracking (either `PROT_READ` on executable pages + fault-driven invalidation, or the
   page-table dirty-bit sweep emulators use), a `mprotect`→cache-invalidation hook, and
   `ic iall`/`dc civac` maintenance on the host after patching. Keep the "interpreter is
   always correct" rule (doc 01) so that an invalidation bug degrades to slow, not wrong.
8. **16 KiB-page hosts.** Android's 16 KB-page mode (`page_size_16kb` images — this machine has
   one, §1.4) means the *host* granule is 4× the *guest* granule. Guest `mprotect`/`munmap`
   on 4 KiB subranges therefore **cannot** be expressed with host `mprotect` alone: keep a
   shadow per-4 KiB permission table, apply host protections at 16 KiB granularity as the
   union, and enforce the finer-grained rules in the translator (bounds-checked slow path for
   the mixed pages). This also means RetroBug's guest arena must not assume `PAGE_SIZE==4096`
   anywhere in its own code.

---

## 5. App-in-app / app virtualisation on Android, and RetroBug's own distribution risk

### 5.1 Techniques used by the existing genre

`VirtualApp`-style multi-instance apps (VirtualApp/Virtual Space, Parallel Space, BlackBox,
Island/Shelter, VMOS) combine: a *fake* `IPackageManager`/`IActivityManager` obtained by
reflection on hidden `ServiceManager` binder proxies; `Instrumentation` swapping;
`ActivityThread`/`LoadedApk` re-creation to load another APK's `classes.dex` into the host
process; manifest rewriting so guest activities are stubs registered by the host ("proxy
activities"); binder transacting with `IPCThread` swapping; per-guest user dirs + `sehandle`
relabel; and (VMOS) a *real* VM. None of this is a translator; the shared part RetroBug can
reuse is **loading a foreign APK's DEX into your own process and standing up its
`Application`/`ContentProvider`/`Activity` lifecycle without `pm install`** — which RetroBug
needs anyway (§1.2: old-target guests may not even be installable).

### 5.2 What breaks on Android 12/13/14+, with evidence

* **Non-SDK interface restrictions** (Android 9+): lists are enforced on
  reflection **and JNI** (`env->GetFieldID`/`GetMethodID`) and Dalvik field/method refs;
  categories are `blocked`/blacklist, `max-target-x` (conditional block by target SDK),
  `unsupported`/greylist, and **since Android 11 (API 30) `test-api` entries are in the
  blacklist**. Consequences for virtualisation: everything in §5.1's first paragraph lives on
  hidden APIs, and each release shrinks the surface.
  [Restrictions on non-SDK interfaces](https://developer.android.com/guide/app-compatibility/restrictions-non-sdk-interfaces).
  Community bypasses exist (`LSPosed/AndroidHiddenApiBypass`, Apache-2.0,
  [repo](https://github.com/LSPosed/AndroidHiddenApiBypass);
  [XDA how-to](https://www.xda-developers.com/bypass-hidden-apis/)) and are removed over time —
  RetroBug must treat every hidden-API dependency as a **scheduled outage** and keep a
  public-API fallback.
* **`execve` from app storage is blocked** for `targetSdkVersion >= 29`
  (Android 10): SELinux forbids `appdomain` executing `app_data_file`, the change that broke
  Termux — [termux/termux-app#1072 "No more exec from data folder on targetAPI >= Android Q"](https://github.com/termux/termux-app/issues/1072),
  [Termux and Android 10 (wiki)](https://github.com/termux/termux-packages/wiki/Termux-and-Android-10/87a1cd57acef83399c1539225b284ec8bd533780),
  [PSA thread](https://www.reddit.com/r/androiddev/comments/b2inbu/psa_android_q_blocks_executing_binaries_in_your/),
  and a Magisk module that exists only to undo it ([okyes/app-data-file-exec](https://github.com/okyes/app-data-file-exec)).
  **Implication for RetroBug: it can never `fork+exec` a guest binary, and cannot ship a
  helper executable in `files/`.** Everything must be *in-process*: RetroBug's own ELF loader
  (map `PT_LOAD`s itself, run `.init_array` under translation, never `dlopen` the guest
  `.so` because the host linker rejects `ELFCLASS32`), and its own thread creation translated
  rather than spawned. (Doc 09 covers the complementary `execmem`/JIT permission story.)
* **Play availability pressure:** even the app-cloning vendors had to publish 64-bit packages
  (Parallel Space's own listing is literally "Parallel Space - **64Bit Support**",
  `com.lbe.parallel.intl.arm64`,
  [Play](https://play.google.com/store/apps/details?id=com.lbe.parallel.intl.arm64)) —
  a 32-bit-only *helper* app is now undistributable on new devices, so RetroBug's own shell
  must be arm64 (and API-36-targeting from 2026-08-31, §1.2).
* **Distribution viability (unverified):** we did **not** find a dated, citable Play takedown
  of a virtualisation app with its stated reason; the specific claims circulating
  ("Parallel Space was banned") trace to *India's* app ban list, not Play policy
  ([Quora summary](https://www.quora.com/What-is-the-alternative-of-the-now-banned-Chinese-app-Parallel-Space)).
  RetroBug's real distribution risks are the generic ones: (i) a policy line against apps that
  run/download other apps' code, (ii) "device and network abuse"/side-loading categories, and
  (iii) hosting or encouraging piracy of APKs it did not author. See open questions; the
  licensing agent should confirm against the current Play content policies.

---

## 6. Is anybody already doing exactly this? (searched hard; the space looks empty)

Searches run (2026-09-18): `armv7 emulator android arm64`, `run armeabi-v7a on arm64 only`,
`32-bit android app support 2025`, `android 16 32-bit drop`, `aarch32 dynamic binary
translator arm64 Android JNI`, `ARM32 emulator Android arm64 native libraries translation`,
`x86_64 emulator armeabi-v7a Android 13`. **Result: no public ARMv7(A32/T32)→AArch64 Android
app translator, and no open project claiming "run 32-bit Android APKs on 64-bit-only devices"
as its goal.** The nearest things, with what each is *not*:

| Project | What it is | Why it isn't RetroBug |
|---|---|---|
| [unidbg](https://github.com/zhkl0228/unidbg) (Apache-2.0) | emulates Android `.so` for RE; ARM32+ARM64; unicorn/dynarmic/KVM backends | analysis tool on a JVM host; guest memory ≠ host memory; no APK launch, no UI/audio/GL |
| Intel Houdini | ARM→x86 Android native bridge | closed-source, dead, host process was 32-bit x86 (§2.2) |
| [ndk_translation](#14-what-google-ships-today-for-foreign-abi-execution-measured-on-this-machine) | arm64→x86_64 in Android emulator/ARC | 64-bit guest only; system integration, not an app |
| [Berberis](https://android.googlesource.com/platform/frameworks/libs/binary_translation/) | riscv64→x86_64 (and arm64 host variants), AOSP, Apache-2.0 | wrong guest; but the best structural template |
| [FEX-Emu](https://wiki.fex-emu.com/index.php/Development:32Bit_Syscall_Woes), [box86/box64](https://box86.org/2021/08/a-deep-dive-into-library-wrapping/) | x86/x86-64 ↔ arm64 DBTs | wrong guest ISA; no Android runtime layer; but solve 32-bit-in-64-bit-process (§4.2) |
| QEMU `qemu-arm` (GPL-2.0) | ARMv7 user-mode emulator; `guest_base`/`reserved_va` | GPL boundary (doc 01); no Android layer; slow |
| redroid / waydroid / Android-x86 | Android in Linux containers/PC | need ARM translation for ARM APKs; their issues document how badly 32-bit support aged ([redroid#261](https://github.com/remote-android/redroid-doc/issues/261), [redroid#669](https://github.com/remote-android/redroid-doc/issues/669)) |
| VM-on-phone (UTM, QEMU full-system, `741g/android-emulator-m1-preview`) | full system images | violates "no VM, no guest OS image"; and on Apple Silicon the preview's users asked for 32-bit ARM and there was none ([issue #2](https://github.com/741g/android-emulator-m1-preview/issues/2)) |
| Android emulator (Google) | x86_64 images with `translatedAbis=arm64-v8a` | dropped `armeabi-v7a` translation after API 30 ([appstraction#54](https://github.com/tweaselORG/appstraction/issues/54)) |

**Valuable negative finding to record in the architecture doc:** RetroBug has no direct
competitor, and the demand is visible and *unserved* (React-Native, LiteRT, XDA, obtainium
threads above). The risk is not competition; it is (a) Google re-shipping a 32-bit native
bridge (Berberis's `native_bridge_support` scaffolding is right there — RetroBug's guest
arm32 could plausibly be a Berberis frontend), and (b) the runtime/API surface (doc 02)
turning out to be the 80% of the work, as it did for Houdini.

---

## 7. Structural lessons from console/other emulator cores

Mapped onto RetroBug's components (each row = a pattern to copy or a mistake to avoid):

| Core | Pattern worth RetroBug's attention |
|---|---|
| **Dolphin** | Fixed-host-base `fastmem` (`0x2300000000` for Wii RAM) with **no bounds check** in the emitted load/store — the speed, and the published exploit chain against it ([dougallj](https://dougallj.wordpress.com/2016-11-13/exploiting-dolphin-part-1/), [Dolphin forums "Fastmem"](https://forums.dolphin-emu.org/Thread-fastmem)). Lesson: guard pages + fault attribution + an opt-out per block (see §4.3.6). |
| **PPSSPP** | A JIT with a *complete* interpreter fallback and a runtime "disable JIT on crash" switch; huge per-game compatibility database ([ppsspp guides](https://www.ppsspp.org/)) — model for RetroBug's per-app profiles (README feature) and for a compatibility-report pipeline. |
| **PCSX2** | Reusable "memory + host reserve" abstraction (`VirtMemoryReserve`) and separate `fastmem` toggle from JIT toggle — RetroBug should likewise keep `fastmem`, `jit`, `lazy-flags` as **independent** switches so perf regressions are attributable. |
| **RPCS3** | Explicit 4 GiB guest reservation with fixed `vm::base` and `utils` page protection helpers; also the "interpreter for boot, recompiler for speed" split discussed in the community ([reddit: where rpcs3 allocates emulated memory](https://www.reddit.com/r/rpcs3/comments/wy6hpk/where_exactly_rpcs3_does_allocate_all_the_memory/)). RetroBug: interpreter-only milestone 1, DBT in milestone 2, never both-at-first. |
| **xemu / Xenia (this developer's own)** | The JIT-core + guest-OS + service-layer separation and the two-entry-point build discipline already documented in `08-prior-work-local-audit.md` (P1, P2) carry straight over; Xenia's Metal backend and the iOS ports are the iOS-track evidence, not Vita3K (which, per doc 08, is **not** on this machine). |
| **dynarmic** (used by Citra/Yuzu-family) | `FastmemManager` + `DoNotFastmemMarker{LocationDescriptor, index}`: **per-basic-block** fastmem banning, and an `ExceptionHandler` abstraction; also `optimization_flags.h` as an explicit tier enum (`BlockLinking`, `ReturnStackBuffer`, …) — RetroBug should have the same "one enum of translator features" so behaviour is reportable per device/profile. |
| **unicorn** | GPL-2.0 dependency reality: it is *linked* by unidbg's JVM project but a shipped RetroBug binary cannot link it (doc 01's boundary). Use it (and QEMU) only as a **test oracle process**, never as a library. |

---

## 8. Licensing / clean-room notes (short — the licensing agent owns this)

* Prior-art anchors for "reimplementing an ISA is lawful": the BIOS clean-room line
  (Compaq v. Prosyna, Phoenix), `Sony v. Connectix` (2000) for *reverse-engineering a
  console BIOS for interoperability*, and `Apple v. Franklin` — **not re-verified in this
  pass** (a search for these was blocked by tooling); re-cite them from the licensing doc.
* Practical, verifiable facts from this pass: the permissive ecosystem RetroBug can actually
  link against is **0BSD (dynarmic) / MIT (box86, FEX) / Apache-2.0 (AOSP native-bridge &
  Berberis headers, unidbg)**; the GPL-2.0 boundary is **QEMU and Unicorn** (verified:
  `unicorn-engine/unicorn` → `"spdx_id": "GPL-2.0"`, `qemu/qemu` → `NOASSERTION` i.e. mixed
  GPL-2.0+/BSD per-file SPDX).
* **Copyright vs. functionality:** instruction *encodings* and the ARM ABI's symbol lists are
  facts, not expression; the risk concentrates in copying **decoding tables, disassembly
  text, or documentation prose**. The safe, verified route to "which symbols must RetroBug
  provide for a 32-bit Android `.so`" is the **NDK's own linker stub libraries** — see
  doc 10 §4.3 for the exact enumerated path — which are AOSP/Apache-2.0 and give the
  authoritative per-API-level 32-bit ABI surface.
* ARM's own materials: the Architecture Reference Manual (DDI 0487) and **MRA/ASL** derived
  content have redistribution constraints; use the *manual's semantics* via independent
  reimplementation and use machine-readable test vectors only from sources whose license
  permits it (doc 10 §6).

---

## Open questions / unverified

1. **DEF CON 29 Houdini talk**: PDF fetch failed twice (`fetch failed`), so its specifics
   (engine layout, syscall path, perf) are cited by title only. Re-read offline; it is the
   single most relevant engineering document on this list.
2. **Houdini's process bitness** (that an ARM32 app ran inside a *32-bit* x86 host process) is
   an inference from the ABI model, not a quoted fact. Verify in the PDF or Android-x86 source.
3. **Rosetta cache specifics**: `/var/db/oah`, `oahd`/`aotctl`/`xtld`, and the `LC_ROSETTA`-style
   load command — no fetchable source found this pass. Also unverified: published Rosetta
   overhead numbers.
4. **Arm64EC/Arm64X mechanics** (EC ABI differences, hybrid PE load-config flags, what is
   "thunked" vs "emulated"): only the doc titles were confirmed; `emulators.com` fetch failed.
5. **ART `jobject` encoding on 64-bit** (`IndirectReferenceTable` joined cookie layout, whether
   a truncated local ref is recoverable) — the header fetch returned nothing; must be read from
   `platform/art/runtime/indirect_reference_table.h` before §4.3.4 is finalised.
6. **iOS**: whether a contiguous 4 GiB `PROT_NONE` reservation (or a `MAP_NORESERVE`-style
   equivalent) succeeds in an iOS app process, and whether iOS's JIT/`MAP_JIT` rules permit
   RetroBug's translated-code arena at all (doc 04's territory). Treat §4.3.3 (biased mode) as
   the iOS plan until measured.
7. **Android `mmap` policy**: no evidence found that Android's seccomp/SELinux blocks
   `MAP_FIXED_NOREPLACE` or huge `PROT_NONE` reservations for apps, but no evidence that it is
   blessed either → probe on device (add to RetroBug's device-report task list).
8. **CONFIG_COMPAT status on current Android kernels** (which shipping OEM kernels still build
   it, and whether `binfmt_misc`/`compat` ELF loading is reachable from an app's `execve`) —
   not resolvable from public docs; requires the §1.3 device probe matrix.
9. **Play policy enforcement history for virtualisation/multi-instance apps**: no dated,
   citable takedown with a stated reason found. Do not rely on anecdotes.
10. **ExaGear**: existence and discontinuation unverified this pass (no fetchable source);
    worth a follow-up because it is a *shipped* ARM32-in-64-bit-process DBT.
11. **Measured speedups for lazy vs eager flags**: none found. Design evidence only (§3.2.7).
12. **Android 14's exact "targetSdk < 23 cannot install" wording**: corroborated only by
    third-party issues; fetch the official behavior-changes page/section for the verbatim rule.
13. **Memory-model detail for ARMv7→AArch64** (whether AArch64's weaker default ordering
    permits omitting guest `DMB`s entirely, and the `LDREX/STREX` → `LDAXR/STLXR` +
    ABA/monitor-width story): the ATC'24 paper is fetched-by-title only here.
14. **Apple silicon AArch32-at-EL0**: community threads
    ([HN 27277351](https://news.ycombinator.com/item?id=27277351)) indicate Apple never enabled
    it, but no Apple doc was fetched. Relevant to RetroBug's iOS track (hardware fast path is
    off the table).

## Sources

Part A pages/documents consulted (fetched unless noted):

- [Support 64-bit architectures | Compatibility](https://developer.android.com/google/play/requirements/64-bit) · [Get your apps ready for the 64-bit requirement](https://android-developers.googleblog.com/2019/01/get-your-apps-ready-for-64-bit.html) · [64-bit-only devices](https://android-developers.googleblog.com/2022/10/64-bit-only-devices.html) · [Meet Google Play's target API level requirement](https://developer.android.com/google/play/requirements/target-sdk) · [Play target API answer](https://support.google.com/googleplay/android-developer/answer/11926878?hl=en) · [Behavior changes: apps targeting Android 14](https://developer.android.com/about/versions/14/behavior-changes-14) · [Restrictions on non-SDK interfaces](https://developer.android.com/guide/app-compatibility/restrictions-non-sdk-interfaces)
- [AOSP native_bridge.h](https://android.googlesource.com/platform/art/+/refs/heads/main/libnativebridge/include/nativebridge/native_bridge.h) · [AOSP libnativebridge](https://android.googlesource.com/platform/art/+/refs/heads/main/libnativebridge/) · [Berberis README](https://android.googlesource.com/platform/frameworks/libs/binary_translation/+/refs/heads/main/README.md) · [frameworks/libs/native_bridge_support](https://android.googlesource.com/platform/frameworks/libs/native_bridge_support/+/refs/heads/main/?format=JSON) · [Berberis RISC-V Summit 2024 abstract](https://riscvsummit2024.sched.com/event/1iYuJ/berberis-dynamic-binary-translation-from-risc-v-to-x8664-on-android-lev-rumyantsev-jeremiah-griffin-google)
- [About the Rosetta translation environment](https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment.md) · [How x86 to arm64 Translation Works in Rosetta 2 (InfoQ)](https://www.infoq.com/news/2020/11/rosetta-2-translation/) · [Apple security guide: Rosetta 2](https://support.apple.com/guide/security/rosetta-2-on-a-mac-with-apple-silicon-secebb113be1/web) · [Explainer: Rosetta 2](https://eclecticlight.co/2022-12-10/explainer-rosetta-2/)
- [Arm64EC for Windows 11 apps on Arm](https://learn.microsoft.com/en-us/windows/arm/arm64ec) · [Arm64X PE files](https://learn.microsoft.com/en-us/windows/arm/arm64x-pe) · [ARM64EC vs ARM64 vs ARM64X (NVIDIA)](https://docs.nvidia.com/rtx-spark/rtx-spark-porting-guide/0.1.0/portingandemulation/arm64ecvsarm64.html) · [Arm64EC ABI for mixing x64 and Arm64 (Arm)](https://developer.arm.com/additional-resources/video-tutorials/devhub/arm64ec-abi-for-mixing-x64-and-arm64) · [ARM64 Boot Camp (emulators.com)](http://www.emulators.com/docs/abc_arm64ec_explained.htm)
- [Sleight of ARM: Demystifying Intel Houdini (DEF CON 29 PDF)](https://media.defcon.org/DEF%20CON%2029/DEF%20CON%2029%20presentations/Brian%20Hong%20-%20Sleight%20of%20ARM%20-%20%20Demystifying%20Intel%20Houdini.pdf) · [video](https://www.youtube.com/watch?v=kdd8dSifxvU) · [Android-x86 + Houdini integration](https://blog.csdn.net/Roland_Sun/article/details/50132175) · [Adding ARM native bridge to AOSP11 x86 emulator](https://chgans.design.blog/2021-05-23/adding-arm-native-bridge-to-the-aosp11-x86-emulator/) · [Intel community: still supported?](https://community.intel.com/t5/Software-Archive/ARM-Binary-Translation-On-The-Intel-x86-Platform-Still-Supported/td-p/1024981) · [redroid Houdini on Android 12](https://github.com/remote-android/redroid-doc/issues/261)
- [FEX wiki: 32Bit Syscall Woes](https://wiki.fex-emu.com/index.php/Development:32Bit_Syscall_Woes) · [FEX Flags.cpp](https://github.com/FEX-Emu/FEX/blob/main/FEXCore/Source/Interface/Core/OpcodeDispatcher/Flags.cpp) · [FEX RedundantFlagCalculationElimination.cpp](https://github.com/FEX-Emu/FEX/blob/main/FEXCore/Source/Interface/IR/Passes/RedundantFlagCalculationElimination.cpp) · [box86: A deep dive into library wrapping](https://box86.org/2021/08/a-deep-dive-into-library-wrapping/) · [box86 USAGE.md](https://github.com/ptitSeb/box86/blob/master/docs/USAGE.md) · [Wrapped vs Emulated](https://hackmd.io/@devaraja/rkXpJfGxgl)
- [dynarmic emit_arm64_a32.cpp](https://github.com/azahar-emu/dynarmic/blob/master/src/dynarmic/backend/arm64/emit_arm64_a32.cpp) · [dynarmic opcodes.inc](https://github.com/azahar-emu/dynarmic/blob/master/src/dynarmic/ir/opcodes.inc) · [dynarmic fastmem.h](https://github.com/azahar-emu/dynarmic/blob/master/src/dynarmic/backend/arm64/fastmem.h) · [QEMU target/arm/cpu.h](https://raw.githubusercontent.com/qemu/qemu/master/target/arm/cpu.h) · [QEMU include/user/guest-base.h](https://raw.githubusercontent.com/qemu/qemu/master/include/user/guest-base.h) · [QEMU: don't assume MAP_FIXED_NOREPLACE](https://patchew.org/QEMU/20210131061930.14554-1-vfazio@xes-inc.com/)
- [CONFIG_COMPAT (lkddb)](https://cateee.net/lkddb/web-lkddb/COMPAT.html) · [Mixing 64/32-bit userspace in arm64 linux](https://stackoverflow.com/questions/28334715/mixing-64-32-bit-userspace-in-arm64-linux-using-config-compat) · [Arm KA006014: AArch32/AArch64 execution state](https://support.arm.com/documentation/ka006014/1-0/) · [HN: modern Arm 64-bit cores dropped 32-bit](https://news.ycombinator.com/item?id=32473577) · [Magisk Zygote64_32](https://xdaforums.com/t/magisk-magisk-zygote64_32-enabling-32-bit-support-for-apps.4521029/) · [39-bit VA failure example](https://github.com/google-antigravity/antigravity-cli/issues/64)
- [Termux: no exec from data folder on Android Q](https://github.com/termux/termux-app/issues/1072) · [Termux and Android 10](https://github.com/termux/termux-packages/wiki/Termux-and-Android-10/87a1cd57acef83399c1539225b284ec8bd533780) · [Android Q blocks executing binaries (PSA)](https://www.reddit.com/r/androiddev/comments/b2inbu/psa_android_q_blocks_executing_binaries_in_your/) · [app-data-file-exec](https://github.com/okyes/app-data-file-exec) · [AndroidHiddenApiBypass](https://github.com/LSPosed/AndroidHiddenApiBypass) · [XDA: bypass hidden API blacklist](https://www.xda-developers.com/bypass-hidden-apis/) · [Parallel Space 64-bit on Play](https://play.google.com/store/apps/details?id=com.lbe.parallel.intl.arm64)
- [unidbg](https://github.com/zhkl0228/unidbg) · [react-native#54150](https://github.com/react/react-native/issues/54150) · [LiteRT#3926](https://github.com/google-ai-edge/LiteRT/issues/3926) · [appstraction#54 (Android 13 emulator dropped armeabi-v7a)](https://github.com/tweaselORG/appstraction/issues/54) · [android-emulator-m1-preview#2](https://github.com/741g/android-emulator-m1-preview/issues/2) · [NDK issue #1772 drop 32-bit x86](https://github.com/android/ndk/issues/1772) · [redroid#669](https://github.com/remote-android/redroid-doc/issues/669)
- [Exploiting Dolphin (dougallj)](https://dougallj.wordpress.com/2016-11-13/exploiting-dolphin-part-1/) · [Dolphin forums: Fastmem](https://forums.dolphin-emu.org/Thread-fastmem) · [r/rpcs3 on emulated-memory allocation](https://www.reddit.com/r/rpcs3/comments/wy6hpk/where_exactly_rpcs3_does_allocate_all_the_memory/) · [USENIX ATC'24: Harmonizing Memory Consistency in Cross-ISA Binary Translation](https://www.usenix.org/system/files/atc24-gao-chen.pdf) · [ArmWrestling (binary rewriting for ARM)](https://www.research-collection.ethz.ch/bitstreams/e77454f1-beb6-47c7-9bc2-62d4267fa0eb/download)
