# Near-native speed on iOS without a JIT — AOT translation design

**Status:** research complete, plan written 2026-10-31. Owner chose the **per-game IPA** shape
(`docs/ios-notes.txt` §4), so the generic import-any-APK app remains the fallback host.

## Goal

Run a 32-bit Android game's native ARM code on an iPhone at near-native CPU speed **without runtime code
generation**, by translating its `lib/*.so` ahead of time on a PC and linking the result into the app as
ordinary signed code. General-speed fallback stays Tier I (the interpreter), which is also the safety net
for any code the static pass cannot see.

## Measured facts these decisions rest on

Two research passes (2026-10-31) over the two real targets — `libdlgl.so` (KitKat Challenge, 364 KB text) and
`libgame.so` (Blocky Roads, 1,931 KB text):

| Discovery source | libdlgl | libgame |
|---|---|---|
| defined FUNC in `.dynsym` | 2,080 (47.8 % of text by size) | 15,072 (85.1 %) |
| `.ARM.exidx` unique function starts | 2,202 (+953 not in dynsym) | 14,694 (+2,742) |
| relocations into `.text` (vtables/.got/init) | 1,040 | 1,757 |
| `DT_INIT_ARRAY` slots | 22 | 53 |
| linker veneers (pattern-only) | ARM stubs, 1,100 B | 1,633 Thumb `movw/movt/add pc/bx` |
| union of all starts | 2,788 | 16,789 |
| bytes outside every exidx interval | 0.3 % | 1.0 % |
| BLX(reg) / BX(reg) sites | 907 / 46 | 6,731 / 2,085 |
| TBB/TBH | 0 | 292 / 180 |

- **Static discovery reaches ~99 % of `.text` as function starts**; the residual is veneers and a 20-byte
  head, both pattern-matchable. Vtables are *not* the lever (only 985 `.data.rel.ro` words point into
  libgame's text — the engine builds tables at runtime), so `exidx` plus recorded indirect targets are.
- **Neither title generates code at runtime**: no Mono/il2cpp/Unity strings; no `mprotect` or
  `cacheflush`/`__clear_cache` imports. Blocky is Cocos2d-x 3 + Box2D; KitKat is C++/GCC 4.6 with
  LodePNG/OpenAL. (One open check: whether a libgame `svc` is `__ARM_NR_cacheflush` — 74 `svc` sites.)
- The per-op interpreter (Tier I) is the only iOS execution path today: measured 42.97 M IR-steps/s on an
  SM-X710 after the dense-slot change (3.6x), and KitKat's atlas load spends minutes inside
  `LodePNG_zlib_decompress` — guest CPU, not I/O.

### Dynarmic's AArch64 emission is separable; dispatch and the prelude are not

- Emission is a free function: `EmitArm64(CodeGenerator&, IR::Block, const EmitConfig&, FastmemManager&)`
  (`external/dynarmic/src/dynarmic/backend/arm64/emit_arm64.h:166`), producing
  `Relocation{code_offset, LinkTarget}` and `BlockRelocation{code_offset, type}` records
  (`emit_arm64.cpp:270-288`); the summarized result is `EmittedBlockInfo` (`emit_arm64.h:101-107`), today
  behind protected members of `AddressSpace`.
- Everything absolute is applied afterwards by the address space: `Link()` patches `B/BL` to prelude
  trampolines (`address_space.cpp:134-272`), `LinkBlockLinks()` patches cross-block edges (`:274-301`).
  The prelude bakes callback addresses, the page-table register and the JIT-state register layout
  (`a32_address_space.cpp:23-72,193-357`; `abi.h`, `stack_layout.h`, `a32_jitstate.h`).
- There is **no usable dump API on arm64**: `DumpDisassembly()` is `ASSERT_FALSE`
  (`a32_interface.cpp:125-127`); only x64 has `BlockOfCode::GetCodeBegin`.
- One hole found: coprocessor callbacks bake absolute `MOV X0,imm64; MOV Xscratch0,fn`
  (`emit_arm64_a32_coprocessor.cpp:29-34`) with no relocation record; they must become literal loads.
- iOS packaging: a **static library linked into the app binary** is permitted with zero uncertainty; a
  bundled dylib + `dlopen` depends on every sideload path re-signing nested code (unverified); a runtime
  `mmap` RX blob is dead (RWX is downgraded, `mprotect` RW→RX is EACCES, `MAP_JIT` needs an entitlement).

## Architecture

```
 PC (Windows)                                  CI (macOS)                       iPad
 ─────────────                                 ──────────                       ────
 rb_aotgen game.apk ──► code map (RBCM)  ──►  compile aot_<game>.S      ──►  RetroBug+<game>.ipa
                    ──► aot_<game>.S           link into app binary            (signed by sideload)
                    ──► aot_<game>_table.cpp                                    │
                                                                              imported APK (user)
                                                                                    │
                                                                        library hashes match?
                                                                           yes → AOT dispatch
                                                                            no → Tier I interpreter
```

- **Runtime** (`rb::exec::Aot`): a prelude copied from Dynarmic's `EmitPrelude`, a hash-keyed dispatcher
  (the key is the same identity Tier I already uses: PC, Thumb, FPSCR, IT — `InterpExecutor::BlockKey`),
  per-site inline caches, and the existing `UserCallbacks`/`HaltExecution` shape so memory access, host
  calls and syscalls are unchanged. Every translated block ends at the dispatcher, exactly as Dynarmic's
  blocks end at `GetOrEmit`; a miss calls Tier I for the target block and resumes translated code after it
  (the shape of `JitExecutor::InterpreterFallback`, `core/exec/jit.cpp:920`).
- **Code map (RBCM v1)**: header {magic, soname, base, text_off/len, n}, sorted
  `{u32 guest_off, u16 size, u8 flags(thumb|arm), u8 source_mask}`, plus a site table
  `{u32 site_off, u8 kind, u8 n, u32 targets[]}` for indirect sites, from static discovery merged with a
  recorded trace (`PreCodeReadHook`, `core/exec/jit.cpp:878`, on the tablet).
- **Per-game IPA**: the IPA carries translated code and the code map, never the APK. On import the app
  compares each `lib*.so` hash against the manifest; a match runs AOT, anything else falls back to Tier I.
- **Fallback interpreter**: Tier I today; the threaded pre-decode engine (separate plan) later.

## Risks, ranked

1. **Prelude/dispatch correctness** — one hand-written prelude must match Dynarmic's register/stack ABI;
   mitigated by copying `EmitPrelude` and asserting offsets.
2. **Unrecorded absolute immediates** — patch `0008` must cover the coprocessor emitters.
3. **Indirect coverage** — ~31 % of libgame's register-indirect sites were unclassified by a static decoder;
   recorded traces + inline caches + per-function fallback close it.
4. **Text size** — translated code may be several MB; per-game subsetting and jetsam limits apply.
5. **Signing** — static linking avoids it; never depend on `dlopen` of generated code.
6. **Perf unknowns** — dispatcher cost and page-table miss rate must be measured in the fixture spike.

## Decisions

- Product shape: **per-game IPA**, chosen by the owner on 2026-10-31.
- No runtime JIT is required or assumed; the JIT toggle stays but is not part of this path.
- Reuse over rewrite: `core/`'s loader/relocations/IFUNC/syscall monitor/thread engine and Dynarmic's A32
  front end are the decoder; only the AArch64 emission and dispatch are new.
