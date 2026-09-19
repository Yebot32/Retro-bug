# RetroBug — 06. Licensing Due Diligence

> Method: every license claim below is checked against the ACTUAL LICENSE file fetched
> from the canonical repo (raw source), not README badges or blog summaries. URLs cited
> inline. This document is research input, not legal advice.

## Claims verified / refuted

Verdict scale: **CONFIRMED** (fetched evidence supports the claim) / **REFUTED** /
**PARTIAL** (true in part) / **UNVERIFIABLE**.

| # | Claim under review | Verdict | Evidence URL fetched | Exact license text / finding (quoted) |
|---|--------------------|---------|----------------------|----------------------------------------|
| 1a | Dynarmic is **0BSD** | **CONFIRMED** | `https://raw.githubusercontent.com/azahar-emu/dynarmic/master/LICENSE.txt` | *"Copyright (C) 2017 merryhime `<git@mary.rs>` — Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted."* = verbatim 0BSD. SPDX `0BSD`. NOT GPL → the "0BSD" claim is **upheld**, not refuted. |
| 1b | Dynarmic `arm_32` already JITs ARM32→**x64** | **CONFIRMED** | `.../master/CMakeLists.txt`, `.../master/src/dynarmic/CMakeLists.txt` | `DYNARMIC_FRONTENDS` defaults to `"A32;A64"`; `backend/x64/emit_x64_*` compiled under `if("A32" IN_LIST DYNARMIC_FRONTENDS)`. |
| 1c | Dynarmic `arm_32` already JITs ARM32→**AArch64** host | **CONFIRMED** | `.../master/src/dynarmic/CMakeLists.txt` (arm64 block), `.../master/.github/workflows/aarch64.yml` | `if("arm64" IN_LIST ARCHITECTURE)` compiles `backend/arm64/emit_arm64_a32.cpp`, `emit_arm64_a32_coprocessor.cpp` (VFP/NEON), `emit_arm64_a32_memory.cpp`; then `if("A32" IN_LIST DYNARMIC_FRONTENDS)` adds `a32_interface.cpp`. CI `aarch64.yml` builds `-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=29` and runs *"Test against x86_64 implementation (A32, arm)"* / *"(A32, thumb)"* under `qemu-aarch64`. So A32-on-AArch64 host is default-on and CI-tested. **The claim is TRUE, not refuted.** |
| 2a | Berberis is **Apache-2.0** | **CONFIRMED** | `.../binary_translation/.../main/Android.bp` + per-file headers in native_bridge_support `android_api/libc/stubs_arm.cpp` | No root `LICENSE` blob exists (AOSP convention); every source file carries *"Copyright (C) <year> The Android Open Source Project … Licensed under the Apache License, Version 2.0."* Contributions require Google's **CLA**. |
| 2b | Berberis has a genuine **ARM32/AArch32 guest** | **PARTIAL / claim as stated is REFUTED** | `.../native_bridge_support/.../main/native_bridge_support.mk`, `.../binary_translation/.../main/{Android.bp,interpreter,lite_translator,backend,guest_abi}` | The "arm" naming is **genuinely AArch32** (mk: *"NATIVE_BRIDGE_PRODUCT_PACKAGES_ARM: arm32-only"*, *"libclcore_neon.bc is arm32 only"*; dirs `guest_abi/arm`, `guest_state/arm`, `stubs_arm.cpp`) — so the "`arm`==x86-32 `__i386__`" theory is **REFUTED**. **BUT** the only complete/buildable *translator engine* today is **riscv64→x86_64** (`interpreter/riscv64`, `lite_translator/riscv64_to_x86_64`, `backend/riscv64_to_x86_64`). There is **no AArch32 decoder/translator backend** — see §2. |

> Note on canonicality: `github.com/merryhime/dynarmic` currently returns **HTTP 404** (repo
> removed/relocated from GitHub; not present on `codeberg.org/merryhime/dynarmic` either). The
> LICENSE text is identical across upstream-tracking forks (`azahar-emu`, `lioncash`,
> `yuzu-mirror`). Source-tree + CI evidence taken from `azahar-emu/dynarmic` @ `master`.

## 1. Dynarmic (arm_32 frontend)

**License — 0BSD (SPDX `0BSD`).** Confirmed by fetching `LICENSE.txt`. Full text is the
Zero-Clause BSD: a single grant line ("Permission to use, copy, modify, and/or distribute this
software for any purpose with or without fee is hereby granted") plus an all-caps warranty
disclaimer. Sole named copyright holder: **merryhime `<git@mary.rs>`, 2017**. There is no
copyleft, no attribution-in-binary requirement, and no patent clause.

**Can an Apache-2.0 project link/derive from it? YES, freely.** 0BSD is one of the most
permissive OSI licenses (functionally public-domain-with-disclaimer) and is *compatible* with
Apache-2.0 in both directions — you may statically link, dynamically link, copy code, or fork
it into an Apache-2.0 work with no license conflict. Obligations that actually apply:
- **No obligation from 0BSD itself** to ship source, retain notices in the binary, or grant
  patents. The only "obligation" is good hygiene + RetroBug's own Apache-2.0 §4 duty: when you
  redistribute Dynarmic source/derivatives, keep the copyright line and a license copy (put
  `Copyright (C) 2017 merryhime` + the 0BSD text under `LICENSES/` and in `NOTICE.md`).
- Because 0BSD carries **no patent grant** (unlike Apache-2.0), RetroBug's own Apache-2.0
  §3 patent grant covers *your* contributions but gives you no patent cover *from merryhime*.
  Negligible for an ISA emulator; noted for completeness.
- Vendored deps inside Dynarmic are separately licensed (e.g. the AArch64 host backend needs
  **`merry::oaknut`**, x64 needs **xbyak** (MIT) + **Zydis** (MIT); IR uses **robin-map**).
  These are permissive and compatible; list them in the REUSE manifest.

**The `arm_32` capability claim — CONFIRMED (not refuted).** All three sub-claims hold:
1. A genuine **AArch32 (A32) *guest* frontend** exists at
   `src/dynarmic/frontend/A32/` — decoders `arm.inc`, `thumb16.inc`, `thumb32.inc` (so A32 +
   both Thumb widths), plus **`vfp.inc`** (VFP) and **`asimd.inc`** (NEON/Advanced SIMD), full
   `translate/impl/*` incl. `vfp.cpp`, `asimd_*.cpp`, `coprocessor.cpp`. → VFP **and** NEON
   present for A32.
2. An **AArch64 *host* backend for that A32 frontend** exists and is compiled by default:
   `src/dynarmic/backend/arm64/{a32_interface.cpp, a32_address_space.cpp, emit_arm64_a32.cpp,
   emit_arm64_a32_coprocessor.cpp, emit_arm64_a32_memory.cpp}`. The `emit_arm64_a32_coprocessor.cpp`
   file is what lowers A32 VFP/NEON onto an AArch64 host. Gating: outer
   `if("arm64" IN_LIST ARCHITECTURE)`, inner `if("A32" IN_LIST DYNARMIC_FRONTENDS)`.
3. CI (`aarch64.yml`) builds for Android `arm64-v8a` / platform 29 and runs cross-checked
   A32 (arm + thumb) JIT output on `qemu-aarch64` against the x86_64 build.

**"Did Citra's Android/ARM64 build actually JIT ARM32 guests on AArch64 hosts?"** — Consistent
and supported: Citra's ARM11 CPU core is built on Dynarmic's A32 frontend, and Citra shipped
Android arm64 builds; Dynarmic's own CI compiles + tests the A32-on-AArch64 host path for
Android `arm64-v8a`. Treat the *Citra-specific* runtime confirmation as **CONFIRMED-by-proxy**
(the enabling code and its CI live in Dynarmic, which we verified directly). For RetroBug this
means Dynarmic's A32 frontend is a *credible, permissively-licensed* reuse candidate for a
32-bit-ARM-guest / 64-bit-ARM-host JIT — the exact architecture direction RetroBug needs.

**Caveat for the architect:** Dynarmic is an **embedded** JIT library, not a standalone emulator
— it has no OS/ELF loader, no guest-ABI/syscall layer, no `libc`. Reusing its A32 frontend buys
decode+JIT+VFP/NEON lowering only; the Android runtime/ABI surface (§2, §6) is still RetroBug's
work. Also: the canonical repo's GitHub disappearance is a **maintenance/supply-chain signal** —
pin a specific commit + vendor a copy under `LICENSES/` rather than tracking a moving URL.

## 2. AOSP Berberis & native_bridge_support

Repo roots: `https://android.googlesource.com/platform/frameworks/libs/binary_translation`
(the translator, "Berberis") and
`https://android.googlesource.com/platform/frameworks/libs/native_bridge_support`
(the guest-ABI / API-shim layer).

**License — Apache-2.0, CONFIRMED.** There is no root `LICENSE` blob (AOSP convention); every
`.cpp/.S/.bp` carries the standard *"Licensed under the Apache License, Version 2.0"* header (verified in
`android_api/libc/stubs_arm.cpp`). Copyright holder = **The Android Open Source Project**.
Implications for RetroBug:
- Fully **compatible** with RetroBug's Apache-2.0 — same license, same §3 patent grant, same
  §4 NOTICE/changes obligations. Zero friction to link/derive/redistribute.
- **Contributions upstream require Google's CLA** (individual or corporate) and go through AOSP
  Gerrit; that is only relevant if RetroBug sends changes *back* to AOSP. Consuming/redistributing
  does **not** require a CLA.
- If RetroBug *vendors* AOSP files, retain the Apache header + the AOSP copyright line, add
  "Copyright 20xx The Android Open Source Project" to `NOTICE.md`, and state modifications
  (Apache-2.0 §4(b)) — e.g. a short header note per modified file.

**Which guest arches exist, and their completeness — the falsified claim.** Fetching the tree:
- README (main): *"Dynamic binary translator to run Android apps with **riscv64** native code on
  **x86_64** devices."* The flagship, product-shipped config is **RISC-V64 guest → x86-64 host**
  (`enable_riscv64_to_x86_64.mk`, `berberis_all_riscv64_to_x86_64_defaults`).
- Code-generation engine has **only a riscv64 guest**: `interpreter/riscv64`,
  `lite_translator/riscv64_to_x86_64`, `heavy_optimizer/riscv64`, `backend/riscv64_to_x86_64`,
  `decoder` (riscv). There is **no AArch32 and no AArch64 decoder/translator** in the current tree
  — ARM/ARM64 *guest translation* (the historical "berberon" arm/arm64→x86 backends used by
  ChromeOS ARC++) has been removed as the project pivoted to RISC-V.
- AArch32 **ABI/shim** support, however, is real and present:
  `guest_abi/arm`, `guest_state/arm`, `native_bridge_support/android_api/*/stubs_arm.cpp|cc`,
  `linker.arm.map`, `init_static_tls_arm.S`; `Android.bp` still defines
  `NATIVE_BRIDGE_GUEST_ARCH_{ARM,ARM64,RISCV64}`.

**Resolving `stubs_arm.cpp` / `libc.arm.map` / `init_static_tls_arm.S` (the disputed files).** These
are the **32-bit ARM (AArch32 / armeabi-v7a)** guest targets, NOT an `__i386__` x86-32 pairing:
`native_bridge_support.mk` explicitly comments *"NATIVE_BRIDGE_PRODUCT_PACKAGES_ARM: arm32-only
product packages"* and *"libclcore_neon.bc is arm32 only"*; the `arm` files sit beside `arm64`
and `riscv64` siblings (no x86 sibling), and `guest_abi/arm/` contains AArch32 trampoline
generators. The `intrinsics/all_to_x86_32_or_x86_64` directory is about the **host** being
32- or 64-bit x86 — a different axis from the guest. So the "`arm` is secretly x86-32" hypothesis
is **REFUTED**. Historical note: `native_bridge_support`'s linker/ABI layer originated to let x86
Android run **ARM** (arm + arm64) guest libs; the arm32 ABI plumbing survives even though the
arm32 *translator* no longer ships.

**Does it build outside an AOSP tree? NO.** Build is Soong/GreenAndroid:
`source build/envsetup.sh && lunch sdk_phone64_x86_64_riscv64-trunk_staging-eng && m berberis_all`
(or `mmm frameworks/libs/binary_translation`). It depends on AOSP-only modules (bionic, ART,
`.native_bridge` multilib variants, `native_bridge_guest_linker`, config macros in `Android.bp`).
There is **no CMake / standalone** build. For RetroBug, treat Berberis as: (a) a *reference
implementation* of the Android native-bridge ABI and proxy-library generation, and (b) a source
of *individual* vendable files — **not** a library you link in a non-AOSP build.

**What `native_bridge_support/android_api`'s generator gives / does NOT give.**
Generators: `api_analysis.py`, `gen_proxy_libraries.py`, `gen_known_trampolines.py`,
`check_proxy_libraries.py`.
- **Inputs:** the platform `.map` version scripts (the exported-symbol lists per API level /
  per lib, e.g. `libc`, `libEGL`, `libGLESv2`, `libOpenSLES`, `libandroid`, `linker`), the
  `stubs_*.{cpp,cc}` interceptable-function declarations, and the host `android_api/*/proxy`
  descriptions. `api_analysis.py` parses the ELF/`.map` symbol metadata.
- **Outputs:** **host-arch "proxy" `.so` libraries** that re-export the *guest's* expected symbol
  names/ABI but forward each call into the real host implementation (auto-generated trampolines
  bridging guest↔host calling conventions). This is exactly the mechanism RetroBug needs to make
  an ARM32 guest `libEGL.so`/`libGLESv2.so`/`libOpenSLES.so`/`libandroid.so` resolve into the
  host's native GL/audio/input.
- **What it does NOT give:** it is **not** a CPU translator (no machine-code lifting — that's §1
  Dynarmic), **not** a guest loader/linker runtime (it produces the *shims*; the guest `linker`
  + `app_process` runtime live in binary_translation and are riscv-only for the engine), **not**
  the real bionic libraries (it wraps existing host libs), and it assumes a **host that already
  exports those APIs**. On an AArch64 Android host those APIs are natively present *in the host's
  own libc/libEGL*, so RetroBug can mostly bind guest→host directly; the generator's value here is
  its **`.map`/ABI-parsing tooling and the auto-trampoline pattern**, plus the symbol lists it
  encodes (useful for the 32-bit stub set in §6).

## 3. Capstone & decoder-oracle alternatives

**Capstone — BSD-3-Clause, CONFIRMED (SPDX `BSD-3-Clause`).** Fetched `LICENSE.TXT`:
*"Copyright (c) 2013, COSEINC. All rights reserved."* + the standard three-clause BSD text (note
the third clause *"Neither the name of the developer(s) nor the names of its contributors may be
used to endorse or promote…"* → it is BSD-**3**, not BSD-2; the researcher's "BSD-3" claim is
correct). Maintainer/designer: Nguyen Anh Quynh; now under the `capstone-engine` org.
**Maintenance:** active — latest release **`6.0.0-Alpha10` (published 2026-07-21)**, stable line
5.0.x. **Compatibility:** BSD-3-Clause is compatible with Apache-2.0 (may link/derive/redistribute;
retain copyright notice + license text + the endorsement disclaimer).

**Embedded LLVM-derived code.** Several Capstone backends (ARM/AArch64 among them) historically
derive opcode tables and decode logic from **LLVM's MC layer** and from **GNU binutils/GAS** opcode
tables; Capstone ships them under the single umbrella BSD-3 notice ("*If you redistribute the binary
or source code of Capstone, please attach file LICENSE.TXT*", README §License). LLVM MC at the time
of derivation was under the **University of Illinois/NCSA** license (BSD-style, compatible); modern
LLVM is **Apache-2.0 WITH LLVM-exception** (also compatible). A byte-for-byte provenance audit of
every table is out of scope here → treat the binutils-GAS-table lineage as a **residual, low
UNVERIFIABLE risk** and neutralize it with the recommendation below.

**ARM backend completeness for a *decode oracle*.** `CS_ARCH_ARM` decodes A32 (ARM), T32 (Thumb-2)
and T16, plus VFP/NEON (Advanced SIMD) mnemonics. As a **decode oracle** (bytes → mnemonic + typed
operands + PC/size) it is well suited to fuzz/cross-check RetroBug's A32/T32 decoder for
*decoding* agreement. **Limitation:** Capstone is a *disassembler* — it validates decoding and
operand structure, **not** execution semantics. It will not confirm that a VFP/NEON instruction
*computes* the right result; for that you still need a reference CPU model / unicorn (study-only) or
differential execution against a real device.

**Safe-use recommendation for RetroBug:** use Capstone **only at build/test time**, invoked from the
host-side test harness; **do not link `libcapstone` into the shipped ARM32-on-AArch64 binary**.
That confines it to the repo's test tooling, keeps the distributed artifact clean, and moots any
binutils-table-provenance question. If a runtime decode capability is truly needed in the product,
prefer LLVM MC (§ below) over Capstone.

**Alternatives.**
- **Keystone** (the assembler sibling): last release **0.9.2 (2020-06-21)** — effectively
  **stale**; and it is an *encoder*, not a decoder — wrong tool for a decode oracle. Its mixed
  per-file licensing (several backends under GPL-derived lineage + MIT/Apache fragments) makes it a
  poor reuse candidate. **Deprioritize.**
- **binutils `libopcodes`/`libbfd`**: **GPL-3.0+** (with libiberty exceptions). **DISQUALIFYING** for
  a proprietary-adjacent, closed-source-distributed Play/App-Store product — copyleft propagates
  through static/dynamic linkage in the eyes of the FSF; do not link into RetroBug's binary.
- **LLVM `llvm-mc` / MCInstantiator**: **Apache-2.0 WITH LLVM-exception** (SPDX `Apache-2.0 WITH
  LLVM-exception`), same family as RetroBug's license → clean to vendor. TableGen-driven AArch32 +
  Thumb-2 + VFP/NEON encoders/decoders; complete and maintained. This is the best *in-product*
  decode/encode dependency if we need one, and is also candidate (c) for host-code emission. Note
  the "LLVM exception" merely lets you link LLVM into non-Apache-licensed output without the whole
  output becoming Apache — it never adds a copyleft burden for an Apache-2.0 host project.

## 4. Arm ASL / MRA (Machine Readable Architecture)

This item splits into **three separable things** that other researchers have blurred:
the *tools*, the *machine-readable data*, and the *ISA itself*.

**(A) The ASL tools are permissive — verified.**
- `alastairreid/mra_tools` `LICENSE` = **BSD-3-Clause** (*"Copyright (c) 2017, Alastair Reid. All
  rights reserved."*). SPDX `BSD-3-Clause`.
- `UQ-PAC/aslp` (ASL partial evaluator) `LICENSE` = **BSD-3-Clause** (*"Copyright 2017-2019 Arm
  Limited; SPDX-License-Identifier: BSD-3-Clause"*).
- Note: the ARM-software ASL reference tooling repos (`ASL-to-SystemC`, `ASLi`, `isa_verilog`)
  now **404 on public GitHub** — they were the study/eval tooling; treat their availability as
  UNVERIFIABLE and do not depend on a live URL.

**(B) The MRA *data* is proprietary "All rights reserved" — verified from the actual notice.**
I downloaded Arm's own release (`developer.arm.com/.../AArch32_ISA_xml_v86A-2019-12.tar.gz`, an
official **AArch32** ISA XML set — directly relevant to RetroBug) and read
`ISA_AArch32_xml_v86A-2019-12/notice.xml` verbatim:

> *"This document is protected by copyright and other related rights … **No part of this document
> may be reproduced in any form by any means without the express prior written permission of Arm.
> No license, express or implied, by estoppel or otherwise to any intellectual property rights is
> granted by this document** unless specifically stated. … Your access … is conditional upon your
> acceptance that you will not use or permit others to use the information for the purposes of
> determining whether implementations infringe any third party patents. … Copyright © 2019 Arm
> Limited (or its affiliates). All rights reserved. … LES-PRE-20349."*

Two hard consequences for RetroBug:
1. **MRA-derived artifacts may NOT be freely redistributed.** `mra_tools/README.md` itself states
   *"All generated files include ARM's license notice"* — i.e. any decoder/encoder/tables you
   auto-generate from the MRA carry the above "all rights reserved, no reproduction without Arm's
   written permission" notice. You cannot put MRA-generated decoder tables into an Apache-2.0
   repo and distribute them on Play/App-Store without Arm's written permission. **This is the
   (ii) category — redistributing machine-readable spec-derived artifacts — and it is BLOCKED.**
2. The **anti-patent-analysis clause** is unusual and actively hostile to an open-source posture:
   it forbids using the info to judge third-party patent infringement, which collides with
   Apache-2.0's §3 patent-grant culture and with any defensive FTO analysis RetroBug might do.
   Simply *reading/using* the MRA to understand the ISA is fine, but shipping artifacts that embed
   that data inherits these terms.

**(C) The ISA / architecture itself is fair game to reimplement — the (i) category.**
Reimplementing ARMv7 (A32/T32, VFP/NEON) from the **published Arm Architecture Reference Manual**
(DDI0487, freely downloadable; and the older ARMv7-A manuals) is the normal, permitted route:
- Architectures/instruction sets are **ideas and methods of operation**, not copyrightable
  expression; the ARM ISA has been reimplemented for decades by third parties (Qualcomm Kryo,
  Apple, Cortex-lookalikes) legally.
- The **expression** you may copy is *your own* decode tables and translation code written from
  your reading of the prose + encoding diagrams — NOT a transcription of Arm's machine-readable
  XML/ASL text. The line to hold: **read the ARM ARM (prose/encoding tables) → write code**; do
  **not** pipe the MRA through a generator and ship the output.

**Precedent (how others handle it) — stated with confidence level:**
- **QEMU** (GPL-2.0, confirmed) and **Dynarmic** (§1, 0BSD): ARM decoders are **hand-written from
  the ARM ARM**, not MRA-generated. This is the dominant open-source pattern.
- **Crosvm / FEX-Emu / Dyninst**: I could not run authenticated code-search here, so the specific
  "do they ship MRA artifacts" check is **UNVERIFIABLE in this pass**; on architecture grounds,
  FEX-Emu's guest is x86 (it *emits* AArch64, doesn't decode MRA) and crosvm/Dyninst ARM decoders
  are hand-written — **no known production open emulator ships MRA-derived decoder tables.**
  Action item for §Open questions: grep those trees for `LES-PRE-20349` / "machine readable" to
  close this out.

**Bottom line for RetroBug:** **Do not** build the shipped A32 decoder from redistributed MRA data.
Either (a) hand-write the decoder from the ARM ARM (what QEMU/Dynarmic do — recommended, and the
"reuse candidate (d) ASL/MRA" should be down-scoped to *reading the spec*, not *shipping generated
tables*), or (b) if MRA generation is used at all, keep it strictly a private *authoring aid* whose
output is independently re-expressed, never committed. Candidate (d)'s "auto-generate decoders from
Arm's spec data" is therefore **legally constrained, not freely reusable.**

## 5. Graphics / audio dependencies

All licenses below are from the **fetched LICENSE file**, not badges.

| Dep | Real license (SPDX) | Fetched file | Compatible w/ Apache-2.0 binary? |
|-----|---------------------|--------------|----------------------------------|
| **MoltenVK** | **Apache-2.0** | `KhronosGroup/MoltenVK@master/LICENSE` = full Apache-2.0 text | YES (identical license). The "BSD-3" report is **WRONG**; "Apache-2.0" report is **CORRECT**. (Donated to Khronos 2018 and relicensed from a proprietary model to Apache-2.0.) |
| **ANGLE** | **BSD-3-Clause** | `google/angle@main/LICENSE` = *"Copyright 2018 The ANGLE Project Authors. All rights reserved."* + 3-clause BSD (has the TransGaming/Google/3DLabs no-endorsement clause) | YES. No GPL in the core; `third_party/` holds permissive-only deps — audit that dir if vendoring. |
| **SwiftShader** | **Apache-2.0** | `google/swiftshader@master/LICENSE.txt` = Apache-2.0; GitHub SPDX `Apache-2.0`; repo **not archived** | YES. Caveat: the copy shipping inside Android is a Google-internal fork; the public `google/swiftshader` master is the redistribution-safe source. |
| **gl4es** | **MIT** | `ptitSeb/gl4es@master/LICENSE` = MIT (*"Copyright (c) 2016-2018 Sebastien Chevalier; Copyright (c) 2013-2016 Ryan Hileman … Permission is hereby granted…"*) | YES. **The "gl4es is GPL-2.0 and therefore DISQUALIFYING" hypothesis is REFUTED — gl4es is MIT, not GPL.** No disqualification. |

So none of the four is a copyleft landmine. MoltenVK/Apache and SwiftShader/Apache add nothing
beyond RetroBug's own Apache-2.0; ANGLE/BSD-3 and gl4es/MIT require only retaining their copyright
notices in `LICENSES/`/`NOTICE.md`. (Audio: RetroBug's GLES/OpenSL ES surface is the same ABI-shim
problem as §2/§6, not a licensing problem.)

**Apache-2.0 ↔ GPL-2.0 incompatibility — precise direction.**
- The conflict is **one-way and about the *combined* work**: you may take Apache-2.0 code and, if
  you want, *relicense* it under GPL-3.0 (Apache-2.0 is explicitly approved as GPL-3.0-compatible).
  But **Apache-2.0 is NOT compatible with GPL-2.0-only.** Reason: GPL-2.0 §6 forbids "further
  restrictions," while Apache-2.0 §3 (patent-termination/retaliation) and §4 (attribution/NOTICE)
  add conditions GPL-2.0 doesn't allow — so no one can satisfy both licenses on one combined work.
- **What is blocked:** linking or otherwise combining RetroBug's Apache-2.0 code with a
  **GPL-2.0-only** program (QEMU's core, Unicorn, binutils `libopcodes` [GPL-3.0]) *into a single
  distributed work* while keeping RetroBug under Apache-2.0. That is exactly why RetroBug keeps
  QEMU/Unicorn **study-only, never linked** — correct and load-bearing.
- **static vs dynamic linking of a GPL `.so` into the Apache binary (Play/App-Store):** the FSF
  treats *both* static linking and ordinary dynamic linking (calling a library's functions) as
  creating one combined/derivative work → the result would have to be distributable under GPL-2.0,
  which you cannot reconcile with Apache-2.0 (see above). So **dynamically loading a GPL-2.0-only
  `.so` as a library, shipped together as one product, does NOT insulate RetroBug** — that's a
  common misconception. The only clean separation is "mere aggregation" / independent programs
  talking at arm's length (separate process, pipes/sockets/exec with generic interfaces), which a
  linked emulator engine is not. Practical rule for RetroBug: **do not ship or dlopen any
  GPL-2.0-only code as a linked library**; permissive (MIT/BSD/Apache/0BSD) deps are fine to link.
  The full App-Store source-offer consequences of ever distributing GPL code are in §7.

## 6. 32-bit system libraries

RetroBug must ship 32-bit ARM `libc.so`/`libm`/`libdl`/`libc++`/`liblog`/`libz` plus 32-bit stubs
for `libEGL`/`libGLESv1_CM`/`libGLESv2`/`libOpenSLES`/`libandroid`. Verified source licenses:

| Library | License (verified) | Evidence | Notes for RetroBug |
|---------|--------------------|----------|--------------------|
| **bionic `libc`** | BSD-family (`MODULE_LICENSE_BSD` + `NOTICE`) | `platform/bionic/.../main/libc/{NOTICE,MODULE_LICENSE_BSD}`; `arch-arm/` present | NOTICE aggregates **NetBSD / FreeBSD / OpenBSD** derived code (each BSD-2/4-clause style, all permissive). `arch-arm` ⇒ **arm32 builds from source**. Compatible w/ Apache-2.0. |
| **bionic `libm`** | `MODULE_LICENSE_BSD_LIKE` | `platform/bionic/.../main/libm/{NOTICE,MODULE_LICENSE_BSD_LIKE}`, `upstream-freebsd`, `upstream-netbsd` | Contains **fdlibm** (Sun) and FreeBSD `gdtoa`/libm code — permissive; keep the notices. |
| **bionic `libdl`/`linker`** | BSD (same bionic LICENSE set) | bionic tree `libdl/`, `linker/` | Permissive. |
| **LLVM `libc++`** | **Apache-2.0 WITH LLVM-exception** | `llvm/llvm-project/.../libcxx/LICENSE.TXT` | NDK/Android build already links it into apps → redistributable; add to `LICENSES/`. |
| **`liblog`** | **Apache-2.0** | AOSP `system/core` logging (`liblog`) | Standard AOSP license. |
| **`libnativehelper`** | **Apache-2.0** (`NOTICE`) | `platform/libnativehelper/.../main/NOTICE` | JNI glue; Apache-2.0. |
| **zlib** | **zlib license** | `madler/zlib/.../develop/LICENSE` (*"(C) 1995-2026 Jean-loup Gailly and Mark Adler … provided 'as-is'"*) | Permissive, compatible. |
| **SQLite** | **Public Domain** | sqlite.org ("*SQLite is in the public domain*") | No obligations; note PD status in manifest. |
| **`libEGL`/`libGLESv2`/`libOpenSLES`/`libandroid` (guest side)** | Apache-2.0 (AOSP `native_bridge_support` proxy + `opengles`/`media`/native-activity headers) | §2 android_api generator | RetroBug supplies its own 32-bit **stubs/shims**; the *shim generator* is §2 (Apache-2.0). |
| **NDK `sysroot` headers + stub `.so`s** | Apache-2.0 (bionic/opengles headers); stub libs are **link-time only, no implementation** | NDK/SDK license; SDK Agreement cl. 4.1 *"no rights … asserted over software you develop with the SDK"* | See build-vs-extract below. |

**Build from AOSP source vs. extract `.so` off a device — the key question.**
- **Build from AOSP source (RECOMMENDED, clean).** bionic, libc++, liblog, zlib, libnativehelper
  are all permissive (BSD/Apache-2.0/zlib/PD). Compiling them for `armeabi-v7a` from
  `source.android.com` AOSP gives RetroBug binaries whose *only* obligations are the standard
  permissive ones (retain copyright notices in `LICENSES/`/`NOTICE.md`). This is the same route the
  NDK itself uses. Fully distributable on Play/App-Store.
- **Extract `.so` off a *commercial OEM/GMS device* — PROHIBITED / HIGH RISK. FLAG.** A retail
  device's `/system`, `/apex`, `/vendor` images are **not** Apache-2.0 binaries: they are the OEM's
  (and Google's, for GMS) copyrighted works distributed under the **device's own license / Google
  Mobile Services (GMS) agreement**, which forbids extraction and redistribution, and the vendor
  partition frequently holds proprietary blobs (GPU/GL driver, codecs). Pulling those `.so`s and
  shipping them in RetroBug would (a) breach the device/OEM license and the Google/Android
  compatibility/GMS terms, (b) infringe copyright independent of any license, and (c) violate Play
  policies against apps that redistribute others' binaries. **Any proposal to ship device-pulled
  binaries must be rejected** — build from source instead.
- **Nuance — AOSP (non-GMS) build output is fine:** extracting from an *AOSP emulator / `aosp_*`
  product* (built by you from public source) is legally the same artifact as the previous bullet's
  "build from source," so it's acceptable; the prohibition is specifically about **GMS/OEM device
  images**, not about AOSP. When in doubt, always compile from source so provenance is auditable.
- **NDK stub libs / headers for ABI symbol lists:** using NDK `sysroot` headers and the ARM32 linker
  **stub** `.so`s to *derive the ABI symbol list* (which `libc.so`/`libEGL.so` symbols an arm32 app
  expects) is squarely within the SDK permission to "use the SDK for development," and Google
  asserts no ownership over build output. That is a legitimate way to seed RetroBug's 32-bit stub
  symbol tables. (The stubs themselves carry no implementation, so there's nothing to "ship.")

## Dependency table

Legend: **OK** = link/derive/redistribute freely in an Apache-2.0 product; **OK(test-only)** =
use in the build/test harness only, do **not** link into the shipped binary; **STUDY-ONLY** = read
for ideas under the clean-room protocol (§Clean-room), never link; **BANNED** = copyleft/proprietary
incompatibility disqualifies it from the product.

| Component | Purpose in RetroBug | License (verified) | SPDX | Verdict |
|-----------|---------------------|--------------------|------|---------|
| RetroBug core | product | Apache-2.0 | `Apache-2.0` | — |
| **Dynarmic** `arm_32` | A32/T32 decode + A32→AArch64 JIT + VFP/NEON lowering | 0BSD | `0BSD` | **OK** |
| merry::oaknut | AArch64 emitter (Dynarmic host backend) | MIT | `MIT` | OK |
| xbyak / Zydis / robin-map | (x64 path / containers) | MIT / MIT / MIT | `MIT` | OK (x64 only) |
| **LLVM MC / MCInstantiator** | host-code emitter (candidate c) | Apache-2.0 + LLVM exception | `Apache-2.0 WITH LLVM-exception` | **OK** |
| **AOSP Berberis** `native_bridge_support/android_api` | 32-bit guest proxy-lib generator (candidate e) | Apache-2.0 | `Apache-2.0` | **OK** (vendored files; AOSP-tree build only) |
| Berberis translator engine (riscv64 only) | reference for ABI/loader | Apache-2.0 | `Apache-2.0` | OK (no A32 engine to reuse) |
| **bionic** libc/libm/libdl/linker | 32-bit system libs | BSD (aggregated NetBSD/FreeBSD/OpenBSD/gdtoa/fdlibm) | `BSD-2-Clause AND BSD-3-Clause AND …` | **OK** (build from source) |
| LLVM **libc++** | 32-bit C++ runtime | Apache-2.0 + LLVM exception | `Apache-2.0 WITH LLVM-exception` | **OK** |
| AOSP **liblog**, **libnativehelper** | logging / JNI glue | Apache-2.0 | `Apache-2.0` | **OK** |
| **zlib** | compression | zlib | `Zlib` | **OK** |
| **SQLite** | storage | Public Domain | `blessing`/`CC0-1.0`-like | **OK** |
| **MoltenVK** (iOS Metal path) | GLES→Vulkan→Metal | Apache-2.0 | `Apache-2.0` | **OK** |
| **ANGLE** | GLES→Vulkan/Metal/D3D | BSD-3-Clause | `BSD-3-Clause` | **OK** |
| **SwiftShader** | software Vulkan/GL | Apache-2.0 | `Apache-2.0` | **OK** |
| **gl4es** | GLES2-on-GL1/3 shims | MIT | `MIT` | **OK** (NOT GPL — disqualification refuted) |
| **Capstone** | A32/T32 **decode oracle** in tests | BSD-3-Clause | `BSD-3-Clause` | **OK(test-only)** — keep out of shipped binary |
| **Arm ASL `mra_tools` / `aslp`** | tooling to *study* the spec | BSD-3-Clause | `BSD-3-Clause` | **OK(test-only / private authoring aid)** |
| **Arm MRA data (XML/ASL)** + anything generated from it | auto-generate shipped decoders | Arm proprietary notice, "all rights reserved", LES-PRE-20349 | *(none)* | **BANNED from product** — do not redistribute generated tables |
| **QEMU** (TCG) | study TCG-style translation design | GPL-2.0-only | `GPL-2.0-only` | **STUDY-ONLY** |
| **Unicorn** | reference/side-by-side testing | GPL-2.0-only (BSD-2 headers + GPLv2 combo) | `GPL-2.0-only` | **STUDY-ONLY** |
| binutils `libopcodes`/`libbfd` | (alt oracle) | GPL-3.0+ | `GPL-3.0-or-later` | **BANNED** |
| Keyst​one | (alt encoder) | mixed (partly GPL-derived) | `mixed` | **BANNED / avoid** (also stale, wrong direction) |
| Device-pulled `/system`/`/vendor` `.so` blobs | 32-bit libs | OEM/Google proprietary (device license / GMS) | *(proprietary)* | **BANNED** — build from AOSP source instead |
| Third-party APKs (shipped) | guest apps | third-party copyright | — | **BANNED** to ship; user-supplied import only (§Distribution) |

## Distribution obligations

### 7a. Apache-2.0 §4 for a binary + source app
Shipping on Google Play / Apple App Store *is* "distribution" under the license, so every
conveyance of RetroBug (binary and source) must, for **each** Apache-2.0-licensed (and derivative)
file it carries:
- **§4(a)** — give recipients a copy of the **Apache-2.0 license**;
- **§4(b)** — **state changes** in modified files (add a `// Modified by RetroBug: <what/when>`
  header line to every vendored/edited third-party file);
- **§4(c)** — keep, in the redistributed source, the original **copyright/license/NOTICE/license-
  link headers** (don't strip them);
- **§4(d)** — if the upstream ships a **`NOTICE`** file (bionic, AOSP, libc++ all do), **reproduce
  that NOTICE's attribution text** in RetroBug's own `NOTICE` (it is *attribution*, may be appended
  to, may not be stripped). Put the full third-party text under `LICENSES/` (§Recommended plan).
- **§3 patent grant** — distributing grants recipients a royalty-free patent license from *you* on
  your contributed code, and the grant **terminates** if a recipient files a patent suit alleging
  the work infringes. This is a feature for RetroBug (deters patent trolling), not a burden.
- **Binary-only distribution** still needs the notices *visible in the app* (an "Open-source
  licenses"/"Third-party notices" screen or an asset in the APK/IPA), because §4 attaches to the
  *work conveyed*, not only to source.

Because RetroBug's deps are almost entirely permissive, §4 is a bookkeeping exercise — there is **no
source-release mandate from the dependencies themselves.** The only copyleft (QEMU/Unicorn, §6/§5)
is intentionally kept out of the linked product, so no §4/GPL source-offer duty is triggered —
*provided* the clean-room protocol below is honored.

### 7b. Obligations when distributing GPL code on the App Store (the historical friction)
If RetroBug ever *did* convey GPL-2.0 code (e.g. by linking a GPL component), GPL §3 would require
giving every recipient the **Complete Corresponding Source** or a **written three-year offer**, and
GPL-2.0 §6 forbids "further restrictions." Apple's App Store Terms of Service historically imposed
restrictions (DRM/lock-down, no-reverse-engineering, TOS-overrides-licenses) that the FSF argued
were **incompatible** with GPL §6 → in 2010–2011 the FSF (Bradley Kuhn) publicly challenged Apple
over GPL apps. Real, citable outcomes:
- **VLC for iOS** — GPLv2 app **pulled from the App Store in Jan 2011** over the Apple-TOS vs
  GPL incompatibility (and a GPLv2-vs-its-own-libs issue); it only **returned in July 2013 after
  VideoLAN relicensed to MPL 2.0 (with a GPL option) and rewrote the app** to be dual-licensable.
- **Git2Go** (2012) — a GPLv2 iOS app flagged by the FSF; the author resolved it by relicensing
  and making source available, cited ever since as the practical "how GPL-on-iOS gets fixed" case.
- **FSF/Apple + OpenSSL** — the broader fight pushed Apple to *tolerate* GPLv3 and to permit
  third-party licensing (and GPLv3's App-Store carve-out (§14 "remote web application") came from
  this era); OpenSSL's later relicensing to Apache-2.0 (and then to the SLFA/Apache dual) is a
  separate story but often invoked alongside — treat that specific pairing as **UNVERIFIABLE** here.
- **RetroBug takeaway:** Apache-2.0 has **none of this problem** — Apple's and Google's TOS are
  compatible with a permissive license that merely requires attribution. This is an argument to
  **keep the GPL deps out** and stay Apache-2.0.

### 7c. Google Play policy risk — an app whose *purpose* is running other apps
This is a **platform-policy** risk, *separate from copyright*, and it is the biggest non-license
threat to RetroBug. Evidence:
- **Play "Device and Network Abuse"** policy (support.google.com/googleplay/android-developer/
  answer/16559646) is the ground Google has used to suspend apps that create virtualized/isolated
  runtime environments or clone other apps. Multiple developers report takedowns/suspensions under
  it (Stack Overflow Q36276810; r/androiddev 2023 suspension threads).
- **App-virtualization / app-cloner** products (**Parallel Space**, **VirtualApp**, **VMOS**,
  **DU Speed Booster**'s app-clone features) have been **repeatedly removed/rejected from Play**,
  typically under **Device and Network Abuse** (unauthorized virtualization / running un-reviewed
  code inside the host app), plus **Deceptive Behavior / Impersonation** (misrepresenting what the
  app does) and, where they repackaged others' apps, **Intellectual Property**. The core policy
  objection is *"an app that installs/runs other apps of unknown provenance inside itself,"* which
  Play treats as evading app review.
- **RetroBug risk profile:** RetroBug's purposeful, transparent "run ARM32 APKs" framing is *less*
  clone-like than Parallel Space, but it still touches the "runs other apps / dynamic code" nerve.
  Mitigations to document up front: (i) prominent, honest store listing + in-app disclosure
  (Deceptive Behavior); (ii) **do not** provide an in-app APK catalog/download of others' apps
  (that's the clause that killed the cloners); (iii) only load **user-supplied** APKs the user
  already possesses; (iv) no circumvention of other apps' protections; (v) prepare for the
  possibility of an appeal/manual review. Flag as a **genuine launch risk, not a solved problem.**

### 7d. DMCA / copyright — importing user APKs vs shipping them
- **The emulator tool itself is lawful.** Case law supports building a *compatibility* emulator:
  **Sony Computer Ent. v. Connectix** (9th Cir. 2000) — reproducing a BIOS via intermediate
  decompilation to reach unprotectable functional interfaces was **fair use**; Connectix's
  VirtualPSX sold legally. **Sega v. Accolade** (9th Cir. 1992) — disassembly for interoperability
  = fair use. **Sony v. Bleem!** (9th Cir. 2000) — the BLEEM! PlayStation emulator (and its
  comparative screenshots) upheld as fair use / lawful. These cover *clean, third-party-authored*
  emulators, which is the posture RetroBug's original ARMv7 core must keep (see §Clean-room).
  **Apple v. Franklin** (3d Cir. 1983) is the counterweight: **object code and ROM contents *are*
  copyrightable** — i.e., you may copy *functional interfaces* but you may **not** copy an OEM's
  actual BIOS/`libc` binaries wholesale (reinforces §6's "build from source, never pull device
  blobs"). *(The prompt's "Compaq v. ProSyna" citation could not be verified — closest real line of
  cases is the 1980s IBM/Compaq PC-BIOS **clean-room** litigation; marked UNVERIFIED below.)*
- **Shipping third-party APKs = infringement (distribution right).** RetroBug must **never bundle**
  apps it doesn't have the right to distribute. This mirrors console-emulator **ROM policy**: the
  emulator is legal; shipping ROMs/APKs of others' software is not.
- **A user importing *their own* APK** (their licensed copy they already own) to run on RetroBug is
  the analog of a user dumping their own game: RetroBug supplies only the *tool*, the user supplies
  the *content* — the doctrine that has kept emulators (and video/media players) out of liability.
  RetroBug should stay a neutral tool: **no APK index, no "recommended apps" store, no
  auto-download, no circumvention hooks** (DMCA §1201 anti-circumvention would otherwise attach if
  any imported APK is TPM-protected).

### 7e. Patent risk — ARM instruction-set implementation (honest note)
- **Copyright/trademark:** reimplementing the ARMv7 **architecture** is not copyright infringement
  (§4). But *"ARM," "Cortex," "Thumb," "NEON," "VFP," "Jazelle"* are **Arm trademarks** — use them
  only in truthful/descriptive ways, never as RetroBug branding or in a way implying endorsement.
- **Patents:** Arm holds patents covering specific *architectural techniques* and *micro-architectural
  implementations*. Writing an interpreter/JIT that reproduces **program-visible architectural
  behavior** (the ISA contract from the ARM ARM) is a different object from implementing Arm's
  *optimized internal* designs; a from-spec software emulator targets the former. The residual
  *patent* exposure of software emulation is **not zero and is genuinely unsettled** — there is no
  widely-recognized blanket statutory "emulation safe harbor." Be honest: this is the weakest
  leg of the legal footing and warrants counsel.
- **How others handle it:** **QEMU**, **Unicorn**, **box64**, **FEX-Emu**, **Rosetta 2** (Apple's,
  reverse direction) all *implement* ISAs to run foreign binaries without an ISA-holder patent
  license for the emulation itself — they rely on (a) the ISA interface being re-implementable and
  (b) the practical/commercial tolerance of the ISA owners, **not** on an explicit architecture
  license to emulate. **Houdini** is the instructive exception: Intel ships x86-on-ARM *under a
  commercial agreement with Google* — it is a **licensed** product, which shows big-platform players
  choose to license rather than rely on doctrine; RetroBug cannot buy an equivalent "ARM emulation"
  license cheaply.
- **Do NOT rely on Arm's `AMLL`/"AMBA"/architecture/`Developer Content` licenses** — those govern
  RTL/CPU-design and *hardware* implementation and/or Arm's own evaluation models; they neither are
  required-for nor clearly *grant* software-ISA-emulation rights, and their terms are non-standard
  and gated. The **MRA anti-patent-analysis clause** (§4: *"you will not … use the information for
  the purposes of determining whether implementations infringe any third party patents"*) is a
  concrete reason to keep Arm's machine-readable data out of RetroBug entirely. **Recommendation:**
  proceed under the QEMU/box64 "implement-from-published-spec, no architecture license" model,
  document the provenance discipline, and get a patent-FTO read from counsel before commercial
  scale-up.

## Clean-room protocol

RetroBug keeps QEMU/Unicorn (GPL-2.0) as **study-only** references. To keep the Apache-2.0 core
clean of copyleft derivation, the following protocol is binding on every contributor who reads GPL
sources.

**1. "Idea" vs "expression" for a TCG-style translator — what you may and may not take.**
- **Ideas / interfaces (takeable, not copyrightable):** the *general design* of a dynamic
  translator — a decoder → an intermediate/lite IR → per-host-backend lowering; basic-block
  chaining; a return-address-stack; inline `call`/`return` helpers; an "ops" enum; the *fact* that
  ARMv7 condition codes are folded into flags; how a `TB` (translation block) is keyed by entry PC;
  the *list* of ARM instructions and their semantics (that's the public ARM ARM, §4). These are
  unprotected ideas/methods/functional specs.
- **Expression (NEVER copy):** QEMU/Unicorn's actual **source code**, their verbatim struct/enum
  member ordering, their specific C function bodies, their comments, their macro spellings
  (`TCGv_i32`-style type names), their file layout, their distinctive constant tables, and any
  line you can point to and say "I typed that in while looking at it." **Do not paste. Do not
  "translate" GPL code statement-by-statement from another language.** Re-typing GPL C as slightly
  different C is still a derivative work.
- **The rule:** read a GPL design *conceptually*, close the file, then implement from the **ARM ARM
  + RetroBug's own IR design notes**. If a hunch came *only* from GPL code, record it in the audit
  log (§5 below) as an *idea*, not as copied code.

**2. Absolute no-paste rule + tooling.**
- No code, comment, identifier-unique-to-upstream, or table may be copied from any GPL-2.0 source
  into RetroBug. Pre-commit hook / CI `license-scan` greps for known GPL fingerprints and blocks
  matches (e.g. `qemu`, `tcg_`, `TCGv`, unicorn symbol names, `target/arm/` comment banners).
- Vendored **permissive** code (Dynarmic 0BSD, Berberis Apache-2.0, libc++ Apache-2.0, bionic BSD)
  is *allowed* but must be vendored whole under `third_party/<name>/` with its original
  `LICENSE`/`NOTICE` retained and an entry in `LICENSES/` + `REUSE.toml`. "Vendored" ≠ "copied into
  RetroBug files" — keep the boundary a folder, so provenance is obvious.

**3. Per-file provenance header (required on every source file).**
```
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 RetroBug contributors
// PROVENANCE: clean-room implementation from Arm ARM DDI0487 (A32/T32) + RetroBug IR spec.
// STUDIED: none        // or: "QEMU docs (design ideas only); no code copied" — GPL sources here
//                        //        are *ideas-only* and must be listed to be auditable.
```
A file whose `STUDIED:` line names a GPL project is auto-flagged for maintainer review before merge.

**4. Contributor sign-off (DCO) + license attestation.**
- Require **`Signed-off-by:`** (DCO 1.1 — *"I have the right to submit this work …"*), added by a
  `commit -s` convention enforced in CI. This is the cheap, standard mechanism and matches how
  Dynarmic/kernel/most Apache projects accept contributions.
- Add a **CLA-lite attestation** in the PR template: contributor confirms (a) no code copied from
  GPL/proprietary sources, (b) any third-party code is permissively licensed and vendored per §2,
  (c) they grant RetroBug under Apache-2.0 (contributor-license grant). For external contributors,
  Apache-2.0 §5 already covers the grant, so a **DCO is sufficient** unless RetroBug later wants a
  formal CLA — recommend **DCO now**, revisit CLA only if patent-suing risk materializes.

**5. `docs/license-audit.md` template (maintain per dependency).**
```
## <dependency-name>
- Upstream repo + commit/tag pinned: <url>@<sha>
- License (SPDX): <id>   Source of truth: <fetched LICENSE URL>   Verified: <date/who>
- Sub-licenses / NOTICE obligations: <list>
- How used: link | vendor-folder | test-only | study-only
- Files imported: <paths>   Modified? (Apache §4(b) "state changes" note): <yes/no>
- Copied from any GPL source? NO  — provenance: <clean-room | permissive vendor>
- NOTICE text appended to NOTICE.md? <yes/no>
- REUSE.toml entry added? <yes/no>
```

## Recommended licensing plan

**Keep Apache-2.0 as RetroBug's license.** It is the right choice: compatible with every reusable
component above (0BSD, BSD-3, MIT, Apache-2.0, zlib, PD), carries an express patent grant (§3) that
permissive-only licenses like 0BSD lack, and — per §7b — dodges the entire GPL-on-app-store friction
that forced VLC/Git2Go to relicense. There is no reason to change and strong reasons not to.

**Repo layout to add:**
```
LICENSE            # Apache-2.0 (already present) — leave untouched
NOTICE.md          # NEW: attribution aggregation (Apache §4(d))
LICENSES/          # NEW: full text of every third-party license
  Apache-2.0.txt   # 0BSD.txt  BSD-3-Clause.txt  MIT.txt  zlib.txt  LLVM-exception.txt …
REUSE.toml         # NEW: SPDX bill-of-materials (machine-readable)
docs/
  license-audit.md # NEW: per-dependency table (template §5)
  clean-room.md    # NEW: this protocol, contributor-facing
```

**`NOTICE.md` sketch:**
```
RetroBug
Copyright 2026 RetroBug contributors

This product bundles third-party open-source software. Attributions required by
those licenses are reproduced below. Full license texts are in LICENSES/.

== Dynarmic (0BSD) ==
Copyright (C) 2017 merryhime <git@mary.rs>   [used for the A32/T32 guest frontend]

== LLVM libc++ / libc++abi / LLVM MC (Apache-2.0 WITH LLVM-exception) ==
Copyright … The LLVM Project contributors.

== AOSP Berberis native_bridge_support, bionic, liblog, libnativehelper (Apache-2.0 / BSD) ==
Copyright … The Android Open Source Project.  [NOTICE text of each module appended here]

== ANGLE (BSD-3-Clause) == Copyright 2018 The ANGLE Project Authors.
== MoltenVK / SwiftShader (Apache-2.0) == Copyright …
== gl4es (MIT) == Copyright (c) 2016-2018 Sebastien Chevalier; (c) 2013-2016 Ryan Hileman.
== zlib (zlib) == (C) 1995-2026 Jean-loup Gailly and Mark Adler.
== SQLite == Public Domain.
(32-bit system libs libc/libm/libdl/libc++/liblog/libz are BUILT FROM AOSP SOURCE per §6;
 their bionic/NetBSD/FreeBSD/fdlibm notices are carried above, not pulled from a device.)
```

**`REUSE.toml` sketch:**
```
version = 1
[[package]]
SPDX-PackageName = "RetroBug"
SPDX-PackageSupplier = "RetroBug contributors <...>"
path = "src/" ; SPDX-License-Identifier = "Apache-2.0"
[[package]]
path = "third_party/dynarmic/**" ; SPDX-License-Identifier = "0BSD"
[[package]]
path = "third_party/berberis_android_api/**" ; SPDX-License-Identifier = "Apache-2.0"
[[package]]
path = "tests/oracle/capstone/**" ; SPDX-License-Identifier = "BSD-3-Clause" ;
  # TEST-ONLY, not linked into the shipped binary
# … one entry per vendored dependency
```

**Build-vs-source rule (from §6):** all 32-bit ARM system libraries and stubs are **compiled from
AOSP source**; device-pulled binaries are prohibited.

**Component guidance (the re-architecture-relevant calls):**
- CPU: reuse **Dynarmic `arm_32`** (0BSD) for decode + A32→AArch64 JIT + VFP/NEON; it is
  permissive and the capability is confirmed — the 0BSD/ARM32→AArch64 claims hold.
- Host emitter (candidate c): **LLVM MC** (Apache-2.0+LLVM-exception) is fine if needed; Dynarmic
  already emits AArch64 so LLVM MC is optional.
- Decoders (candidate d, MRA/ASL): use the ASL **tools** only as a private authoring aid; **do not
  ship MRA-generated tables** (§4). Hand-write or reuse Dynarmic's decoders from the ARM ARM.
- ABI shim (candidate e): reuse **Berberis `android_api`** generator (Apache-2.0) for the 32-bit
  guest proxy libs. Correct the other researchers' two errors: it is Apache-2.0 (✓), but its
  *translator* runs only riscv64→x86-64 (no ARM32 engine) and the "arm" naming is genuinely
  AArch32 (not x86-32) — so reuse the **shim/ABI** parts, not a nonexistent AArch32 JIT.
- Graphics/audio (item 5): **all four named deps are permissive** — gl4es is MIT (not GPL), so the
  only *true* disqualifiers are binutils `libopcodes` and QEMU/Unicorn-as-linkage.

## Open questions / unverified

1. **Dynarmic canonical URL.** `github.com/merryhime/dynarmic` is HTTP 404 and it is not on
   `codeberg.org/merryhime/dynarmic`; the license/source/CI evidence was taken from the upstream-
   tracking fork `azahar-emu/dynarmic@master`. **Action:** locate the new official home and pin an
   exact commit before vendoring; treat the disappearance as a supply-chain/maintenance risk.
2. **Citra Android/AArch64 runtime confirmation.** The A32→AArch64 host path is verified in
   Dynarmic itself (code + `aarch64.yml` CI + Android `arm64-v8a` build). I did **not** independently
   fetch a Citra build to prove Citra *shipped* it; recorded as CONFIRMED-by-proxy. If the
   architecture decision leans hard on "Citra already does this," get one direct primary source.
3. **Dynarmic VFP/NEON completeness for A32-on-arm64.** `emit_arm64_a32_coprocessor.cpp` + the CI
   thumb/arm cross-checks exist, but I did not run a coverage analysis of which VFP/NEON ops are
   implemented vs stubbed on the arm64 backend. **Action:** enumerate unimplemented ops before
   committing to Dynarmic as *the* core.
4. **Capstone binutils-table provenance.** Whether specific Capstone ARM tables are verbatim-derived
   from GPL binutils `libopcodes` was not audited byte-for-byte (Capstone asserts blanket BSD-3).
   Neutralized by the "test-only, do not ship" rule, but flag if Capstone ever moves into-product.
5. **MRA precedent in crosvm/FEX-Emu/Dyninst.** Authenticated GitHub code-search was unavailable in
   this environment (needs a token; unauthenticated `/search/code` returns "Requires auth"). The
   conclusion ("no production open emulator ships MRA-derived tables") rests on project architecture
   + QEMU/Dynarmic being hand-written. **Action:** grep those trees for `LES-PRE-20349` /
   "machine readable" to close definitively.
6. **Exact current Arm MRA *user* terms.** I verified the license notice embedded **inside** the
   2019-12 AArch32 ISA XML tarball (`notice.xml`, verbatim). The click-through `developer.arm.com`
   download terms may add or modify rights (there is sometimes a separate "may use internally"
   grant). Re-check the current click-through before any internal (even private) MRA generation,
   because the anti-patent-analysis covenant may bind beyond redistribution.
7. **"Compaq v. ProSyna."** Could not verify a case by this name. The interoperability/clean-room
   line I *can* cite is the IBM/Compaq PC-BIOS cases + **Sony v. Connectix**, **Sega v. Accolade**,
   **Sony v. Bleem!**, **Apple v. Franklin**. Confirm the intended citation.
8. **Play policy verbatim text.** The "Device and Network Abuse" clause wording was not captured
   verbatim (the Google support page returned CSS only to the fetcher); named-app removals
   (Parallel Space / VMOS / VirtualApp) are reported, not primary-sourced here. **Action:** capture
   the current policy subsection text + a concrete removal notice before launch, and consider a
   pre-submission policy review with Google.
9. **Apple GPLv3 posture.** Apple tolerates GPLv3 and updated TOS (cl. ~2? ) post-2014, but the
   exact current clause set was not re-fetched; low stakes for RetroBug (it is Apache-2.0), noted
   for completeness.
10. **iOS executable-memory / JIT interaction with licensing.** Out of scope here (covered in
    `09-android-executable-memory-jit-permissions.md`), but the *feasibility* of an AArch64 JIT on
    iOS (no writable+executable pages) is a hard technical constraint that no license choice can
    resolve — flag for the architecture doc so Dynarmic's A32→AArch64 JIT isn't assumed shippable
    on iOS.

*Not legal advice; a licensing/patent attorney should confirm items 5-8 before commercial release.*

## Sources

Fetched primary license/tree sources (this pass):
- Dynarmic 0BSD `LICENSE.txt`, `CMakeLists.txt`, `src/dynarmic/CMakeLists.txt`, `aarch64.yml` —
  `https://raw.githubusercontent.com/azahar-emu/dynarmic/master/…`
- Berberis / native_bridge_support (gitiles `?format=TEXT`) —
  `https://android.googlesource.com/platform/frameworks/libs/binary_translation/…` and
  `…/native_bridge_support/…`
- Capstone `LICENSE.TXT` — `https://raw.githubusercontent.com/capstone-engine/capstone/master/LICENSE.TXT`
- Keystone (release metadata) — `https://api.github.com/repos/keystone-engine/keystone/releases/latest`
- QEMU `LICENSE` — `https://raw.githubusercontent.com/qemu/qemu/master/LICENSE`
- MoltenVK / ANGLE / SwiftShader / gl4es `LICENSE` — KhronosGroup, google, google, ptitSeb (raw)
- bionic `libc/NOTICE`, `libc/MODULE_LICENSE_BSD`, `libm/…` — android.googlesource.com/platform/bionic
- libc++ `LICENSE.TXT` — llvm/llvm-project; zlib `LICENSE` — madler/zlib; SQLite — sqlite.org
- Arm MRA `AArch32_ISA_xml_v86A-2019-12/notice.xml` + `mra_tools/README.md` —
  `https://developer.arm.com/…/AArch32_ISA_xml_v86A-2019-12.tar.gz`,
  `https://raw.githubusercontent.com/alastairreid/mra_tools/master/{LICENSE,README.md}`
- Play Device & Network Abuse — `https://support.google.com/googleplay/android-developer/answer/16559646`
- VLC-on-iOS history — TNW "VLC for iOS returns … re-written … MPLv2 and GPLv2"; HN 2082505; ZDNET
