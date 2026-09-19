# core/ — platform-agnostic RetroBug engine

The C/C++ that is shared verbatim between Android and iOS (`docs/10` §13.1). **No Android headers, no JNI,
no Objective-C, no host GL/audio types may appear here.** If a definition needs them, it belongs in
`platform/android/` or `platform/ios/` behind one of the interfaces below.

Module index, with the interface contract for each defined in
[`docs/20-implementation-plan.md`](../docs/20-implementation-plan.md) §3 (component IDs C01–C19):

| Directory | Component | Namespace |
|---|---|---|
| `isa/` | C01 ARMv7 front end (wraps vendored Dynarmic; the only place guest semantics are defined) | `rb::isa` |
| `exec/` | C02 executors — Tier I IR interpreter, Tier J host codegen | `rb::exec` |
| `mem/` | C03 guest memory, 4 KiB guest pages, address models | `rb::mem` |
| `codecache/` | C04 code cache + instance-tagged block cache | `rb::codecache` |
| `loader/` | C05 guest ELF loader | `rb::loader` |
| `linker/` | C06 guest dynamic linker / namespace semantics | `rb::linker` |
| `syscall/` | C07 ARM EABI syscall monitor | `rb::syscall` |
| `abi/` | C08 AAPCS32 call bridge | `rb::abi` |
| `vfs/` | C10 resolver, mount trie, fd capability table | `rb::vfs` |
| `profile/` | C14 closed, namespaced profile store | `rb::profile` |
| `revival/` | C15 App Revival override engine (mechanisms 1+2) | `rb::revival` |
| `gfx/` | C12 `Channel` interface + executor-agnostic guest surface | `rb::gfx` |
| `diag/` | C17 "requested vs active" recorder | `rb::diag` |

Two invariants that are enforced by the build, not by convention, because both have been named as
silent-failure risks elsewhere in this project:

1. **`guest_base` is never assumed to be zero.** Every pointer crossing goes through `rb::mem`'s
   `Host`/`Translate`; a raw cast between a guest address and a host pointer does not compile. This is what
   keeps AD-6's two address models from becoming an iOS-only bug factory.
2. **No host-wide include of `<android/*>`, `<jni.h>`, `<GLES*/…>` or `<Metal/…>`** in this directory; a
   grep gate in CI, since the header a component includes is the clearest statement of its layer.
