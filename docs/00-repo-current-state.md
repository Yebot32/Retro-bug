# Repo current state — as Phase 0 began

Working document for RetroBug development. Every later phase must build on this state,
not on an assumed blank slate. Captured 2026-09-18 against
[`Yebot32/Retro-bug`](https://github.com/Yebot32/Retro-bug) commit `faf524b`.

## 1. What exists

Four tracked files, no code, no build system.

| Path | State | Content |
|---|---|---|
| `README.md` | functional | 41 lines: project pitch, feature list, status banner, license line. No build instructions, no architecture, no screenshots, no docs links. |
| `LICENSE` | functional | Verbatim Apache License 2.0 full text, 201 lines. No `NOTICE` file. |
| `.gitignore` | functional | 34 lines. Standard Android Studio / Gradle template. |
| `file_0000000033448211bbdb620eed523634.png` | functional | 635 KB banner image, referenced by `README.md:5`. Telegram-export filename. |

Total commit history is five commits, all documentation or file uploads:

```
faf524b Update README.md
89056d9 Update README.md
f6520dc Update README.md
d6a72f2 Add files via upload        <- the banner PNG
15fb3ad Initial commit
```

Nothing is stubbed, because nothing is implemented. There is no partial CPU core, no
dependency manifest, no CI config, no source directory, no tests, no issue templates,
and no CONTRIBUTING file. No branches exist besides `main` (verified with
`git ls-remote`; the only refs are `HEAD` and `refs/heads/main`).

## 2. Conventions already established

These are weak signals, but they are real, and Phase 2 honours them rather than
proposing a competing layout.

- **Language/build**: the `.gitignore` is an Android project template. It names
  `.gradle/`, `build/`, `local.properties`, `.externalNativeBuild/`, `.cxx/`,
  `*.aab`, `*.apk`, and IntelliJ/`.idea/` artefacts. So the repo was set up expecting a
  Gradle-based Android application with native (`.cxx`, `.externalNativeBuild`) C/C++
  built through the NDK. That matches the planned shape, so the proposed structure
  extends it rather than replacing it.
- **Licensing**: Apache-2.0, already chosen, not open for reconsideration. This is a
  binding constraint on dependencies — see `docs/research/06-licensing.md`.
- **Git hygiene**: no `.gitattributes`, no protected-branch config observable from the
  clone, no `gradle/wrapper` despite the ignore rules implying Gradle. Commits so far
  came through the GitHub web UI ("Add files via upload", "Update README.md").
- **Distribution intent**: `README.md:30` states "Cross-Platform Target: Designed for
  both Android and iOS environments", so iOS was declared in scope before any code —
  consistent with the brief's instruction to architect the core for both from day one.

## 3. Conflicts with the project brief, and how each is reconciled

The existing README is not wrong so much as loose about terminology, and the brief is
explicit that terminology matters here. Three conflicts are flagged rather than silently
overridden.

### 3.1 "compatibility layer" vs CPU emulation (terminology conflict — README wins, corrected)

`README.md:7` calls RetroBug "A 32-bit Android compatibility layer for modern ARM64
devices", and `README.md:21` "an open-source compatibility layer". The brief forbids
this framing: RetroBug is *CPU emulation* — interpreting/JIT-compiling ARMv7
instructions and reproducing ARMv7 CPU and memory semantics — with the Android API
service layer sitting *on top* of the emulated CPU. "Compatibility layer" is exactly the
"thin compatibility shim" description the brief rejects.

**Reconciliation**: the README wording will be corrected to state CPU emulation with an
Android runtime layer above it. The research in `docs/research/` uses the corrected
terminology throughout. No behaviour changes; only the description does.

### 3.2 "Dynamic Translation" is ambiguous, and partly overstated (claim conflict — resolved by deferring the claim)

`README.md:26` advertises "Dynamic Translation: High-performance ARMv7 → ARM64 dynamic
binary translation."

Two problems. First, "dynamic translation" in this project's vocabulary is also the name
for *API* binary translation, which the brief explicitly excludes; the README does not
say it is instruction-level. Second, "high-performance" is a claim Phase 0 has no
evidence for yet — no measured ARMv7-user-on-AArch64 throughput was found in any
research track (`docs/research/01-cpu-emulation-armv7-on-arm64.md`, open questions).

**Reconciliation**: instruction-level DBT *is* the intended design, so the feature is
legitimate — but it is not implemented, and on iOS a host-code JIT is not available at
all (`docs/research/04-ios-feasibility.md`), so the core must additionally run a
JIT-less data-form IR tier. The README will say "ARMv7 → AArch64 dynamic binary
translation with a JIT-less interpreter tier for platforms that forbid JIT" once
milestone 1 exists, and until then the Status warning covers it. This is left as a
to-do, not asserted now.

### 3.3 Feature list already promises unbuilt subsystems (status conflict — kept, with a caveat)

`README.md:27-29` list per-app profiles, "Advanced Environments: isolated, per-app
environment configurations", and "App Revival Patches". None exist. `README.md:34-36`
does carry an "early development / expect unfinished features" warning, which arguably
covers it, so these are not removed — but they do constrain design. Specifically, the
isolated environment described at `README.md:28` is the same subsystem the brief
defines as optional, per-app, network-off-by-default and never the default path, and
the App Revival system at `README.md:29` must be data-driven rather than hardcoded
per-app. Both are treated as first-class requirements in Phase 1, not as later
inventions of ours.

### 3.4 No conflict on dependencies

There is no existing dependency choice, vendored library, or partial implementation to
reconcile against, so the licensing analysis in `docs/research/06-licensing.md` starts
clean. Had the repo already vendored QEMU or Unicorn (both GPL-2.0) under this Apache-2.0
LICENSE, that would have been a hard conflict; it does not.

## 4. Local build environment (not in the repo, but constrains every milestone)

Verified on the development host, 2026-09-18:

| Item | Status |
|---|---|
| Git | 2.54.0 |
| JDK | Temurin 21.0.11 (`javac` 21.0.11) |
| Android SDK | present at `%LOCALAPPDATA%/Android/Sdk` |
| Platforms | `android-35`, `android-36`, `android-36.1` |
| Build-tools | `35.0.0`, `36.1.0`, `37.0.0` |
| NDK | `28.2.13676358`, `29.0.14206865` |
| CMake | SDK-bundled `3.30.3`, `3.31.6` (no system `cmake` on PATH) |
| System image | `android-37.1/google_apis_playstore_ps16k` only |
| `gh` CLI | **absent** — repo operations must use `git` over HTTPS |
| `gradle` | **absent** on PATH — must use the Gradle wrapper |
| `python` | **absent** — scripts must not depend on it |
| Host CPU | `x86_64` Windows (Git Bash) |

Two consequences worth stating plainly, both elaborated in
`docs/research/10-milestone1-target-and-tooling.md`:

1. **The host cannot run an `armeabi-v7a` guest.** The CPU is x86_64, and the one
   installed system image is a 16 KB-page playstore image whose `abilist32` is empty.
   Compiling and unit-testing ARMv7 code is possible here; *launching a real 32-bit APK
   end-to-end* is not. Milestone 1 therefore requires a physical 64-bit-only arm64
   Android device. This is a hard dependency of the plan, not a detail.
2. **A useful reference implementation sits in this SDK already.** The installed image
   carries `libndk_translation.so`, Google's ARM→x86 native bridge, alongside the
   emulator's `translatedAbis=arm64-v8a` configuration. That is the closest thing to a
   shipped, working "host ART + translated native libraries" system obtainable on this
   machine, and it is inspectable locally.

Prior work by the same developer is also on disk, outside this repo, and is audited in
`docs/research/08-prior-work-local-audit.md`: `xaniteOG/` (Xbox Original emulator on
xemu/QEMU, shipping Android + iOS), `xaniteOG/XenDroid/` (its Gradle + CMake + `ios/` +
`patches/` app shell), and Xenra (Xbox 360 emulator) Android/iOS build artefacts. Those
directories are **not** part of RetroBug and nothing may be copied from them without an
individual license check; they are sources of design precedent, and of one or two
independently re-implementable ideas.

## 5. Structure proposed where none exists

Since the repo has no source layout, Phase 2 proposes one, keeping the Gradle/NDK
expectation the `.gitignore` already encodes. Documentation lands in `docs/` now:

```
docs/
  00-repo-current-state.md              <- this file
  10-architecture.md                    (Phase 1)
  20-implementation-plan.md             (Phase 2)
  research/
    01-cpu-emulation-armv7-on-arm64.md
    02-android-runtime-abi-surface.md
    03-graphics-audio-input-emulation.md
    04-ios-feasibility.md
    05-sandbox-isolation.md
    06-licensing.md
    07-prior-art-public.md
    08-prior-work-local-audit.md
    09-android-executable-memory-jit-permissions.md
    10-milestone1-target-and-tooling.md
    90-cross-check.md
```
