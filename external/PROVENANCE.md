# Vendored third-party source — provenance and policy

RetroBug is Apache-2.0. This directory contains upstream source kept **byte-identical** to the pinned
commit (architecture decision AD-14). Nothing in `external/` may be edited in place: RetroBug-side
changes live in [`/patches`](../patches/) as patch files applied at build time, plus wrapper code under
[`/core`](../core/). A diff against the pinned upstream commit must be empty after reverting `patches/`.

Why vendoring rather than a submodule: the engine is load-bearing, upstream is unmaintained (see below),
and a pinned byte-identical tree makes provenance auditable per file instead of relying on a moving
foreign repository.

## Pinned revisions

Imported 2026-09-19 from `https://github.com/azahar-emu/dynarmic` (master).

| Component | Commit | License (SPDX) | License file in tree | Notes |
|---|---|---|---|---|
| [dynarmic](https://github.com/azahar-emu/dynarmic) | `e77b1ba0b7da7cbe93021b01a663acfe7c4dd516` | **0BSD** | `dynarmic/LICENSE.txt` | The ARMv7 (A32/T32) front end + x64/AArch64/riscv64 backends. Canonical home `merryhime/dynarmic` returns 404 and `lioncash/dynarmic` is **archived** (pushed 2024-03-05), so this fork is the import source — a known maintenance risk we are accepting knowingly (AD-1, `docs/10` §17) |
| biscuit | `8bd0f7538b9ed7bedf90e789ffbd9eaeb484b28d` | MIT | `externals/biscuit/LICENSE.md` | x64 instruction decoder, used by the x64 backend |
| Catch2 | `76f70b1403dbc0781216f49e20e45b71f7eccdd8` | BSL-1.0 | `externals/catch/LICENSE.txt` | test harness only; not linked into shipped binaries |
| {fmt} | `7b273fbb541c7c9058800bc613f4291274b2d26e` | MIT | `externals/fmt/LICENSE` | |
| mcl | `5fc4beaf331037649b10625736b41365defb4f50` | MIT | `externals/mcl/LICENSE` | |
| **oaknut** | `94c726ce0338b054eb8cb5ea91de8fe6c19f4392` | MIT | `externals/oaknut/LICENSE` | AArch64 emitter **and** the executable-memory allocators. See §"oaknut resolves `RbCodeCache`" below |
| robin-map | `054ec5ad67440fcd65e0497e5a27ef31f53fcc7f` | MIT | `externals/robin-map/LICENSE` | |
| xbyak | `8c0098b69f95d02dea47efcdd42ab5aa2d03c72d` | BSD-3-clause ("modified new BSD license", `xbyak/xbyak/xbyak.h:9`; `COPYRIGHT`) | `externals/xbyak/COPYRIGHT` | x64 emitter for the x64 backend; ships an `Android.bp`, so it is Android-buildable |
| Zydis | `bffbb610cfea643b98e87658b9058382f7522807` | MIT | `externals/zydis/LICENSE` | used by dynarmic's own disassembler |
| Zycore | `0b2432ced0884fd152b471d97ecf0258ff4d859f` | MIT | `externals/zycore/LICENSE` | also present as `zydis/dependencies/zycore` (pinned there at `38d4f028…`, currently unchecked out) |

Every license above is permissive and compatible with Apache-2.0 distribution to Google Play and the App
Store. None is GPL. **QEMU, Unicorn and binutils `libopcodes` are deliberately absent** — GPL-2.0/GPL-3.0,
study-only per `docs/research/06`. Arm's machine-readable ISA data is also absent: it carries a
no-redistribution clause and an anti-patent-analysis clause, so decoder tables must be hand-derived from the
ARM Architecture Reference Manual.

## oaknut resolves `RbCodeCache` (`docs/10` §8.3)

Open risk **U-6** asked what oaknut actually calls to obtain executable memory. Read in-tree at
`externals/oaknut/include/oaknut/`:

- `code_block.hpp` — `mmap(PROT_READ|PROT_EXEC, MAP_ANON|MAP_PRIVATE)` for write-then-exec, and
  `PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANON|MAP_PRIVATE` for the RWX form. Under a Darwin define it uses
  `MAP_JIT` with `pthread_jit_write_protect_np()` — **the path unavailable on iOS**, which is precisely the
  constraint recorded in `docs/research/04`; under a Linux define it uses `PROT_MPROTECT(...)`, the
  kernel-6.6+ mprotect-restricted mapping that permits later `PROT_EXEC` upgrades while staying W^X-clean.
- `dual_code_block.hpp` — `memfd_create()` plus **two `MAP_SHARED` mappings of the same fd**, one
  `PROT_READ|PROT_WRITE` and one `PROT_READ|PROT_EXEC`. This is the `MemfdDualAlias` backend, already
  written and MIT-licensed.

So both `RbCodeCache` backends named in the architecture (AD-6/§8.3) have working upstream implementations we
may use, and `docs/10` §17's U-6 is closed. The `Interpreted` backend remains ours to write — it is the iOS
path and the OEM-`execmem`-denied fallback, and it needs no executable memory at all.

## Mechanical deviation from byte-identical (the only one)

Submodules were flattened into ordinary files and every nested `.git` directory plus
`dynarmic/.gitmodules` was deleted, because a gitlink would otherwise commit an empty directory. Source,
headers, build files and licence texts are untouched. `diff -r` against a fresh upstream clone must
therefore ignore `.git`/`.gitmodules` and nothing else.

## Re-import procedure

1. Fetch the new upstream commit; record it here with its license check.
2. `diff -r` against the in-tree copy; port any RetroBug change that must survive into `patches/`.
3. Delete and replace the tree wholesale; never merge in place.
4. Run the ISA suite and the Tier I ≡ Tier J back-end equivalence test before the bump is committed.
