# M1 on-device acceptance runbook — proving emulation, not native execution

Written now, during Phase 2, so M1's final step is executed rather than improvised.
Target device (owner-confirmed available when M1 reaches this step): **OnePlus 12,
Snapdragon 8 Gen 3, no 32-bit ARM support at all.**

Local toolchain verified this session: `platform-tools/adb.exe` 1.0.41 / 37.0.0. No device
attached yet, so every command below is written to be pasted, not assumed-working-by-luck.

---

## 1. Why this device is the right one, and what it cannot prove

**Right, because it is the target class exactly.** Snapdragon 8 Gen 3 uses ARMv9.2-class
cores (Cortex-X4 / Cortex-A720 generation), which drop AArch32 execution at EL0. That is the
same fact `docs/research/01` identified as the reason hardware AArch32 is a fair-weather path
and why RetroBug exists. On this device there is **no possible native execution route** for an
`armeabi-v7a` binary, so the question "is it really emulating?" has a clean answer — and step
E1 can be a hard negative control rather than an inference.

**Two things one run here cannot settle on its own:**

| Not proven on OnePlus 12 | Why | Where it gets covered |
|---|---|---|
| 16 KiB host page vs 4 KiB guest page mixed-granule path (AD-7) | SD 8 Gen 3 / OxygenOS ships 4 KiB pages | **This device covers both granule cases, not neither.** On a 4 KiB host the native case is 4 KiB and the 16 KiB case is testable by injecting a coarser grouping — because `mmap`/`mprotect` granularity can be simulated *coarser* than the kernel's but never finer (`docs/30` §Step 4). The local x86_64 emulator (`getconf PAGESIZE` = 16384) tests 16 KiB natively and must skip its 4 KiB-only cases. So: 4 KiB + injected 16 KiB here, both natively-observed behaviours across the two machines. |
| OEM variance of `execmem`/memfd-exec (U-13) | one OEM build is one data point | the 6-device probe matrix in `docs/research/05`'s test plan; OnePlus 12 is device #1 of that matrix, not the matrix |

Record these as *limits of what one device run proves*, not as accepted conclusions.

---

## 2. Methodological rule that governs the whole runbook

RetroBug's own telemetry cannot, by itself, prove RetroBug is emulating: a buggy or lying
build could report `active_tier=TierJ` while something else ran. So:

> **At least one piece of acceptance evidence must be produced by something other than
> RetroBug.** That is E1 (the Android package manager, an OS component, refusing the guest
> APK) and E4 (a control build in which translation is *disabled* and the launch must fail).
> Evidence generated inside RetroBug (E2, E3, E5) is corroborating, never sufficient alone.

---

## 3. Fixture

`com.retrobug.m1fixture` — the spike already built during Phase 2 (§1 of
`20-implementation-plan.md`), promoted into `tests/corpus/` with the no-Gradle build script
checked in. Properties that make it a good probe:

- `native-code: 'armeabi-v7a'` **only**, no `arm64-v8a`, no pure-DEX fallback.
- Its visible output (`fib(20)=6765`) is produced by ARMv7 machine code, so a wrong or absent
  number means the guest code did not execute.
- It calls into the host through exactly one seam (`NewStringUTF`), so the JNI path is covered
  without ambiguity.
- It is RetroBug's own Apache-2.0 code, so there is no licensing question in committing it.

Two additions the fixture must make for E5 (both are small, do them in the same change):
`mrc p15, 0, rN, c0, c0, 0` (MIDR) read, and a Thumb-2-only arithmetic sequence
(`movw`/`movt` pair) whose result is printed. Both are unexecorable natively on this device and
both exercise the CP15 / T32 paths named in `docs/research/01`.

---

## 4. Capture protocol

Run as one script, writing a single evidence directory, zipped and kept with the milestone.

```bash
D=../m1-evidence-$(date +%Y%m%d-%H%M%S); mkdir -p $D

# 0. Device identity — proves the ABI situation from the OS side
adb -s SERIAL shell getprop | grep -E "abilist|native_bridge|qemu|ro.product.model|ro.build.version" > $D/props.txt
adb -s SERIAL shell cat /proc/cpuinfo > $D/cpuinfo.txt
adb -s SERIAL shell getconf PAGESIZE > $D/pagesize.txt          # expect 4096 here; see §1

# E1. The platform itself refuses the guest — the load-bearing, RetroBug-independent proof
adb -s SERIAL install -r -g m1fixture.apk                > $D/E1_install_refused.txt 2>&1
adb -s SERIAL shell pm list packages | grep retrobug.m1fixture > $D/E1b_not_installed.txt 2>&1
adb -s SERIAL shell dumpsys package com.retrobug.m1fixture     > $D/E1c_dumpsys.txt 2>&1
#   PASS: install rejected (expect INSTALL_FAILED_NO_MATCHING_ABIS or an ABI-equivalent
#   failure), no package entry, dumpsys finds nothing.
#
#   ALREADY MEASURED, AND STRONGER THAN ASSUMED (docs/30 §Step 5d): before any APK is involved,
#   the kernel refuses to *start* a 32-bit ELF on this device —
#       /system/bin/sh: ./aarch32_probe: not executable: 32-bit ELF file
#   i.e. ENOEXEC at the execve boundary, not a package-manager policy decision. Evidence at this
#   level cannot be explained by any native path, so keep both: the exec probe as the hard control
#   and the install refusal as the user-visible one. Re-run tools/aarch32_el0_probe.c per device:
#   the Tab S9 *does* execute the same binary, so this control is device-dependent and must never
#   be assumed from a spec sheet.

# Baseline: RetroBug's own APK is installed and the guest file is present on-device
adb -s SERIAL install -r retrobug.apk
adb -s SERIAL shell run-as com.retrobug ls -l files/imported   > $D/imported.txt

# E2. Guest .so must NOT appear as a host mapping, and MUST appear in the guest map table
adb -s SERIAL logcat -c
adb -s SERIAL shell am start -n com.retrobug/.LibraryActivity
# ...drive the UI: import, then single-tap launch (see §5), then:
adb -s SERIAL shell "cat /proc/\$(pidof com.retrobug)/maps" > $D/E2a_host_maps.txt
adb -s SERIAL shell run-as com.retrobug cat files/diag/guest_maps.txt > $D/E2b_guest_maps.txt
#   PASS: libm1fixture.so absent from E2a; present in E2b with a base inside the declared
#   guest window, plus its [stack]/[tls] regions.

# E3. Emulator telemetry, including the numbers that prove translation happened
adb -s SERIAL logcat -d -v threadtime > $D/E3_logcat.txt
adb -s SERIAL shell run-as com.retrobug cat files/diag/instance-report.txt > $D/E3b_diag.txt
#   PASS: active_tier present, translated_blocks > 0, svc_dispatch_count > 0 with ARM EABI
#   syscall numbers, guest_pc_within_module true.

# E4. Control build with translation disabled must fail — proves the pipeline is load-bearing
adb -s SERIAL install -r -t retrobug-nojit-control.apk     # build flag: RB_CONTROL_NO_TRANSLATE
adb -s SERIAL shell am start -n com.retrobug/.LibraryActivity
# ...launch the same fixture, then capture; EXPECT failure at the loader stage with a named
#   diag stage, not a silent success and not a crash without a stage line.

# E5. CPU-identification divergence + Thumb-2 result, from the guest's own registers
#   (values are printed by the fixture and echoed in E3b)
```

Capture `bugreport` only if something fails — it is large and slow, and the files above are
normally sufficient.

---

## 5. Driving the UI

Deliberately manual, recorded by screen capture, because M1 is the first milestone where the
import→select→launch path exists and automation would hide exactly the kind of friction the
milestone is meant to reveal. Record `adb -s SERIAL exec-out screenrecord --time-limit 60
$D/launch.mp4` while doing: import the pushed fixture APK file → see it in the library →
**single tap** → wait for the guest's text. Also screenshot the long-press menu to show AD-17
exists, and confirm it does nothing yet on this build (that is a *pass*, not a defect).

Two things to watch that are easy to miss: whether any configuration was required (brief (f) —
zero config means the tap path needs no prompts beyond Android permissions), and whether the
UI ever displays the word "isolated" for a T0 session (it must not — AD-8 naming discipline).

---

## 6. Pass/fail table — the milestone definition

| ID | Evidence | Producer | M1 verdict |
|---|---|---|---|
| E1 | platform refuses to install the guest APK; no package entry | **Android OS** | **mandatory** (independent proof) |
| E4 | translation-disabled control build fails at a named loader stage | **build variant, not RetroBug's runtime claim** | **mandatory** (independent proof) |
| E2 | guest `.so` in guest map table, absent from host `/proc/PID/maps` | RetroBug + `/proc` | mandatory |
| E3 | `active_tier`, `translated_blocks > 0`, ARM EABI `svc` numbers, guest PC in module range | RetroBug telemetry | mandatory |
| E5 | Thumb-2 `movw/movt` result correct; MIDR reads as the emulated ARMv7 part, not the host's | guest code | mandatory |
| V | visible text `fib(20)=6765` rendered on screen | human + screenshot | mandatory |
| Z | no config required for the tap path; no "isolated" wording on T0 | human review | mandatory |

**M1 passes only if every mandatory row passes on this device and the evidence archive is
committed to the repo** (under `tests/m1-evidence/`, small text files only — no APKs, no
bugreports). E1/E4 failing while the others pass is not a partial success; it means the
demonstration is unproven, because then nothing external to RetroBug says the code really ran
under emulation.

---

## 7. What this runbook also settles, as a side effect

Answered on first execution, at no extra cost — each maps to an open risk in
`docs/10-architecture.md` §17:

- **U-2** — the 4 GiB low reservation: the reservation probe result is echoed in E3b on real
  hardware. If it fails here, `kBiased` becomes the Android default and §3 of the plan absorbs
  the cost before M2 is designed.
- **U-11** — hidden-API `Activity.attach` / `AssetManager` hosting: if E2/E3 show the guest's
  `Context` and resources work, this is green; if the guest launches but renders nothing or has
  no resources, it is the specific thing that failed and the v1a/v1b fallbacks in `docs/10` §6
  get scheduled.
- **U-13** — `execmem` and memfd-exec on an OPPO/OnePlus build: `active_tier=TierJ` plus a
  working code cache is the probe result, recorded as device #1 of the OEM matrix.
- **`translatedAbis`** — `props.txt` also tells us whether this build carries any translation
  layer of its own. It should not; if it does, E1's reasoning needs re-examining before the
  runbook is trusted on other devices.

---

## 8. Follow-on devices

After M1 passes here, the same script runs, unchanged, against: a 16 KiB-page Android 16
device (for AD-7), at least one Samsung/OPPO/vivo/Xiaomi/Huawei build (U-13 matrix), and one
device that *does* still support 32-bit natively — where the correct result is that RetroBug
still emulates, because RetroBug never asks the OS to run guest code and must not silently take
a different path on those devices.
