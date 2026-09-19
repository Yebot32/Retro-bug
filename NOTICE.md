RetroBug
Copyright 2026 RetroBug contributors

This product includes software developed at third parties, as follows. Full license texts are retained
in-place inside each vendored tree and summarised in `external/PROVENANCE.md`.

================================================================================
Vendored source (external/)
================================================================================

dynarmic — 0BSD
  Copyright (C) 2017 merryhime <git@mary.rs>
  Imported from https://github.com/azahar-emu/dynarmic @ e77b1ba0b7da7cbe93021b01a663acfe7c4dd516
  Located at: external/dynarmic

oaknut — MIT
  Copyright (c) 2022 merryhime <https://mary.rs>
  Imported as a submodule of dynarmic @ 94c726ce0338b054eb8cb5ea91de8fe6c19f4392
  Located at: external/dynarmic/externals/oaknut

xbyak — BSD-3-Clause ("modified new BSD license")
  Copyright (c) 2007-2021 herumi
  Located at: external/dynarmic/externals/xbyak

biscuit — MIT; mcl — MIT; robin-map — MIT; Zydis — MIT; Zycore — MIT
  Copyright their respective authors, as stated in each licence file
  Located at: external/dynarmic/externals/{biscuit,mcl,robin-map,zydis,zycore}

{fmt} — MIT
  Copyright (c) 2012-present, Victor Zverovich and {fmt} contributors
  Located at: external/dynarmic/externals/fmt

Catch2 — Boost Software License 1.0
  Copyright the CATCH2 contributors
  Used in tests only; not linked into distributed binaries.
  Located at: external/dynarmic/externals/catch

================================================================================
Components planned but not yet vendored — attribution obligations attach when they land
================================================================================

* AArch32 bionic / libc++ / libnativehelper build outputs (guest system libraries): AOSP, BSD-3-Clause and
  Apache-2.0-with-LLVM-exception respectively. See `docs/research/06` §6. AOSP-derived code carries NetBSD-,
  FreeBSD-, and fdlibm-derived components with their own notices; those must be retained per file, not just
  per-repository.
* ANGLE / SwANGLE (SwiftShader) / MoltenVK for the graphics executors: BSD-3-Clause, Apache-2.0,
  Apache-2.0 respectively.

================================================================================
Excluded deliberately, and why
================================================================================

* QEMU and Unicorn Engine (GPL-2.0) and binutils libopcodes (GPL-3.0) are studied for design, never linked,
  and never redistributed here. `qemu-arm` may be invoked as a separate process as a test oracle, which
  keeps it outside the combined work.
* Arm's Machine Readable Architecture data and ASL-to-SystemC artefacts are not used to generate anything
  shipped, because their licence forbids redistribution of derived material and contains an anti-patent-
  analysis clause. Instruction semantics are implemented from the ARM Architecture Reference Manual.
