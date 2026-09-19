# 02 — Android runtime / ABI surface: the smallest correct emulate-vs-native cut

Research sub-agent deliverable for RetroBug. All claims below are either (a) quoted from a primary
source with URL + line number, or (b) explicitly marked **inference** / **unverified**.

Method note: nearly every AOSP file cited here was pulled as raw text from
`https://android.googlesource.com/<repo>/+/refs/heads/main/<path>?format=TEXT` (base64 body), Linux
headers from `https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/<path>`, and
Arm ABI text from `https://raw.githubusercontent.com/ARM-software/abi-aa/main/<doc>/<doc>.rst`.
Line numbers refer to `refs/heads/main` as of 2026-09 unless a commit/tag is named. Re-fetch any
file with `curl -s "<url>?format=TEXT" | base64 -d` (gitiles) or plain `curl` (kernel.org / GitHub
raw); no local copies were kept.

---

## TL;DR

**Recommended cut point (decisive):**

> **Run all *managed* code (DEX) natively on the host's 64-bit ART. Emulate *only* `lib/armeabi-v7a/*.so`.
> Hook ART at exactly one place — `java.lang.ClassLoader.findLibrary(String)` — and answer it with an
> **arm64 "shadow" `.so` that RetroBug generates per guest library**. Every exported JNI symbol in the
> shadow lib is an arm64 stub that enters RetroBug's translator with a guest-call descriptor. Everything
> else (guest ELF loading, guest relocations, guest linker, guest libc, guest syscalls) lives inside
> RetroBug's own in-process engine and is invisible to ART.**

That is the *smallest correct* cut because:

1. Android app Java/Kotlin bytecode is architecture-neutral and the host ART is a conforming
   runtime, so `classes.dex` needs nothing but a class loader (topic 1, topic 6).
2. ART's only architectural coupling to app native code is: (i) `dlopen()` of a path that
   `ClassLoader.findLibrary()` returned, (ii) `dlsym(handle,"JNI_OnLoad")`, (iii) symbol lookup by
   JNI name + *shorty*, and (iv) a `SIGSEGV` chain owner. All four are satisfiable with a host-arch
   `.so` (topic 1, topic 2).
3. Pointer size, calling convention, TLS, unwinding, and syscall ABI deltas are all confined to the
   native side; none of them leak into DEX execution.
4. The one thing you cannot get without system integration is **ART's own decision to call the
   native bridge** (`AndroidRuntime.cpp` enables it only for the zygote) — so do not try to be a
   native bridge in v1. Be a class loader instead (topic 1.4, "Native bridge verdict").

**Process architecture (normal path, one host process, no VM, no guest OS):**

```
Host Android (arm64) app process: RetroBug
├── host ART (unchanged; executes guest DEX)
├── RetroBugClassLoader extends PathClassLoader/DexClassLoader
│    ├── dex path = guest APK (base.apk / classes.dex) via normal DexClassLoader
│    └── findLibrary("foo") -> /data/data/.../nb/arm64/libfoo.so   <-- THE HOOK (public API)
│         (arm64 shadow lib; exports exactly the guest's Java_* symbols + JNI_OnLoad)
├── RetroBug's shadow-lib runtime (host .so, plain JNI, no hidden APIs)
│    ├── GuestLinker   : 32-bit ELF loader + namespace/reloc semantics (topic 4)
│    ├── GuestKernelApi: ARM EABI syscall -> host syscall translation, auxv, signals (topic 5)
│    ├── GuestLibs     : AOSP-built arm32 bionic libc/libm/libdl/liblog/... + proxy libs (topic 7)
│    ├── Translator    : A32/T32 -> arm64 DBT (fork of Berberis + new ARM32 frontend)
│    └── JniMarshal    : 32-bit handle tables + per-shorty trampolines + guest va_list (topic 2)
└── (optional, later) same engine re-exported as NativeBridgeItf v8 for ROM/OEM builds
```

**Native bridge verdict:** Android's native bridge is the right *interface* and the wrong *entry
point* for a normal APK. It is gated to zygote-launched processes, requires the implementation to be
dlopen-able from the **system** namespace by simple filename, and its pre-initialization does
`unshare(CLONE_NEWNS)` + bind-mount of `/proc/cpuinfo` — all privileged. RetroBug therefore
implements the *same contract internally* (loadLibrary / getTrampoline / signal handler /
namespaces) but reaches ART through `ClassLoader.findLibrary()` instead of through
`-XX:NativeBridge=`. (Details and citations: §1.3–§1.4.)

**Biggest single reuse opportunity (found during this research):** AOSP already contains a complete,
Apache-2.0 native-bridge stack — **Berberis** (`frameworks/libs/binary_translation`, riscv64 guest on
x86_64/arm64 host) plus **`frameworks/libs/native_bridge_support/android_api`**, which *already has
`arm` (32-bit) guest build variants*. RetroBug's engine should be a fork of Berberis with an ARM32
decoder/backend added, not a from-scratch design; the proxy-library list, guest-libc build, guest
linker, guest vdso and syscall-translation layout are all already upstream and already legal to ship.
See §4, §5, §7.

**Top risks that could invalidate parts of this:** (1) whether `dlopen()` of a RetroBug-written arm64
shadow `.so` is permitted from app-writable storage on targetSdk ≥ 29 (unverified, §6.4/OQ-3);
(2) guest Activity hosting needs hidden-API `Activity.attach` (no-root path is fragile, §6.3);
(3) 16 KB-page hosts vs 4 KB-aligned 32-bit ELFs (§5.4, §4.3).

---

## 1. The DEX/ART split — can managed code run on host ART with only `.so` emulated?

### 1.1 Short answer

Yes, in principle and almost entirely in practice. A 32-bit APK's `classes*.dex` contains no machine
code; ART's compiled output (oat/odex/vdex) is per-device and regenerated at install/AOT time, so a
guest APK's DEX is consumed by host ART unchanged. `armeabi-v7a` is a *native* ABI label
(`lib/armeabi-v7a/`) that only affects `.so` extraction and `findLibrary()` results.

What breaks, concretely:
- The `lib/<abi>/` directory the host extracts for the *installed* APK will not exist for an
  arm64-only device (PM only extracts a matching ABI), and even if it did, `dlopen()` of an
  `EM_ARM` object in an `EM_AARCH64` process fails in bionic's `linker`.
- Anything that queries the ABI sees the *host*: `Build.CPU_ABI`, `Build.SUPPORTED_ABIS`,
  `os.arch`, `ABI` in tombstones. A native bridge is allowed to rewrite `Build.CPU_ABI/CPU_ABI2`
  and `os.arch` through JNI (see `SetupEnvironment()`, `native_bridge.cc:422-479`) — RetroBug, not
  being a bridge, must spoof these from its own class-loading layer or accept app misbehaviour.
  **Inference:** many older apps branch on `CPU_ABI`/`SUPPORTED_ABIS` (e.g. to pick a codec or to
  refuse to run), so this is a real compat surface, not cosmetics.
- `targetSdkVersion` behaviour is applied by the *host* framework using the guest's manifest value
  (ART stores it per-process and per-library: `Runtime::GetTargetSdkVersion()` is passed into
  `android::OpenNativeLibrary(...)`, `java_vm_ext.cc:1067-1075`; the linker also gates on
  `get_application_target_sdk_version()`, `linker.cpp:245,803,1087`). RetroBug must be careful:
  a modern host framework applies *host*-side policy for a guest that targets API 8. This is the
  classic "old app on new framework" problem and is orthogonal to emulation (§6.5).

### 1.2 The exact Java→native load path (cited)

`System.loadLibrary(name)`
→ `java.lang.Runtime.loadLibrary(String)` → `loadLibrary0(Reflection.getCallerClass(), libname)`
(`Runtime.java:992-993`, AOSP `platform/libcore`, `ojluni/src/main/java/java/lang/Runtime.java`)
→ `Runtime.loadLibrary0(ClassLoader loader, Class<?> callerClass, String libname)`
(`Runtime.java:1054-1103`). The important lines:

```java
String filename = loader.findLibrary(libraryName);            // Runtime.java:1064  <-- hook point
... fallback: filename = System.mapLibraryName(libraryName);  // Runtime.java:1078
String error = nativeLoad(filename, loader, callerClass);     // Runtime.java:1088
```
```java
private static native String nativeLoad(String filename, ClassLoader loader, Class<?> caller);
                                                            // Runtime.java:1138
```

`ClassLoader.findLibrary(String)` is a **`protected`, documented, overridable** API
(`ClassLoader.java:2349-2351`, same file). It must return an absolute path
(`ClassLoader.java:2383-2386` enforces absoluteness in the legacy code path; the returned string is
used verbatim as the `dlopen` path). This is the only place in the whole chain where an app can
change the fate of a native load without reflection, root, or bytecode rewriting.

`Runtime.nativeLoad` → `JavaVMExt::LoadNativeLibrary()`
(`platform/art`, `runtime/jni/java_vm_ext.cc`) →
`android::OpenNativeLibrary(...)` (`java_vm_ext.cc:1067`) in **libnativeloader**
(`platform/art/libnativeloader/native_loader.cpp:294-466`) → per-class-loader
`NativeLoaderNamespace` (`libnativeloader/native_loader_namespace.cpp`) →
`android_dlopen_ext()` with `ANDROID_DLEXT_USE_NAMESPACE`
(`native_loader_namespace.cpp:166-183`).

Then, still in `java_vm_ext.cc`:
- `dlsym(handle, "JNI_OnLoad")` and call it with the host `JavaVM*`; result must be
  `JNI_VERSION_1_2/1_4/1_6` (`java_vm_ext.cc:67-70` `IsBadJniVersion`, `:1132-1148`).
- After `JNI_OnLoad`, for old targets it re-asserts sigchain ownership of `SIGSEGV`
  (`java_vm_ext.cc:1137-1140`, `EnsureFrontOfChain(SIGSEGV)`).
- Lazy native-method resolution: `Libraries::FindMethod(...)` iterates the libraries registered to
  the *declaring class's* class loader and calls `SharedLibrary::FindSymbol(name, shorty,
  jni_call_type)` (`java_vm_ext.cc:321-347`), i.e. `dlsym` by JNI short and long name.

Historical note for completeness: the JNI stub used to live in
`platform/art/runtime/native/java_lang_Runtime.cc` (`Runtime_nativeLoad` →
`JavaVMExt::LoadNativeLibrary`, fetched at commit
[`906846f/runtime/native/java_lang_Runtime.cc`](https://android.googlesource.com/platform/art/+/906846f/runtime/native/java_lang_Runtime.cc));
that file no longer exists on `main` (`runtime/native/` has 70 entries, none for
`java_lang_Runtime`, and `Runtime` is absent from `Runtime::RegisterRuntimeNativeMethods()`,
`runtime.cc:2408-2442`). **Unverified:** where `nativeLoad` is implemented on current `main`
(probable: registered from `libcore`/`art/runtime/jni/jni_internal.cc` or a renamed file). The
Java-side contract above is verified and is what matters for RetroBug.

### 1.3 Native Bridge — the official mechanism, in depth

**Where it lives now.** `libnativebridge` moved out of `system/core` into the ART tree:
[`platform/art/+/refs/heads/main/libnativebridge/`](https://android.googlesource.com/platform/art/+/refs/heads/main/libnativebridge/)
containing `native_bridge.cc`, `native_bridge_lazy.cc`, `include/nativebridge/native_bridge.h`,
`libnativebridge.map.txt`, `Android.bp`, `tests/`, `README.md`. (Older copies:
`system/core/libnativebridge/...` at `android-8.1.0_r1`.) The names in the task brief —
`native_bridge_load_library`, `native_bridge_get_trampoline`, `native_bridge_namespace_create`,
`native_bridge_get_vendor_ns`, `native_bridge_code_cache_callbacks`, `native_bridge_initialize`,
`native_bridge_unlock` — are the **Android 7/8-era** names; they are gone on `main`. Current names
below.

**Contract the bridge implements** — one exported data symbol, `NativeBridgeItf`
(`native_bridge.cc:79`), of type `struct NativeBridgeCallbacks` (`native_bridge.h:190-431`):

| v | field | ART guarantee / RetroBug obligation |
|---|-------|--------------------------------------|
| 1 | `version` | must be ≥ 3 or libnativebridge refuses the bridge (`isCompatibleWith(NAMESPACE_VERSION)`, `native_bridge.cc:266`) |
| 1 | `initialize(runtime_cbs, private_dir, isa)` | `private_dir` is `<app data>/code_cache`, created by libnativebridge (`native_bridge.cc:158,485-509`) — i.e. the bridge may write its JIT/code-cache there |
| 1 | `loadLibrary(path, flag)`, `isSupported(path)` | bridge owns ELF loading for guest libs |
| 1 | `getTrampoline(handle,name,shorty,len)` | **deprecated in v7** (`native_bridge.h:224-227`) |
| 1 | `getAppEnv(isa)` → `NativeBridgeRuntimeValues{os_arch,cpu_abi,cpu_abi2,supported_abis,abi_count}` | used to rewrite `Build.CPU_ABI/CPU_ABI2` + `os.arch` (`native_bridge.cc:70-76,422-479`) |
| 2 | `isCompatibleWith(v)`, `getSignalHandler(signal)` | only SIGSEGV is requested by ART today (`native_bridge.h:261-273`) |
| 3 | `unloadLibrary`, `getError`, `isPathSupported(dirs)`, `createNamespace`, `linkNamespaces`, `loadLibraryExt` | namespace-aware mode; `createNamespace` is explicitly the peer of `android_create_namespace()` (`native_bridge.h:310-346`) |
| 4/5 | `getVendorNamespace` (dead) / `getExportedNamespace(name)` (peer of `android_get_exported_namespace`) | `native_bridge.h:363-380`, `native_bridge.cc:708-724` |
| 6 | `preZygoteFork()` | app-zygote cleanup (`native_bridge.cc:390-399`) |
| 7 | `getTrampolineWithJNICallType(handle,name,shorty,len,jni_call_type)`, `getTrampolineForFunctionPointer(method,shorty,len,jni_call_type)` | `JNICallType ∈ {kJNICallTypeRegular=1, kJNICallTypeCriticalNative=2}` (`native_bridge.h:33-37`) — critical natives are a distinct, `JNIEnv`-free calling shape |
| 8 | `isNativeBridgeFunctionPointer(void*)` | lets ART ask "is this fnPtr a guest address needing a trampoline?" (`native_bridge.h:420-430`) |

**Contract the runtime gives the bridge** — `NativeBridgeRuntimeCallbacks`
(`native_bridge.h:434-465`), implemented in `art/runtime/native_bridge_art_interface.cc:35-99`:
`getMethodShorty(env, jmethodID)`, `getNativeMethodCount(env, jclass)`,
`getNativeMethods(env, clazz, JNINativeMethod*, n)`. The header comment is explicit about *why*:
shorties for calling-convention setup, and the native-method enumeration for `UnregisterNatives`
trampoline teardown (`native_bridge_art_interface.cc:81-92`). **This is the entire JNI bridging
contract ART offers** — everything else (arrays, strings, direct buffers) the bridge must intercept
by owning the `JNIEnv` vtable.

**How ART drives it (call sites, all cited):**
- `Runtime::Init()` → `LoadNativeBridge(runtime_options.ReleaseOrDefault(Opt::NativeBridge))`
  (`art/runtime/runtime.cc:2147-2175`, with the intended-flow comment block at 2147-2171).
- Needs-bridge test is *ISA string equality*: `NeedsNativeBridge(isa) = strncmp(isa, ABI_STRING,...)`
  (`native_bridge.cc:297-303`).
- Zygote fork path: `com_android_internal_os_Zygote.cpp:1921-1929,1974-1980` computes
  `need_pre_initialize_native_bridge = !is_system_server && NativeBridgeAvailable() &&
  !NativeBridgeInitialized() && NeedsNativeBridge(instruction_set)` and then calls
  `PreInitializeNativeBridge(app_data_dir, isa)`.
- `art/runtime/native_bridge_art_interface.cc:101-111`: `PreInitializeNativeBridge` does
  **`unshare(CLONE_NEWNS)`** and then `android::PreInitializeNativeBridge` bind-mounts
  `/system/etc/cpuinfo.<isa>.txt` onto `/proc/cpuinfo` (`native_bridge.cc:305-362`).
- `Runtime::InitNonZygoteOrPostFork(..., NativeBridgeAction, isa)` → `InitializeNativeBridge(env,isa)`
  or `UnloadNativeBridge()` (`runtime.cc:1116-1130,1206-1222`).
- **Signal chain:** `InitializeNativeBridge()` (art side) loops `signal = 0.._NSIG`, asks
  `NativeBridgeGetSignalHandler(signal)`, and registers each returned handler with
  `AddSpecialSignalHandlerFn(signal, &SigchainAction{fn, mask=sigfillset,
  SIGCHAIN_ALLOW_NORETURN})` (`native_bridge_art_interface.cc:117-144`). So: the bridge must *not*
  `sigaction()` itself ("that will potentially lead to cycles", `native_bridge.h:261-265`), it gets
  first-on-chain-after-ART ownership, and it is allowed to not return (longjmp/`RestoreState`).
  `libsigchain`'s location on `main` could **not** be verified this session (probes at
  `frameworks/native/libs/sigchain` and `system/core/libsigchain` 404); `sigchain.h` is still
  included by ART (`runtime.cc:173`, `java_vm_ext.cc:50`, `native_bridge_art_interface.cc:31`).
- **Namespaces:** app class loaders get a linker namespace, and if
  `NativeBridgeIsPathSupported(search_paths)` is true the *whole namespace* is created on the bridge
  side: `is_bridged = NativeBridgeIsPathSupported(search_paths)` →
  `NativeBridgeCreateNamespace(...)` / `NativeBridgeLinkNamespaces(...)` /
  `NativeBridgeLoadLibraryExt(path, RTLD_NOW, ns)`
  (`native_loader_namespace.cpp:91-164,166-183`). So the bridge must emulate namespace semantics,
  `DT_RUNPATH` inheritance and permitted-path rules itself.
- **Symbol → trampoline:** `SharedLibrary::FindSymbolWithNativeBridge()` returns
  `NativeBridgeGetTrampoline2(handle, name, shorty, 0, jni_call_type)` and ART only passes a
  `shorty` when `NeedsNativeBridge()` (`java_vm_ext.cc:154-187,335`).

**Environment surface (what selects the bridge):** `ro.dalvik.vm.native.bridge` and
`ro.product.cpu.abilist32` / `ro.product.cpu.abilist64`, read in
`frameworks/base/core/jni/AndroidRuntime.cpp:1105-1122`. Note the code verbatim:

```cpp
// Native bridge library. "0" means that native bridge is disabled.
//
// Note: bridging is only enabled for the zygote. Other runs of
//       app_process may not have the permissions to mount etc.
property_get("ro.dalvik.vm.native.bridge", propBuf, "");
...
} else if (zygote && strcmp(propBuf, "0") != 0) {
    snprintf(nativeBridgeLibrary, ..., "-XX:NativeBridge=%s", propBuf);
    addOption(nativeBridgeLibrary);
}
```
and `#if defined(__LP64__)` picks `ro.product.cpu.abilist64` else `abilist32` — i.e. on an arm64
device, ART is told `--cpu-abilist=` of the **64-bit** list, and a 32-bit-only APK cannot be
installed at all through normal channels (which is the whole reason RetroBug exists).
**Correction to the brief:** the option is `-XX:NativeBridge=<lib>` on current AOSP, not
`-Xnative-bridge:`.

**Also dead/changed vs the brief:** there is no `dlopen_passthrough` and no
`native_bridge_code_cache_callbacks` on `main`; the code-cache notion survives only as
`<app_data>/code_cache` handed to `initialize()` (`native_bridge.cc:152-158,485-509`). The old
`isSupported(path)`/`loadLibrary` pair survives but is the *non-namespace* (host-test) path
(`native_loader.cpp:476-516`). `frameworks/native/services/nativebd/` **no longer exists** — the
`IBridge`/`nativebd` binder service was removed; `frameworks/native/services/` on `main` contains
only audiomanager…vibratorservice (listing verified). Treat "nativebd/IBridge" as historical.

### 1.4 Can an ordinary app use the native bridge? **No.** Four independent blockers, all cited

1. **Zygote-only opt-in.** `-XX:NativeBridge=` is added only when `app_process` runs with `-zygote`
   (`AndroidRuntime.cpp:1112`). An app process never passes ART runtime options.
2. **Bridge library must be in the system namespace, by simple name.**
   `LoadNativeBridge` → `OpenSystemLibrary(name, RTLD_LAZY)`, which is
   `android_dlopen_ext` into exported namespace `system`/`default`
   (`native_bridge.cc:47-67,260`), and `NativeBridgeNameAcceptable()` rejects any path separator,
   accepting only `[a-zA-Z][a-zA-Z0-9._-]*` — "supposed to be a file in /system/lib or
   /vendor/lib" (`native_bridge.cc:178-208`).
3. **Privileged side effects at pre-init.** `unshare(CLONE_NEWNS)` and `mount(..., MS_BIND,
   "/proc/cpuinfo")` (`native_bridge_art_interface.cc:103-107`, `native_bridge.cc:306-318`) — denied
   to `untrusted_app`.
4. **State machine expects zygote fork semantics** (`kOpened → kPreInitialized → kInitialized`
   driven from `com_android_internal_os_Zygote.cpp`), and `InitializeNativeBridge` is expected to be
   called from `Runtime::DidForkFromZygote` while single-threaded (`native_bridge.cc:233-236,481-483,
   527-529`).

⇒ **Native bridge = requires AOSP/system-image integration** (exactly how Houdini/Berberis ship).
For a normal APK, replace the bridge *entry point* with the `findLibrary()` shadow-lib hook
(§1.2) and reuse the bridge *contract* internally.

### 1.5 dlopen-interception alternatives considered

| Mechanism | Verdict for RetroBug | Evidence |
|---|---|---|
| Native bridge (`NativeBridgeItf`) | System integration only (see 1.4) | cited above |
| `android_dlopen_ext` + caller-supplied namespace | Not interceptable from an app; ART owns the call | `native_loader_namespace.cpp:166-183` |
| `LD_PRELOAD` into RetroBug's own process | Impossible: the process is forked by the platform zygote; env is set by Zygote, and `wrap.<pkg>` properties only apply to `am start`/instrumentation runs via `adb shell` (needs a shell) | bionic's linker does implement LD_PRELOAD (global-group forcing, `linker.cpp:272,1547-1636,1728-1735`), but RetroBug never controls the environment — **inference**, and the `wrap.` property claim is **unverified** this session |
| Overriding `ClassLoader.findLibrary()` | **Chosen.** Public, `protected`, documented, returns the exact path ART dlopens | `ClassLoader.java:2349`, `Runtime.java:1064` |
| DEX instrumentation (rewrite `System.loadLibrary` call sites) | Works but requires a bytecode rewriter at install time and breaks on encrypted/packed DEX; keep as fallback for apps that call `System.load(absolutePath)` directly (`Runtime.java:914,939`) | **inference** |
| PLT/GOT hooking of `libart.so` in-process | Requires writable-text patching of a system lib (SELinux/deny) and is version-fragile. Not needed once `findLibrary` works | **inference** |
| `Runtime.loadLibrary0` via reflection | hidden-API restricted; unnecessary | §6.3 |

**Design precedents (loader integration, not Android):**
- **Rosetta 2 / dyld.** Apple ships a *translator*, not an emulator, and integrates at the loader:
  `dyld` recognises x86_64 images and hands them to Rosetta, which produces arm64 translated
  segments plus an on-disk AOT cache (`/var/db/oah`, `.aot` files). Reverse-engineering writeups:
  [Project Champollion part 2 — Rosetta 2 runtime and AOT shared cache](https://ffri.github.io/ProjectChampollion/part2/),
  [Google Cloud TI: Rosetta 2 artifacts](https://cloud.google.com/blog/topics/threat-intelligence/rosetta2-artifacts-macos-intrusions),
  [Apple support: Rosetta 2 on Apple silicon](https://support.apple.com/en-kg/guide/security-pdf/secebb113be1/web).
  Takeaway for RetroBug: *translation at image-load time, AOT-cached, keyed by build-id* is the
  proven shape; it also implies RetroBug should cache translated blocks under
  `<app data>/code_cache` (exactly what a native bridge is allowed to do,
  `native_bridge.cc:152-158`).
- **Windows ARM64X.** A single PE image contains both ARM64 and ARM64(EC)-emulation-thunk code, and
  the loader binds imports to whichever flavour the *process* is running — i.e. the boundary is at
  import resolution, with typed "EM_THUNKS" describing the signature. Refs:
  [NVIDIA porting guide: ARM64EC vs ARM64 vs ARM64X](https://docs.nvidia.com/rtx-spark/rtx-spark-porting-guide/0.1.0/portingandemulation/arm64ecvsarm64.html),
  [FFRI Chameleon: a new ARM64X relocation in CHPEV2](https://ffri.github.io/ProjectChameleon/new_reloc_chpev2/),
  [Microsoft devblog on ARM64X target selection](https://devblogs.microsoft.com/windows-music-dev/which-compilation-target-should-i-pick-for-my-arm64-music-apps-asio-components-and-plugins/).
  Takeaway: ARM64X's "thunk with signature metadata at the import boundary" is structurally identical
  to ART's `getTrampoline(handle, name, shorty, len)` — RetroBug's shadow libs *are* EM_THUNKS, and
  `shorty` is the signature metadata that makes them cheap to generate.
- **Houdini (Intel/ARM on x86 Android).** Same `NativeBridgeItf` surface, proprietary; the practical
  documentation is how it needed system-image changes (`ro.dalvik.vm.native.bridge=libhoudini.so`),
  i.e. confirms 1.4. Field guides:
  [redroid native-bridge issue](https://github.com/remote-android/redroid-doc/issues/175),
  [Adding an ARM native bridge to the AOSP11 x86 emulator](https://chgans.design.blog/2021-05-23/adding-arm-native-bridge-to-the-aosp11-x86-emulator/),
  [Actinis Chimera](https://actinis.io/chimera) (a current commercial native bridge; its own page
  describes running *arm64* guests on x86_64 via "interpreter and translation, not emulation").
  **Unverified:** whether any shipping product does ARM32-on-ARM64 as a *native bridge*; Android
  arm64 devices normally keep 32-bit EL0 (`CONFIG_COMPAT`) so there was never a need (§5.5).

---

## 2. JNI bridging — where pointers cross, and what must be marshalled vs aliased

Everything below is a consequence of one asymmetry: **host JNI handles are 64-bit and refer to
host-ART objects; guest code believes pointers are 32-bit.** ART's own contract assumes the bridge
owns the whole `JNIEnv` vtable (it hands the bridge `shorty`s and a `JavaVM*`, and expects the bridge
to intercept `RegisterNatives`/`UnregisterNatives`).

### 2.1 The `JNIEnv` function table

`JNIEnv*` is a pointer to a table of ~60+ function pointers (`struct JNINativeInterface_`). The
guest will `LDR r3,[r0,#offset]` and `BLX r3`. Two viable designs:

- **(A) Guest-visible vtable (recommended).** Build a *32-bit* `JNINativeInterface_` image whose
  entries are 32-bit addresses of RetroBug thunks. Each thunk (per entry index + per shorty) packs
  the guest args, calls the real host `env->X(...)`, and unpacks. This is what Houdini/Berberis do;
  AOSP even auto-generates the equivalent: `native_bridge_support/android_api/libnativehelper`
  (`proxy/` + `stubs_arm.cc`/`stubs_arm64.cc`/`stubs_riscv64.cc`) plus
  `tools/gen_known_trampolines.py`
  ([tree](https://android.googlesource.com/platform/frameworks/libs/native_bridge_support/+/refs/heads/main/android_api/libnativehelper/)).
  RetroBug must special-case ~15 entries that are *variadic* or take/return buffers (§2.3–2.4); the
  remaining ~50 are mechanical "shrink pointers to 32 bits" wrappers.
- (B) Pass the host `JNIEnv*` and translate each call — impossible without patching ART.

`GetVersion()` must report a version the guest can accept and `JNI_OnLoad` must return one of
`JNI_VERSION_1_2/1_4/1_6` or ART rejects the library (`java_vm_ext.cc:67-70,1144-1148`). **Note:**
guest libs compiled against newer NDK headers may request `JNI_VERSION_1_6` (0x00010006) which is
what nearly all apps use; RetroBug's proxy should clamp, and should *not* advertise a version the
host ART cannot service.

### 2.2 Pointer-crossing inventory (the actual work list)

| Boundary | Direction | Treatment |
|---|---|---|
| `jobject`/`jclass`/`jstring`/`jbyteArray`/`jobjectArray` | both | **Marshal via 32-bit handle table.** On host these are ART indirect-reference cookies; the guest must never see their true value. Keep a per-process (guest,)→(host) map with a freelist and a generation counter to catch use-after-free. ART may hand out *indexed* (not pointer-shaped) ids: see `-Xopaque-jni-ids` in `AndroidRuntime.cpp:717` and `Runtime::SetJniIdType` (`runtime.cc:3349`) — so a table is mandatory, not optional. |
| `jmethodID`/`jfieldID` | both | Same: opaque 32-bit ids; ART internals literally treat them as decodable ids (`jni::DecodeArtMethod(mid)`, `native_bridge_art_interface.cc:37`). Never truncate a host `ArtMethod*`. |
| `jshorty` / signature / name `const char*` | host→guest (e.g. `GetSourceFile`) | Copy into a guest-readible buffer owned by RetroBug's **string pool** so the pointer is stable across calls; JNI says these are released by `ReleaseStringUTFChars`, but apps leak them — leak-proof pool with refcount + "never freed before dlclose" policy. |
| `GetStringUTFChars` / `GetStringChars` / `GetStringCritical` | host→guest | Escape hatch: returns a **32-bit pointer to modified-UTF-16/8 bytes**. Must live in the guest-visible address space (§2.5). `GetStringUTFLength`, `GetStringLength` must agree. |
| `GetPrimitiveArrayCritical` / `ReleasePrimitiveArrayCritical` | both | Returns a direct pointer to *host heap array data*. Two options: (i) copy-in/copy-out with pin-count (safe, slow, breaks "in-place" `jshort[]` views); (ii) **alias**: only possible if ART array data is already inside the guest's 32-bit window (§2.5). Since ART object headers differ in size between 32/64-bit (`4 vs 8 byte` header + `is_object` layout changes), aliasing a live host array as a guest pointer is only sound if the guest treats it as an opaque byte region *and* RetroBug never hands out host object headers. Recommendation: **copy, with a fast path for large arrays using a borrowed region.** Flag: perf-critical for games (textures, audio). |
| `GetDirectBufferAddress` / `NewDirectByteBuffer` | both | Same problem, longer-lived. Recommend a direct-buffer registry: guest `void*` → host `jobject`, so `NewDirectByteBuffer(env, guestPtr, cap)` allocates a host buffer and records the mapping. `AHardwareBuffer`/`ANativeWindow` paths in `libandroid` proxy need identical treatment. |
| `JavaVM*` + `AttachCurrentThread(void** env, void* args)` | guest→host | Guest passes a `JavaVMInitArgs*` (32-bit layout, `JNI_VERSION`, `nOptions`, `options[]`). Must unpack, then return a 32-bit `JNIEnv*` from §2.1. `GetEnv` for a not-yet-attached host thread must return `JNI_EDETAIL`/`NULL` exactly as ART does. |
| `Call<Type>Method(...)`, `...V`, `...A` | guest→host | Three shapes to implement each: **varargs** (needs the shorty → `getMethodShorty`, `native_bridge.h:435-442`), **va_list** (needs guest `va_list` reader, §3.4), **jvalue\*** (needs element-wise marshal: `jvalue` is 8 bytes on both, but `jobject`/`int` fields inside it are 32-bit for the guest). |
| `RegisterNatives` / `UnregisterNatives` | guest→host | The single most important interception. On `RegisterNatives(clazz, JNINativeMethod* methods, n)` the guest's `fnPtr` values are **32-bit guest code addresses**; RetroBug creates an arm64 stub per (fnPtr, shorty) and calls the real `RegisterNatives` with host stubs. `UnregisterNatives` must enumerate via ART's `getNativeMethods` callback (that's literally its documented purpose, `native_bridge.h:447-464`). |
| `Throw` / `ThrowNew` / `ExceptionCheck` | both | Only handles + enum values; cheap. |
| `FindClass` failure + `NoClassDefFoundError` semantics | — | The class loader used by `FindClass` inside `JNI_OnLoad` is temporarily overridden by ART (`java_vm_ext.cc:1128-1130`), so RetroBug's shadow lib must let the *guest's* `FindClass` see the **guest app's** loader, not RetroBug's. ART does the right thing automatically if the guest `JNI_OnLoad` runs on the same thread/stack. |
| `@CriticalNative` / `@FastNative` | host→guest | `JNICallType` v7 exists precisely because critical natives receive **no `JNIEnv`** and must not touch it (`native_bridge.h:33-37,386-418`); ART will hand the bridge a distinct call type. RetroBug must generate a *narrower* stub for them (primitive args only, no state transition) — otherwise you get non-recoverable runtime aborts. |
| `GetJavaVM`, `MonitorEnter/Exit`, `ExceptionDescribe`, `FatalError` | both | trivial (handle or host-only). |

### 2.3 The "guest pointers inside host structs" problem

Concrete host structs whose *fields* carry pointers the guest owns or vice versa:
`JNINativeMethod{char* name; char* signature; void* fnPtr}`,
`JavaVMInitArgs{int version; int nOptions; int ignoreUnrecognized; JavaVMOption* options}` ,
`JavaVMOption{const char* optionString; void* value}`, `jvalue` union,
`JNICritical` return values, and — critically — **`pthread_internal_t`**. AOSP documents the exact
class of hazard in bionic: the `bionic_tcb`/`pthread_internal_t` split exists because
*"native bridge requires pthread_internal_t to have the same layout across architectures"*
(`bionic/libc/private/bionic_tls.h:51-57`). Implication: any struct that crosses the boundary must be
either (i) arch-independently laid out (same widths — impossible when `off_t`/pointer sizes differ),
or (ii) fully marshalled by the bridge. RetroBug must therefore **never** let a host `pthread_t` /
`pthread_internal_t` / `FILE*` / `DIR*` / `jmethodID` become a guest-visible 32-bit value by
truncation: it needs a **shadow-object table for every opaque host type** it exposes through a proxy
library (`pthread`, `stdio`, `dlfcn`, `sqlite`, `zlib`, `EGL*`/`GLsync`, `AAssetManager*`, ...).
Berberis already does this (`android_api/libc/proxy/pthread_translation.cc`,
`libc_translation.cc`, `malloc_translation.cc`, `setjmp_thunks.cc`, `unistd_thunks.cc`,
`system_properties_trampolines.cc`, `cxa_trampolines.cc` — file list from
`native_bridge_support/android_api/libc/Android.bp:149-160`). `setjmp`/`longjmp` and
`__cxa_thread_atexit_impl` are in the list for exactly this reason — `jmp_buf` embeds host register
state.

### 2.4 Variadics

Two distinct problems:
- **Guest function called with varargs by the host** (rare, but `JNI_OnLoad`-registered callbacks and
  `native` methods are never variadic — JNI forbids it). Mostly N/A.
- **Guest calling a host-provided variadic proxy**: `printf`/`snprintf`, `__android_log_print(prio,
  tag, fmt, ...)`, `eglGetProcAddress`-style, `dlopen`-family `RTLD_*` (not variadic), and
  `FenceTime`/`ALOGE` macros. RetroBug's proxy must read a **guest AAPCS32 `va_list`**: on
  AArch32, `va_list` is a small struct `{ap, vn, vma, grtop, rtop}` in memory (GCC/Clang
  `__builtin_va_list` for AAPCS, 4×4-byte fields when `-mfloat-abi=softfp`), and the register
  save area is only populated *if* the callee might pass FP args, which the ABI documents in
  Appendix "Variadic Function Subroutine Calls" of the AAPCS
  ([aapcs32.rst](https://raw.githubusercontent.com/ARM-software/abi-aa/main/aapcs32/aapcs32.rst) §6.8.
  **Verify the exact struct layout against the clang `AArch64/ARM` `va_arg` implementation before
  implementing** — getting it wrong corrupts format strings silently.) Practical alternative,
  strongly recommended: **implement the variadic proxies in guest code** — ship a small *arm32*
  shim `.so` linked into the guest namespace that converts `(fmt, va_list)` into an explicit
  array of marshalled args (using guest-side `va_arg`), then calls a *non*-variadic host entry.
  That is what `libc/proxy/unistd_thunks.cc` + `libc_translation.cc` effectively do for `open()`.

### 2.5 The guest-visible address space (mandatory)

Because §2.2's `char*`/array/direct-buffer rows cannot be solved by handles, RetroBug must expose a
**real 32-bit address space** that is also usable by the host:

- Allocate all guest data (stacks, heaps, mapped guest ELF segments, string/byte pools) with host
  `mmap` **below 2³²** (arm64 Linux's `TASK_SIZE` gives the whole low 32 bits to userspace; use
  `MAP_FIXED_NOREPLACE` hints or iterative low `mmap` probing; Android forbids page 0, which matches
  ARM Linux's `mmap_min_addr`). **Inference, must be measured:** on a 16 KB-page host, low-4 GB
  allocation is still possible but fragmentation from the zygote's already-mapped low regions
  (`dalvik-cache`, boot.art is usually high) needs checking.
- Guest pointer = host pointer **truncated**, only valid for that window. Everything outside
  (host objects, ART heap, system libs) must be handle-mapped. Add an assertion mode that rejects
  any 32-bit value whose high host bits are non-zero-and-not-window.
- Beware arm64 **top-byte-ignore / MTE tagged addresses**: the kernel explicitly allows tagged
  pointers in syscalls only after `prctl(PR_SET_TAGGED_ADDR_ABI)`
  (`arch/arm64/Kconfig:1800-1807`, `Documentation/arch/arm64/tagged-address-abi.rst`). RetroBug must
  **untag** (`PTR_TAG_MASK` clear) any pointer before truncating into a guest slot, and must not put
  guest pointers into tagged host slots. Also note **PAC**: some host returns are signed pointers;
  strip/avoid storing them in guest-visible tables.

### 2.6 Marshalling cost, and how the native bridge design contains it

Per-call cost is dominated by (a) stub dispatch, (b) handle-table lookup, (c) string/array copies.
Mitigations already sanctioned by ART: `shorty`-keyed stub cache (`getTrampoline*(..., shorty, len)`),
`kJNICallTypeCriticalNative` fast path, and *not* wrapping host→guest calls that are pure primitive.
**Recommendation:** measure with `berberis_jni_tests` (`binary_translation/tests/jni_tests`,
README referenced from `binary_translation/README.md:109-125`) as an initial benchmark harness; it is
a real JNI test APK exercised under a native bridge.

---

## 3. ARM EABI (AAPCS32 / `armeabi-v7a`) vs AArch64 AAPCS — full delta RetroBug must honour

Primary sources: [AAPCS32 (`Procedure Call Standard for the Arm Architecture`, ARM IHI 0042 lineage)](https://raw.githubusercontent.com/ARM-software/abi-aa/main/aapcs32/aapcs32.rst),
[AAPCS64 (IHI 0059)](https://raw.githubusercontent.com/ARM-software/abi-aa/main/aapcs64/aapcs64.rst),
[Arm EABI32 / "Protocol for Object Code and ABI" (IHI 0044)](https://raw.githubusercontent.com/ARM-software/abi-aa/main/ehabi32/ehabi32.rst),
plus AOSP build flags (`build/soong/cc/config/arm_device.go`).

### 3.1 `armeabi-v7a` uses the **base** variant with hardware FP (softfp) — confirmed

`build/soong/cc/config/arm_device.go:59-79` — every arm arch variant Android builds is compiled with

```
"-march=armv7-a", "-mfloat-abi=softfp", "-mfpu=vfpv3-d16"   // and -mfpu=neon for armv7-a-neon
"-march=armv8-a", "-mfloat-abi=softfp", "-mfpu=neon-fp-armv8"
```
and NDK triple is `arm-linux-androideabi` / target `armv7a-linux-androideabi<api>`
([`platform/ndk` BuildSystemMaintainers.md:57,217](https://android.googlesource.com/platform/ndk/+/refs/heads/master/docs/BuildSystemMaintainers.md)).

**Implications (this is the good news):**
- FP/neon *instructions* are used (VFPv3-D16 or NEON), but **arguments and return values are passed
  per the base variant**: floats/doubles go in `r0-r3`/stack as `s`- and `d`-sized pieces, never in
  `d0-d7`. The VFP-register-argument variant is a *different, incompatible* PCS
  ("Code compiled for the VFP calling standard is compatible with the base standard … if no
  floating-point or containerized vector arguments or results are used", `aapcs32.rst:1694-1700`).
- Therefore RetroBug's trampoline generator needs **no VFP/NEON argument allocation** for guest⇄host
  calls: `float`→ one 32-bit GPR slot (as `S` register low bits), `double`→ two GPR slots with
  8-byte NCRN rounding (Rules C.3/C.7, `aapcs32.rst:1391-1419`). Only *data movement* between
  `rN`⇄`sN/dN` is needed in each stub (fmrs/vmov), plus vector types handled by value-in-memory.
- Composite args: alignment-adjusted copies (Rule B.5: 4-byte alignment if natural ≤ 4, 8-byte if
  ≥ 8; `aapcs32.rst:1357-1369`), so struct passing is *by-value-in-registers-or-stack* with
  padding rules distinct from AArch64's (AArch64 uses a completely different composite-arg
  classification: `AAPCS64` §6.3 with HVA/HFA classes — cite `aapcs64.rst`).
- Callee-saved / scratch register sets differ: guest uses `r0-r3` args, `r4-r11` mostly callee-saved
  (Thumb-1 restricts to `r4-r7`), `r12=IP`, `r14=LR`, `r15=PC`; VFP: `d8-d15 (s16-s31)` callee-saved,
  `d0-d7` scratch (`aapcs32.rst:959-963`). Host `x0-x7` args, `x9-x15` scratch, `x19-x28` saved,
  `x8`=indirect result, `x16/x17`=IP0/IP1, `x18`=shadow-call-stack/platform-reserved on Android,
  `v8-v15` (low 64 bits) callee-saved. RetroBug's translator must reserve a host register for the
  guest `ThreadState*` and honour AArch64's `x18` reservation (Android uses `--fixed-x18`? **verify**;
  bionic/clang on Android aarch64 does reserve x18 for the shadow call stack in some configs —
  **unverified**).

### 3.2 Stack alignment

- AArch32: `SP mod 8 == 0` at a public interface (`aapcs32.rst:1079-1085`); the *ARM Linux* runtime
  also requires 8-byte alignment inside functions generally (many routines assume it for `LDRD`).
- AArch64: `SP mod 16 == 0` **always** (AAPCS64 §4.1.1: "SP must be quad-word aligned"; the frame
  record pushes pairs). A guest frame whose SP is 8-but-not-16 aligned is normal AArch32 but illegal
  as a host frame ⇒ RetroBug's guest stacks must be 16-byte aligned at the *host* boundary and the
  emulator must tolerate guest `sp` being ≡ 8 (mod 16) *within* the guest region without letting a
  host helper see it.
- `d8-d15`/`x` save areas: guest code using `POP {…}` on an unaligned SP raises SIGBUS on real ARM;
  RetroBug must reproduce that fault precisely (some apps probe alignment!).

### 3.3 `this` / vptr / return of structs

- C++ `this` = first argument (`r0` / `x0`) — trivially 1:1, but `jclass`/`JNIEnv*` are also first
  args, so all JNI thunks share the same shape (convenient).
- Non-trivial class returns: AArch32 AAPCS passes a hidden result pointer in `r0` and returns the
  pointer in `r0` (base variant), while AArch64 uses `x8` for the return buffer for large non-trivial
  types. **Any C++ type returned *by value* across the boundary must be marshalled via an explicit
  temporary buffer** (this is the main trap in proxy `libandroid_runtime`/`libutils` calls).
  Practical rule for RetroBug: only ever expose **POD/by-pointer** signatures across the boundary;
  wrap C++ returns in out-params inside the proxy code (that is why Berberis hand-writes
  `libc_translation.cc` etc. rather than auto-generating everything).
- Virtual dispatch is entirely guest-side (`vptr` in guest objects, guest code reads it) — safe, as
  long as guest object layouts come from *guest* builds of the same sources (`libnative_bridge_guest_*`).

### 3.4 TLS: `r9`/`tp`, `MRC p15,0,rN,c13,c0,3`, and `__ARM_NR_set_tls`

- The PCS leaves `r9` platform-defined: "it may designate it as the static base (SB) … or … as the
  thread register (TR) in an environment with thread-local storage. … A virtual platform that has no
  need for such a special register may designate r9 as … v6" (`aapcs32.rst:866-881`). ARM Linux /
  Android uses `r9` as the **thread pointer (`tp`)**, i.e. guest code compiled `-mtls=arm-tpctr`
  reads it with `MRC p15,0,rN,c13,c0,3` (TPIDRURW) or directly as `r9`.
- Consequences for the translator:
  1. `tp` must be part of `GuestState` (a plain 32-bit field), not a host register, and must be
     restored on every context switch — including for guest threads created via `clone()`.
  2. `MRC p15,0,Rt,c13,c0,3` and the related `MCR` (rare from EL0) must decode as "read/write
     `GuestState.tp`". Other `MRC/MCR p15` (cache/SIDR/etc.) should SIGILL per ARMv7 EL0 rules.
     **Verify** that bionic's `__get_thread()` uses TPIDRURW and *not* the `kuser_get_tls` page for
     the Android arm build (there is a `__kuser_get_tls` at `0xffff0fe0`; modern bionic uses the
     register — check `bionic/libc/arch-arm/bionic/`; **unverified** this session).
  3. Guest `__aeabi_read_tp()` must map to the same thing.
  4. Guest TLS *block* layout comes from the guest linker's `R_ARM_TLS_DTPMOD32/DTPOFF32/TPOFF32`
     and `R_ARM_TLS_DESC` relocations (`bionic/linker/linker_relocs.h:51-61`) plus
     `native_bridge_support/android_api/libc/init_static_tls_arm.S` and `linker_tls.cpp`
     (same repo, `android_api/linker/` listing) — i.e. **AOSP already has the arm32 static-TLS init
     recipe for a native-bridge guest**; reuse it.

### 3.5 Leaf frames / unwinding / exceptions

- AArch32 Android uses `.ARM.exidx` + `.ARM.extab` (0x4/0x28-byte tables) with the **ARM Exception
  Handling ABI (ARM IHI 0038 / PRARM)**, not DWARF CFI `.eh_frame`. Unwind tables must be parsed by
  RetroBug's *guest* unwinder (for `__cxa_throw`, `backtrace()`, and crash dumping), and the
  translator must be able to synthesise a guest unwind when a host signal lands mid-guest-frame.
- Live evidence of the rough edges: AOSP keeps `.debug_frame` for arm32 native-bridge libc because
  *"arm32 does not produce complete exidx unwind information, so keep the .debug_frame … See
  b/132992102"* (`native_bridge_support/android_api/libc/Android.bp:88-94`). Expect the same gap;
  budget for a `.debug_frame` fallback in the guest unwinder.
- `__cxa_*` personality/terminate must come from the **guest** build of `libc++`/`libunwind`
  (note `whole_static_libs: ["libunwind-exported"]` in that same Android.bp).
- RetroBug must make its own **host-side** stack unwinding survive translated frames: emit a
  `.eh_frame`-equivalent / frame-map for translated code, and hook `debuggerd`/tombstones. Berberis
  solves it by writing its own tombstone extras: "contains stack traces for all the host threads and
  guest threads" (`binary_translation/README.md:133-147`).

### 3.6 PLT/GOT and relocations

- Relocation names differ; bionic normalises them with `R_GENERIC_*` macros which map to
  `R_ARM_JUMP_SLOT / R_ARM_ABS32 / R_ARM_GLOB_DAT / R_ARM_RELATIVE / R_ARM_IRELATIVE / R_ARM_COPY /
  R_ARM_TLS_*` on arm and `R_AARCH64_*` on arm64 (`bionic/linker/linker_relocs.h:37-61`).
  RetroBug's 32-bit loader must handle, at minimum: `R_ARM_ABS32`, `R_ARM_GLOB_DAT`,
  `R_ARM_JUMP_SLOT`, `R_ARM_RELATIVE`, `R_ARM_COPY`, `R_ARM_IRELATIVE`, `R_ARM_TLS_*`, plus the
  Thumb/adrp-family ones that appear in real binaries (`R_ARM_CALL`, `R_ARM_THM_CALL`,
  `R_ARM_THM_JUMP24`, `R_ARM_TARGET1`, `R_ARM_MOVW_ABS_NC`, `R_ARM_MOVT_ABS`, `R_ARM_THM_MOVW_ABS_NC`,
  `R_ARM_THM_MOVT_ABS`, `R_ARM_PREL31`?). **Partially unverified**: current `linker_relocate.cpp`
  handles almost all through the generic path (only a comment about
  [`R_ARM_GLOB_DAT` addend handling vs IHI 0044F](https://android.googlesource.com/platform/bionic/+/refs/heads/main/linker/linker_relocate.cpp)
  at line ~330: *"the document (IHI0044F) specifies that R_ARM_GLOB_DAT has an addend, but Bionic
  isn't adding"*), so RetroBug must reproduce that **same non-conformity** or guest libs linked with
  Android's tools will double-count addends.
- `R_ARM_THM_CALL`/`BLX` target-bit semantics: bit0 of the symbol value = Thumb state. RetroBug's
  entry-point convention must encode guest function addresses with the Thumb bit exactly like real
  hardware (this is the classic ARM-emulator bug source: JNI `fnPtr` for a Thumb function has
  `|1`).
- Lazy PLT: `RTLD_LAZY` is honoured by ART when opening the *bridge* library
  (`OpenSystemLibrary(..., RTLD_LAZY)`, `native_bridge.cc:260`) but app libs are opened with
  `RTLD_NOW` (`java_vm_ext.cc:1076` comment; `native_loader_namespace.cpp:171`). Guest-side, RetroBug
  must implement lazy binding anyway because guest `.so`s depend on each other with `RTLD_LAZY`
  defaults from their own `dlopen`s.
- `--hash-style=gnu`: bionic's linker supports GNU hash (`bionic/linker/linker_gnu_hash.h`, with
  tests), and all modern Android 32-bit `.so` use it. RetroBug's loader must implement both
  `.gnu.hash` (bloom filter) and `.hash`; skipping `.hash` will break pre-API-21-era libs.

### 3.7 Instruction-set surface RetroBug's decoder must cover (A32+T32)

Non-negotiable minimum for real apps: full ARMv7-A A32/T32 integer + multiply/divide(`UDIV`/`SDIV`
require `IDIVA/IDIVT` hwcap, see §5.3), VFPv3-D16 + NEON (all 32 and 64-bit element forms),
`VFP`/`NEON` load-store with writeback, `LDREX/STREX`+`LDREXB/...`/`STREXB` and `LDREXD/STREXD`
(**must** be implemented with host exclusive-monitor semantics, not by locking — otherwise guest
spinlocks deadlock/livelock), barriers `DMB/DSB/ISB` (map to host `dmb ish` etc.), `CLZ/RBIT/REV`,
`MRC/MCR p15` (TLS only, §3.4), `BKPT` (→ `__ARM_NR_breakpoint` / SIGTRAP, §5.2), `SVC`, and
`SETEND`/`CPS`/`MRS/MSR` (privileged forms must be illegal at EL0 emulation level).
**Also required by real-world binaries:** ARMv8-A *AArch32* extensions — `CRC32`, `SHA1/SHA256`
(`HWCAP2_CRC32/AES/SHA1/SHA2`, `arch/arm/include/uapi/asm/hwcap.h:41-47`), `VFPv4` fused multiply,
`SIMD` `VCVT` half/bf16 (`HWCAP_ASIMDFHM/ASIMDBF16`), `HVC`/`SMC` (illegal), and the "deprecated
ARMv8" encodings the host kernel may itself emulate
(`arch/arm64/Kconfig:1880` `ARMV8_DEPRECATED`, `COMPAT_ALIGNMENT_FIXUPS` at 1877).
Deliberately *unimplemented-in-HW* cases must raise the same SIGILL the device would.

---

## 4. ELF format + linker differences: reuse AOSP's 32-bit linker, or write our own?

### 4.1 What bionic's linker actually does that must be reproduced

- **Namespaces** (`bionic/linker/linker_namespaces.cpp`, 5.4 KB, plus `linker_soinfo.cpp`):
  per-class-loader namespace, `ANDROID_NAMESPACE_TYPE_ISOLATED/SHARED/PERMISSIVE/EXEMPT_LIST_ENABLED/
  GLOBAL_VISIBLE/GLOBAL_NAMESPACE` flags, `ld_library_path` → `DT_RUNPATH` of the *needed-by* →
  `default_library_path` → parent link order (documented verbatim in
  `native_bridge.h:310-330`: "The libraries in the namespace are searched by following order: 1.
  ld_library_path … 2. In directories specified by DT_RUNPATH of the 'needed by' binary. 3.
  default_library_path").
- **Path rules for apps**: `LD_LIBRARY_PATH` from the environment is only honoured for
  non-app/legacy cases; the class-loader's paths arrive via
  `android_update_LD_LIBRARY_PATH()` (called by ART's `SetLdLibraryPath`,
  [java_lang_Runtime.cc at `906846f`:36-68](https://android.googlesource.com/platform/art/+/906846f/runtime/native/java_lang_Runtime.cc))
  and via `android_dlextinfo`/namespace. `do_android_get_LD_LIBRARY_PATH` /
  `do_android_update_LD_LIBRARY_PATH` in `bionic/linker/linker.cpp:2060-2089`.
- **targetSdkVersion compatibility hacks that must be copied exactly**:
  `get_application_target_sdk_version() >= 24` ⇒ no exempt-list (`linker.cpp:244-247`);
  `< 23` ⇒ `dlsym(RTLD_DEFAULT)` sees `RTLD_LOCAL` libs (`linker.cpp:800-805`, b/21565766);
  `< 23` ⇒ basename-ify malformed `DT_NEEDED` (`linker.cpp:1085-1090`, b/21364029).
  RetroBug's loader **must** take the *guest APK's* targetSdk here, not the device's.
- `DT_RUNPATH`/`DT_RPATH` handling, `$ORIGIN` expansion (`linker_utils.cpp`), `DF_1_GLOBAL`/
  global-group construction (`linker.cpp:1616-1735`), `RTLD_GLOBAL` vs per-namespace groups,
  `dlopen(NULL)` semantics, `android_get_exported_namespace("default"/"system"/"sphal")`,
  `linker_translate_path.cpp` (`/system` vs `/system_ext`/`/product` translation),
  16 KB compat (`linker_phdr_16kib_compat.cpp`), `.note.gnu.property` (BTI/PAC/MTE,
  `linker_note_gnu_property.cpp`).

### 4.2 Evidence from AOSP: the native bridge *reuses the real guest linker*

`frameworks/libs/native_bridge_support/native_bridge_support.mk:33-36` — the mandatory trio is:

```
NATIVE_BRIDGE_PRODUCT_PACKAGES := \
    libnative_bridge_vdso.native_bridge \
    native_bridge_guest_app_process.native_bridge \
    native_bridge_guest_linker.native_bridge
```

and Berberis' distribution list contains `system/bin/riscv64/linker64`,
`system/lib64/riscv64/ld-android.so`, and `system/etc/ld.config.riscv64.txt`
(`binary_translation/berberis_config.mk:60,87,64`). So: the *guest-arch* dynamic linker is built from
bionic and **runs under translation**, with a per-guest-arch `ld.config.<arch>.txt` giving it
namespace configuration; `android_api/libc` even overrides `libc` with
`libnative_bridge_guest_libc` (`stem: "libc"`, `overrides: ["libc"]`,
`android_api/libc/Android.bp:27-37`).

### 4.3 Recommendation (decisive)

**Do both, in this order, and prefer (A):**

**(A) Run the real AOSP 32-bit `linker` (`/system/bin/linker`, built for arm) as translated guest
code**, exactly as the native bridge does, driven by a host-side "proxy loader" that talks to it via
the guest ABI. Pros: namespace/`DT_RUNPATH`/`RTLD_*`/reloc/targetSdk semantics are *identical* to the
behaviour apps were tested against, for free, including all the legacy hacks; you inherit upstream
fixes; zero reimplementation risk on the ~3000 lines of gnarly search-order logic. Cons: you must
translate ~300 KB of guest code (bootstrap cost ≈ a few ms once per process), you must implement the
guest→host **syscall** surface the linker itself uses (`openat`, `readlinkat`, `faccessat`,
`mmap`/`mprotect`, `prctl`, `getauxval`, `ioctl(TIOCG...?)`, `readlink("/proc/self/maps")`,
`fstatfs`) — which RetroBug needs anyway for guest libc — and you must supply a
`ld.config.arm.txt`-equivalent.

**(B) Fallback: RetroBug's own minimal 32-bit loader** — only for the *first bring-up milestone*
(load guest `libc.so` + the app's `.so` with a flat, single-namespace, `RTLD_NOW`-only search), and
only if (A)'s syscall-surface bootstrapping proves harder than expected. Expect 3–8 weeks of
compat bug-f reports for `DT_RUNPATH`-relative deps, `RTLD_GLOBAL` symbol interposition (games love
it), duplicate-SONAME/dlopen-twice handling, and TLS desc relocs.

**Why (A) wins:** RetroBug's hard part is never "parse a 32-bit ELF"; it's *matching Android linker
behaviour across 15 years of apps*. AOSP's linker is the oracle, is Apache-2.0/BSD (see
`bionic/linker/MODULE_LICENSE_BSD`), and is already integrated into the native-bridge build graph
for arm guests (`android_api/linker/linker.arm.map`, `init_static_tls_arm.S`, `linker_tls.cpp`,
`linker_translate_path.cpp`).

**(C) Do not** try to use the host linker for guest objects (it rejects `EM_ARM`), and do not
`LD_PRELOAD` anything (§1.5).

---

## 5. Syscall ABI delta: ARM EABI (AArch32) vs AArch64

### 5.1 Correcting the premise

The brief says "AArch64 Linux has NO `CONFIG_EABI` compat for ARM32 in an arm64 kernel". Precisely:

- There is no `CONFIG_EABI`. There **is** `CONFIG_COMPAT` = *"Kernel support for 32-bit EL0"*
  ([`arch/arm64/Kconfig:1809-1823`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/arch/arm64/Kconfig)):
  *"This option enables support for a 32-bit EL0 running under a 64-bit kernel at EL1.
  AArch32-specific components such as system calls, the user helper functions, VFP support and the
  ptrace interface are handled appropriately by the kernel."* Note also the 16 KB page caveat at
  line 1821-1823 and `CONFIG_COMPAT_VDSO` (1856).
- **But that is for *AArch32 processes*.** An *arm64* process (RetroBug's host process) cannot issue
  AArch32 numbers; SVC #0 dispatches on the task's thread size. ⇒ RetroBug must do the translation
  itself, in user space. Confirmed by the two disjoint tables below.

### 5.2 Register/ABI shape

| | AArch32 (ARM EABI) | AArch64 |
|---|---|---|
| syscall number | `r7`, `SVC #0` | `x8`, `SVC #0` |
| args | `r0-r5` (= results `r0-r1`) | `x0-x5` (result `x0`) |
| base | `__NR_SYSCALL_BASE 0` for EABI/Thumb; `__NR_OABI_SYSCALL_BASE 0x900000` for OABI; `__NR_SYSCALL_MASK 0x0fffff` | generic `asm-generic/unistd.h` |
| arch-private | `__ARM_NR_BASE = 0xf0000` block | none |
| multiplexed legacy | `ARM_eabi_syscall_*` in table (e.g. `fadvise64_64` with reorder), `arm_sync_file_range` = `sync_file_range2` | direct |

Evidence: [`arch/arm/include/uapi/asm/unistd.h:17-41`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/arch/arm/include/uapi/asm/unistd.h):
`__ARM_NR_breakpoint +1`, **`__ARM_NR_cacheflush +2` (= 0xf0002)**, `__ARM_NR_usr26 +3`,
`__ARM_NR_usr32 +4`, **`__ARM_NR_set_tls +5`**, **`__ARM_NR_get_tls +6`**;
plus `#define __NR_sync_file_range2 __NR_arm_sync_file_range` (line 28).

**`cacheflush` — must we emulate it? YES, non-optional.**
`sys_cacheflush(start,end,flags)` is the only AArch32 way to (a) clean the D-cache / invalidate the
I-cache for a range and (b) is used by *every* JIT, by `ARM` `__builtin___clear_cache()` lowerings, by
some DSP libs, and by self-modifying plugin loaders. RetroBug must implement it as:
flush host-side translated blocks whose *guest* PC range intersects `[start,end)` (drop from the
code cache and mark for re-translation), **and** issue `__builtin___clear_cache`/`IC IVAU`+`DSB`+`ISB`
on the host for the same range in case the guest wrote host-aliased code (see §2.5's low window).
Semantics to match: returns 0; `EINVAL` on unmapped/OOB ranges; it's also a *coherency point*, so a
guest thread that patched code must not let another guest thread execute stale translations. This
pairs with `prctl(PR_SET_MDWE)`/W^X concerns for the code cache.
`__ARM_NR_breakpoint` → deliver SIGTRAP with `si_code == TRAP_BRKPT` and `pc` at the `BKPT`.
`__ARM_NR_set_tls/get_tls` → read/write `GuestState.tp` (§3.4).
`__ARM_NR_usr26/usr32` → `EINVAL` (never supported on Android).

### 5.3 Number deltas (measured, same kernel tree, torvalds/linux @ main)

`__NR` (ARM from [`arch/arm/tools/syscall.tbl`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/arch/arm/tools/syscall.tbl)) vs
`__NR` (AArch64 from [`include/uapi/asm-generic/unistd.h`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/include/uapi/asm-generic/unistd.h)):

| syscall | AArch32 | AArch64 | note for RetroBug |
|---|---|---|---|
| `fork` | 2 | **absent** | must be synthesised as `clone(SIGCHLD,...)`; apps can't fork anyway (§5.6) |
| `vfork` | 190 | **absent** | `clone(CLONE_VFORK|CLONE_VM)`; `execve` after it will fail on SELinux (§5.6) |
| `execve` | 11 | 221 | denied to app domain |
| `brk` | 45 | 214 | **the brief's claim "brk absent on AArch64" is wrong** — both exist; bionic/Scudo uses `mmap`, so it's low-risk, but guest `sbrk()` must still be answered from RetroBug's own heap model, never forwarded |
| `gettimeofday` | 78 | 169 | 32-bit `timeval` layout vs `__kernel_old_timeval` — same fields, but must be copied, not aliased |
| `mmap`(OABI) / `mmap2` | 90 (oabi only) / **192** | **`mmap` 222** | AArch32 `mmap2` takes `pgoff>>12`; AArch64 `mmap` takes bytes ⇒ shift is mandatory. `MAP_ANONYMOUS`+`PROT_EXEC` is the code-cache path |
| `munmap` | 91 | 215 | must also invalidate translations |
| `mprotect` | 125 | 226 | **critical**: guest `mprotect(RX)`/`RW)` on translated regions must invalidate the code cache for that range, else stale blocks execute |
| `sigreturn` | 119 | absent (`rt_sigreturn` only) | guest legacy `sigreturn` must be serviced by RetroBug (it is invoked from a guest trampoline frame) |
| `rt_sigreturn` | 173 | 139 | see §5.5 |
| `rt_sigaction` | 174 | 134 | **different `struct sigaction`** (AArch32 has `sa_restorer` + `sa_mask` as `sigset_t` = 32-bit words, AArch64 has `sa_flags`+`sa_restorer` but different mask size `NSIG` word count) → full translation of `sigset` (guest 1 vs host 1 words for ≤32? actually AArch32 compat uses 64-bit masks) — **verify against `include/linux/compat.h`, not done here** |
| `rt_sigprocmask` | 175 | 135 | mask-size delta |
| `sigaltstack` | 186 | 132 | layout identical (ptr+int+ptr) but guest stack addr is in the low window |
| `clone` | 120 | 220 | §5.6 — **AArch32 `clone` arg order is `clonep, stack, ptid, ctid, tls` and the *function* is done with `BX` to the child's `pc`, i.e. guest `clone` must be emulated, not forwarded** (`CLONE_SETTLS` semantics + TLS from arg4) |
| `clone3` | 435 | 435 | same number, but `struct clone_args` field widths differ (all `u64`, so actually identical) — prefer `clone3` internally; **note `clone3` is unavailable/filtered in app seccomp on many releases (§5.7)** |
| `futex` | 240 (`sys_futex_time32`) | 98 (`sys_futex`, 64-bit `timespec`) | **`struct timespec` width differs**: AArch32 legacy `futex` uses 32-bit `time_t` (pre-Y2038); RetroBug must convert, or forward to `__NR_futex_waitv`/`futex_time64` (422) |
| `clock_gettime` | 263 (`sys_clock_gettime32`) | 113 / **403 `clock_gettime64`** | AArch32 guest `timespec` = `{int32 sec, int32 nsec}` on old targets, `{int64,int64}` when built with `_TIME_BITS=64` → RetroBug must know which. Android's arm build uses 64-bit `time_t` for modern API levels but a guest `libc` built from AOSP `main` will use `CLOCK_*64` (`__NR_clock_gettime64 = 403`, `syscall.tbl:421`) |
| `tgkill` | 268 | 131 | used by ART/`abort()`; also by the guest's `raise` |
| `ioctl` | 54 | 29 | `_IOC` encoding identical, but any struct arg with `unsigned long`/pointer differs → translate the handful apps use (`FIONREAD`, `TCGETS2`, `ion` legacy, `VIDIOC_*` (camera, V4L2 structs contain `u32`/`u64` — the *V4L2 compat* mess), `GPU_*`) |
| `ptrace` | 26 | 117 | denied (§5.7) |
| `openat` | 322 | 56 | path copy |
| `faccessat` | 334 | 48 | |
| `readlinkat` | 332 | 78 | |
| `statfs` | 99 | 43 | `struct statfs` has different `f_type`/`fsid_t` widths → must translate |
| `prlimit64` | 369 | 261 | struct identical (`u64`), fine |
| `getrandom` | 384 | 278 | |
| `memfd_create` | 385 | 279 | needed by RetroBug's own code cache; note the *guest* can legitimately use it too |
| `rseq` | 398 | 293 | **do not forward** for guest threads unless you translate the `__rseq_cs` arrays — bionic registers `rseq` in `__libc_init` (`unregistered`/translation required). Flag as a real hazard: a guest `rseq` registration pointing at guest code would silently mis-handle abortable lists. Recommendation: have RetroBug's guest libc build pass `RSEQ_FLAG_UNREGISTER`/simply not register, and strip registration from `clone` flags |
| `statx` | **absent** | 291 | AArch32 has no `statx`; a modern AOSP-built guest `libc` may still call it via fallbacks — RetroBug must answer `ENOSYS` so glibc/bionic fall back to `fstatat64` |
| `arm_sync_file_range` | (see `__NR_sync_file_range2`) | `sync_file_range` 227? | arg order swapped (`flags` position) — bionic does this correctly in the guest libc; RetroBug only needs the right numbers |

**Rule of thumb:** never *re-map numbers and forward*. Implement each syscall as a **typed handler**
in RetroBug (`kernel_api` in Berberis: `fcntl_emulation.cc`, `open_emulation.cc`,
`sys_mman_emulation.cc`, `sys_prctl_emulation.cc`, `sys_ptrace_emulation.cc`,
`sigevent_emulation.cc`, `unistd_emulation.cc`, `exec_emulation.cc`,
`main_executable_real_path_emulation.cc`, plus `riscv64/` for the per-ABI dispatch —
[list](https://android.googlesource.com/platform/frameworks/libs/binary_translation/+/refs/heads/main/kernel_api/)).
That structure is directly reusable for an `arm/` directory.

### 5.4 Signals, `siginfo`, `ucontext` (the ugly part)

- ART asks for **SIGSEGV only** and runs the bridge handler before the chain with
  `SIGCHAIN_ALLOW_NORETURN` (`native_bridge_art_interface.cc:124-142`). RetroBug's SIGSEGV handler must
  distinguish (i) a fault on *translated host code* (bug), (ii) a fault inside an *intrinsic* that
  emulated a guest load/store (→ synthesise a guest SIGSEGV with a **guest** `ucontext`), (iii) a
  genuine guest null-pointer deref (→ deliver to the guest handler), (iv) ART's own managed-null/
  implicit-suspend checks (`runtime.cc:1909-1912` `implicit_suspend_checks_` for `kArm64`) which fire
  legitimately in host code and must be passed back to ART untouched.
  **This is the single highest-risk correctness area in the whole project.** Berberis' README
  demonstrates the desired outcome: guest crashes produce a tombstone with both host and guest
  registers + guest backtrace (`README.md:133-238`).
- Delivering a signal to a guest handler requires synthesising an **AArch32 signal frame** on the
  guest stack (with `siginfo_t`, `ucontext_t`, `VfpSigFrame`/`arm_vfp_context`, and the legacy
  `__ARM_NR_*`-style return via a guest `sigreturn` stub) — and Android arm `ucontext` includes
  `uc_mcontext.arm_vfp`/`__reserved` with FP state that a handler may inspect. `COMPAT_VDSO`
  (`arch/arm64/Kconfig:1856`) exists on real hardware precisely to supply `__kernel_rt_sigreturn`.
  RetroBug must hand-roll all of it. **Verify** layouts against
  `arch/arm/include/uapi/asm/sigcontext.h`, `asm/siginfo.h`, `asm/ucontext.h` and
  `include/linux/compat.h` (not fetched this session; deferred to a follow-up task).
- Also: `SA_RESTORER` is *required* on AArch32 and absent in practice on AArch64; guest
  `rt_sigaction` without `SA_RESTORER` is `EINVAL`. Must be reproduced.

### 5.5 auxv / `getauxval`: what a 32-bit binary expects

The guest `libc`/linker reads `getauxval()` — this is a **vDSO/AT_* memory read, not a syscall**, so
RetroBug must construct an AArch32 `elf_auxv` for the guest process:

- `AT_HWCAP` — ARM bits from
  [`arch/arm/include/uapi/asm/hwcap.h`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/arch/arm/include/uapi/asm/hwcap.h):
  `SWP 1<<0, HALF 1<<1, THUMB 1<<2, FAST_MULT 1<<4, VFP 1<<6, EDSP 1<<7, NEON 1<<12, VFPv3 1<<13,
  VFPv3D16 1<<14, TLS 1<<15, VFPv4 1<<16, IDIVA 1<<17, IDIVT 1<<18, VFPD32 1<<19, LPAE 1<<20,
  EVTSTRM 1<<21, FPHP 1<<22, ASIMDHP 1<<23, ASIMDDP 1<<24, ASIMDFHM 1<<25, ASIMDBF16 1<<26, I8MM 1<<27`.
  RetroBug must advertise **exactly the subset its decoder implements** (`TLS`+`NEON`+`VFPv3`+`IDIV*`
  ≈ a Cortex-A15-ish baseline). Advertising `HWCAP_IDIVA` without implementing `UDIV/SDIV` in T32,
  or `HWCAP_NEON` without full NEON, is the classic "instant SIGILL/illegal-instruction storm"
  failure mode. Conversely, hiding `NEON` will make libyuv/`-mfpu=vfpv3-d16` paths run slowly but
  correctly. **Recommendation:** start `HWCAP = SWP|HALF|THUMB|FAST_MULT|VFP|EDSP|TLS|VFPv3|VFPv3D16|IDIVA|IDIVT|VFPD32`
  (no NEON) for bring-up, then flip NEON on behind a flag with an instruction-completeness test gate.
- `AT_HWCAP2` — `AES/PMULL/SHA1/SHA2/CRC32/SB/SSBS` (same header, 41-47): advertise only if
  implemented.
- `AT_PLATFORM` — for ARM this is `ELF_PLATFORM` = `"v7l"`/`"v8l"` (kernel sets it from
  `system_elf_platform`); bionic's `linker`/`libc` do use `AT_PLATFORM` historically. **Unverified**
  whether the *arm64* compat path reports `"v8l"` for 32-bit tasks; grep
  `arch/arm64/include/asm/elf.h`/`arch/arm/include/asm/elf.h` in a follow-up.
- `AT_BASE` — address of the **guest linker** map (the native bridge's `linker64`/`linker` is a
  guest binary, §4.2) — RetroBug's loader must fill this with the guest `PT_INTERP` base it chose.
- `AT_PAGESZ` — must be **4096** for a 32-bit guest. On a 16 KB-page host the real granularity is
  16384, so RetroBug must (a) report 4096 to the guest, and (b) round every guest `mmap`/`mprotect`
  to the host page size, and (c) reject/adjust guest segments that are 4 KB-aligned but not 16 KB
  aligned (this is the same problem that killed 32-bit support on 16 KB devices; AOSP's
  `linker_phdr_16kib_compat.cpp` and the `native_bridge_support` commit *"Propagate 16kB app compat
  flag"* (repo HEAD message, 2025-03-14) confirm it is a live upstream concern).
  **This is a top-3 risk.**
- Others expected present: `AT_CLKTCK` (100), `AT_PHDR/AT_PHENT/AT_PHNUM/AT_ENTRY` (of the guest
  `app_process`/`linker`), `AT_UID/EUID/GID/EGID`, `AT_SECURE` (1), `AT_RANDOM` (16 bytes in the
  *low* window — bionic reads it at startup for stack-guard init), `AT_EXECFN`, `AT_FLAGS`,
  `AT_SYSINFO_EHDR` (**only present when the real kernel gives a compat vDSO**; RetroBug must instead
  supply its own `libnative_bridge_vdso.so`, which is exactly what AOSP does —
  `NATIVE_BRIDGE_PRODUCT_PACKAGES += libnative_bridge_vdso.native_bridge` and
  `system/lib64/riscv64/libnative_bridge_vdso.so`, `native_bridge_support.mk:34`,
  `berberis_config.mk:116`). **Key architectural insight:** the guest vdso is the *sanctioned trap
  site* — guest libc's syscall stubs land in a known guest mapping, so RetroBug intercepts at a known
  address instead of pattern-matching `SVC` everywhere (it must still catch stray `SVC` in bundled
  libs). Berberis' own tombstone shows exactly that: guest frame `#02 …
  /system/lib64/riscv64/libnative_bridge_vdso.so` (`README.md:213-217`).
- `AT_HWCAP2`-adjacent: bionic exposes `getauxval(AT_EXECFN)` and `AT_SECURE` to apps, and the guest
  `linker` reads `AT_PHDR/AT_BASE`; missing entries → `LOG_ALWAYS_FATAL` in the guest linker.

### 5.6 The `kuser` page at `0xffff0000` and `uid`/`pid` model

- **kuser helpers**: `CONFIG_COMPAT` explicitly covers "the user helper functions"
  (`arch/arm64/Kconfig:1816-1819`). The AArch32 kuser page supplies `__kuser_cmpxchg` (0xffff0020),
  `__kuser_cmpxchg64` (0xffff0060), `__kuser_memory_barrier` (0xffff0fa0),
  `__kuser_get_tls` (0xffff0fe0). Do 32-bit Android userspaces still need it? **Mostly no, but not
  never:** modern bionic uses `MRC …c13,c0,3` and native atomics; however kuser page 0xffff0000 is
  *still* emulated by the kernel because old binaries contain `MOV PC, R3`-through-0xffff0fc0
  sequences. For RetroBug: **map a 4 KB guest page at `0xffff0000` and implement the four helpers
  (cheap, ~50 lines), and refuse `MAP_FIXED` there.** Risk if skipped: a whole class of ~2011-era
  `.so` files faults. **Unverified** whether Android's arm build ever emitted kuser calls; check
  `bionic/libc/kernel/arch-arm/`/`__kuser` references in a follow-up.
- **uid/pid model**: a RetroBug process is a single Android uid with a single `pid`. Guest `getpid`
  must return the host pid (bionic caches it in the `__preinit_array`/`_getpid` fast path — RetroBug
  should serve `getpid`/`gettid` from the guest `ThreadState`), `getuid`/`geteuid` return RetroBug's
  uid (guest "app sandbox" is the same sandbox — no isolation gain), `setuid/setgid/fork/execve`
  must fail (`EPERM`/`EACCES`), `prctl(PR_SET_NAME)` should forward (it changes the *host* thread
  name, which is actually desirable for debugging; native bridge does exactly that),
  `prctl(PR_SET_VMA_ANNOUNCE)` should forward with a "retrobug:" prefix so `/proc/self/maps` is
  readable. **`/proc/self/maps` and `/proc/self/status` are read by many guest libs** (crash
  reporters, DRM, anti-cheat) and will show *host* arm64 mappings, breaking them; Berberis added
  `main_executable_real_path_emulation.cc` + `open_emulation.cc` precisely to virtualise `/proc`
  (kernel_api listing). RetroBug must do the same: synthesise a guest-view `maps` with 32-bit
  addresses and guest file names, or accept mass breakage of crash handlers.

### 5.7 seccomp: will RetroBug's own process be allowed to do what the guest needs?

- App processes run a **generated BPF filter**: `bionic/libc/seccomp/seccomp_policy.cpp` selects
  `arm64_app_filter` with `PRIMARY_ARCH AUDIT_ARCH_AARCH64` (lines 33-37), plus a "32-bit policy"
  jump for AArch32 tasks (lines 210-211, 275-276 comments) — i.e. bionic maintains *separate* filters
  per compat arch and the *arch token* is checked, so an AArch32-shaped syscall number from an arm64
  task would never match anyway. The per-arch `policy_*.cpp` files named in the brief no longer
  exist: `bionic/libc/seccomp/` now contains only `gen_syscall_nrs.cpp`,
  `gen_syscall_nrs_x86*.cpp`, `seccomp_bpfs.h`, `seccomp_policy.cpp`, `include/`
  ([listing](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/seccomp/)).
- Practical consequence (**inference from the above + bionic's known `SIGSYS`-kill behaviour**):
  RetroBug's host process is under the **arm64 app filter**, and the guest's needs map onto a *host*
  syscall set that is a subset of what apps already use — `mmap/mprotect/munmap/madvise`, `openat`,
  `readlinkat`, `fstat`, `getrandom`, `memfd_create`, `futex`, `tgkill`, `clone` (thread create),
  `sigaltstack`, `rt_sigaction`, `prctl`, `ioctl`. All of those are permitted today (ART's JIT and
  bionic need most of them). The ones RetroBug must **not** attempt from an app process: `execve`,
  `fork`/`vfork`, `ptrace`, `personality`, `setuid*`, `unshare(CLONE_NEWNS)`, `mount` — the last two
  being precisely what the native-bridge pre-init does (§1.4). Also **`process_vm_readv/writev`** and
  **`pidfd_*`** are filtered on some releases; **verify the exact per-release list** by dumping
  `bionic/libc/seccomp/`'s generated `*_app_filter` arrays on a device (method:
  `strace -e trace=%process` a JIT-heavy app, or read `seccomp_filters` from
  `/proc/self/status`'s `Seccomp:` field).
- If RetroBug ever runs under a ROM integration (§1.4 / native bridge), it inherits the *app* filter
  too — Houdini/Berberis don't get a privileged filter, which is a good sign for syscall parity.
- Guest threads: use `clone3`/`clone` with `CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|
  CLONE_SETTLS` semantics *emulated* (host `pthread_create` + guest TLS install), not raw forwarded
  `clone`, because `CLONE_SETTLS` on AArch64 installs `tpidrro_el0`/`tpidr_el0` with a host pointer
  while the guest needs `GuestState.tp`.

---

## 6. What an APK actually needs to launch without a guest OS

### 6.1 The real bring-up sequence (AOSP `main`, file:line)

```
init(1) → app_process (cmds/app_process/app_main.cpp)
  AndroidRuntime::start()  (frameworks/base/core/jni/AndroidRuntime.cpp)
     ├─ JNI_CreateJavaVM → Runtime::Init() → LoadNativeBridge()   [art/runtime/runtime.cc:2173]
     ├─ -XX:NativeBridge=  added ONLY if zygote                   [AndroidRuntime.cpp:1105-1116]
     └─ callMain("com.android.internal.os.ZygoteInit")
ZygoteInit.main() → registerZygoteSocket() → preload(classes,resources) → runSelectLoop()
  forkSpecializeAppProcess/forkAndSpecialize()
     ├─ com_android_internal_os_Zygote.cpp:1921-1980  needs-NB → PreInitializeNativeBridge
     ├─ setresuid/setresgid, capabilities drop, seccomp filter install, mount ns, rlimits
     └─ Runtime::InitNonZygoteOrPostFork(..., kInitialize, isa) → InitializeNativeBridge [runtime.cc:1206-1222]
        → ZygoteHooks.postForkZygote? → Runtime::DidForkFromZygote
  (Java) Zygote.forkAndSpecialize → ZygoteInit.handleChildProc → ActivityThread.systemMain()
ActivityThread.main(String[])                       [frameworks/base/core/java/android/app/ActivityThread.java]
  → attach() binder IPC to ActivityManagerService
  → IApplicationThread.bindApplication(...)          [ActivityThread.java:1290]
  → H.BIND_APPLICATION → handleBindApplication(data) [ActivityThread.java:2512-2518]
       ├─ createAppContext / LoadedApk.getClassLoader()   ← PathClassLoader + librarySearchPaths
       ├─ instrumentation .callApplicationOnCreate via
       │    LoadedApk.makeApplicationInner(...)            [ActivityThread.java:4139]
       │      → Application.attachBaseContext(base) → Application.onCreate()
  → scheduleLaunchActivity → performLaunchActivity(r) [ActivityThread.java:4062]
       ├─ mInstrumentation.newActivity(cl, name, intent)     [4121]   ← PUBLIC API
       ├─ r.packageInfo.makeApplicationInner(false, mInstr)  [4139]
       ├─ activity.attach(context, thread, instr, token, ...)[4179]   ← HIDDEN (@UnsupportedAppUsage)
       └─ mInstrumentation.callActivityOnCreate(...)         [4208-4210]
```
Native code enters at four distinct moments: (a) `preload()`/framework JNI (host-only), (b)
`System.loadLibrary` from guest static initializers (→ §1.2 chain), (c) `JNI_OnLoad`
(`java_vm_ext.cc:1132-1140`), (d) every `native` method invocation (→ §2).

### 6.2 What RetroBug must supply itself, as an ordinary app

| Need | How, and what it costs |
|---|---|
| Process creation | **Don't.** One RetroBug process hosts N guest apps (or 1 for v1). Android gives you one zygote-forked process; spawning a second *named* process legitimately requires either `:remote` services (declared in RetroBug's own manifest — a fixed pool of guest slots, e.g. `GuestProcess0..7`) or WorkManager/`START_STANDING` tricks. Recommendation: declare ~4-8 `android:process=":guestN"` services in RetroBug's manifest and pin each guest app to one. **This is the clean, non-fragile way to get process isolation without root.** |
| APK install/extract | Guest APK comes from: user file pick (`ACTION_OPEN_DOCUMENT`), or RetroBug's own download. `nativeLibraryDir` doesn't exist for it → RetroBug extracts `lib/armeabi-v7a/*.so` itself into `<files>/guest/<pkg>/lib/`. (Only needed for the *guest* linker; host never sees them.) |
| DEX loading | `dalvik.system.DexClassLoader(apkPath, nativeLibSearchPath, libZip?, parentLoader)` — **public API**. `InMemoryDexClassLoader` exists but needs the dex unzipped. Use `PathClassLoader(apk, libPath, parent)` where possible to get resource/`LoadedApk`-like behaviour; note `Runtime.loadLibrary0` special-cases `PathClassLoader`/`DelegateLastClassLoader` (`Runtime.java:1066-1078`) — **so prefer `PathClassLoader`, because its `findLibrary` failure still retries with the SONAME, which is exactly the fallback RetroBug wants to control.** Better: *subclass* `PathClassLoader` and override `findLibrary` — but note the check is `loader.getClass() == PathClassLoader.class`, so a subclass loses that fallback (§**gotcha, verified** at `Runtime.java:1066-1067`). |
| Resources | `AssetManager` is not constructible publicly; the supported route is `ResourcesWrapper`/`Context.createPackageContext` (needs the guest to be *installed* — impossible) → RetroBug must build resources via public `AssetManager.addAssetPath` (hidden) or `Resources(new AssetManager()...)` (hidden) → **hidden API**, or use `android.content.res.Resources` obtained from a `Context` created through `ContextThemeWrapper` + reflection. Real-world projects use `Reflection` here; flag as fragile (§6.3). |
| `Application` + `Context` | `Instrumentation.newApplication(cl, name, ctx)` is **public**. Getting a `Context` with the guest's resource table is the hard part. |
| `Activity` hosting | `Instrumentation.newActivity()` is public; **`Activity.attach(...)` is not** (`@UnsupportedAppUsage`, greylist/blacklist). Without `attach` the Activity has no `Window`, no `ActivityToken`, no `mParent`, no `Intent`, and lifecycle callbacks won't work. Options: (i) hidden-API bypass (§6.3); (ii) **Activity Embedding / `TaskFragment`** (Jetpack `androidx.window.extensions.embedding`, requires OEM-provided extensions — real but limited device set); (iii) draw guest UI into RetroBug's own window via `SurfaceControl`/`SurfaceView` + guest `WindowManager` (needs `WindowManagerGlobal` reflection — hidden). (iv) accept headless/no-UI for v1. |
| `PackageManager`/`ActivityManager` spoofing | Binder proxies (`IPackageManager`/`IActivityManager` via `ServiceManager.getService` or `ActivityManager.IActivityManagerSingleton`) = **hidden API**, plus `IPackageManager$Stub.asInterface` reflection. Fragile across releases (AIDL method-number renumbering breaks "shadow classes" — the classic VirtualApp failure mode). |
| Threads/`Looper` | Public (`Looper.prepare`, `HandlerThread`). Guest main thread = a RetroBug thread with its own `Looper` and a `ThreadState`. |
| `ContentProvider`s | `IContentProvider` proxies via hidden `ContentProviderHolder`; or expose RetroBug's own provider authorities and rewrite URIs (public but only works for apps that use `ContentResolver` — most do). |
| `BroadcastReceiver` / permissions | RetroBug registers **its own** receivers and forwards to guest `onReceive` objects (public API; works). Dynamic guest receivers must be captured by hooking `Context.registerReceiver` at the *guest* layer (i.e. RetroBug supplies its own `Context` implementation subclass — `ContextWrapper` is public ⇒ **this is the clean path**). |
| `ActivityThread` hacks | `ActivityThread.currentActivityThread()` hidden; used to fetch `mInstrumentation`, `mPackages`. Fragile; the `Instrumentation` route (`android.app.Instrumentation` subclass declared in RetroBug's manifest and started via `am instrument` **or** `startInstrumentation()` which is `@UnsupportedAppUsage`… **verify**) is what VirtualApp-family actually does. |

### 6.3 How the "APK-in-APK" projects do it, and what survives on Android 14/15/16

Real techniques (as used publicly by VirtualApp/BlackBox/Multiple App Cloner/Parallel Space/Wave):

1. **Manifest pre-declaration of stub components**: `com.lody.virtual.stub.ShadowActivity$P0..P119`
   (+`$P0T1` transparent/dialog variants, `ShadowJobService`, `ShadowContentProvider`) declared in the
   *host* APK manifest with distinct processes and `intent-filter`s. The guest's real Activity is
   never in any manifest; `IActivityManager` receives the stub's `ComponentName`, and the client-side
   proxy rewrites it back. **The "1000 activity limit" in the brief is a myth/mis-remembering: the
   limit is however many stubs the host author declares** (VirtualApp historically ~100-120 per
   process type; modern clones declare several hundred). Practical ceilings are manifest size and
   Play Store scrutiny, not framework limits. **Unverified/soft claim** — treat as "no framework
   limit found in AOSP; number is a product decision".
2. **Binder proxying**: `ServiceManager.sServiceList`/`getService("activity"|"package"|"notification"|
   "clipboard"|…)` → `IInterface` wrappers installed into `ActivityManager.IActivityManagerSingleton`,
   `NotificationManager.sService`, `ActivityThread.sManager` etc. via reflection on
   `android.os.ServiceManager` + `IPackageManager$Stub.asInterface`. Uses hidden APIs *en masse* and
   hard-codes `TransactionCode`s → **breaks every Android major release**.
3. **Hidden-API bypass**: [LSPosed/AndroidHiddenApiBypass](https://github.com/LSPosed/AndroidHiddenApiBypass)
   — calls `dalvik.system.VMRuntime.setHiddenApiExemptions("L")` obtained via
   `Unsafe`/`MethodHandles` on `sun.misc.Unsafe`/`jdk.internal.misc.Unsafe` (the "meta-reflection"
   trick of `Class.forName("...")` through an exempt caller). Documented restrictions and the
   official policy: [Restrictions on non-SDK interfaces](https://developer.android.com/guide/app-compatibility/restrictions-non-sdk-interfaces).
   **Honest status:** the *public* documentation states the restrictions are enforced for
   `targetSdk ≥ 28` and Google "expands the lists over time"; the bypass techniques are
   cat-and-mouse. **I could not verify from a primary source whether `HiddenApiBypass` still works on
   Android 16 (`targetSdk 36`) at the time of writing — marked unverified (OQ-5).** Plan for it to
   break and gate every hidden-API use behind a capability probe.
4. **`Instrumentation` swap**: replace `ActivityThread.mInstrumentation` with a subclass so
   `newActivity`/`execStartActivity`/`callApplicationOnCreate` are intercepted (this is how "app
   cloners" get the guest's classes instantiated with a working-ish `Activity`). One field write via
   reflection.
5. **IO redirection**: `Os.open`-level hooking was historically done by patching libio or by
   `FileUtils` reflection; on modern Android the workable approach is a `Context`
   `getFilesDir()`/`getDataDir()` override (public `ContextWrapper`) + `nativeLibraryDir` override
   (`ApplicationInfo.dataDir/nativeLibraryDir` are `@UnsupportedAppUsage` but writable via
   `ApplicationInfo` public fields — `nativeLibraryDir` **is** public). `untrusted_app` cannot
   `chroot`/`setns`, so no real mount tricks.
6. **Foreground/process persistence**: `START_STICKY` services, `FOREGROUND_SERVICE_*` types
   (Android 14 requires a declared type + justification), `SCHEDULE_EXACT_ALARM` restrictions;
   background *activity launch* is blocked since Android 10 with narrow exemptions (a visible
   window/notification PendingIntent tap) — RetroBug launching a guest Activity **in response to a
   tap inside RetroBug's own visible Activity** is legal, and this is the recommended UX.
7. **`Island`/`Shelter`** — legitimate, no emulation: they use **Device Policy Manager work
   profiles** to *clone* apps within the same runtime. They solve multi-instance, not ABI. Useful
   precedent for RetroBug's UX story ("managed profile" gives real isolation + admin rights, no
   hidden APIs) but contributes nothing to ARM32 execution.
8. **Commercial "Wave/APKLauncher"-style** and `VMOS`/`Parallel Space`: closed source; VMOS-style
   products run a *full guest ROM in a VM/container* (explicitly out of scope per RetroBug's
   constraint). `Multiple App Cloner`-class products are technique (1)+(2)+(4).
   `VirtualApp` OSS: README states **supports Android 5.0–17.0** but *"GitHub code stopped updating
   in Dec 2017; commercial version continues"*
   ([asLody/VirtualApp](https://github.com/asLody/VirtualApp)) — i.e. **the open-source lineage is
   already dead on modern Android; the surviving implementations are closed.** That is an important,
   honest data point for RetroBug's risk assessment: a normal-APK virtualization layer has been
   unsupportable-by-upstream for 8 years.

### 6.4 RetroBug's recommended v1 scope (decisive)

- **v1a (no privilege, no UI): "guest as library".** RetroBug loads guest DEX in its own process,
  runs `Application`-less code, and executes guest native libs through the translator. Use cases:
  games' logic/DSP libs, `libnativehelper`-free SDKs, unit-test harnesses, `Service`-less jobs. Zero
  hidden APIs beyond `PathClassLoader` (public) + RetroBug's own `findLibrary` override.
- **v1b (low privilege, real UI): host-side `Activity` per guest Activity, `Instrumentation`-based**,
  using `newActivity` + `ContextWrapper`-supplied `Context`, with `Activity.attach` behind a
  HiddenApiBypass capability probe and a degraded "no window" mode if unavailable.
- **v2 (partner ROM): the same engine exposed as `NativeBridgeItf` v8**, where UI, `PackageManager`,
  `Build.CPU_ABI` spoofing, `/proc/cpuinfo` bind-mount and namespace pre-init come free from the
  platform. **This is where RetroBug's "run any legacy APK" promise is actually deliverable**, and
  it is a strict superset of the engine, not a different product.

### 6.5 Non-native reasons old apps break on modern ART (RetroBug must also fix/shim)

Because RetroBug runs guest DEX on a *modern* framework, these bite regardless of emulation:
`PendingIntent` mutability requirement (Android 12/S crash for `FLAG_IMMUTABLE`-less code),
non-SDK interface blocks (NoSuchMethodError), removed `RenderScript` (Android 12) and
removed `android.webkit.WebView` classic APIs, `org.apache.http.legacy` now a separate optional
library (`uses-library` must be declared), `javax.xml.bind`/`java.lang.SecurityManager` removals,
`android.permission.*` runtime model, `startForegroundService` 5s rule, exact-alarm permission,
`requestIgnoreBatteryOptimizations`, and the Android 16/"existing apps must target ≥ 35 to remain
available to new users" Play rule (§7.1). RetroBug's "reproduced Android runtime/API layer" should
include a **compat shim layer** (per-targetSdk behaviour switches) — that is a bigger surface area
than the emulator and should be planned as such. **Inference**, but well-founded:
`targetSdkVersion` is threaded into the linker and ART (`linker.cpp:245,1087`;
`java_vm_ext.cc:1069`) so AOSP itself treats per-app targetSdk as a first-class compatibility axis.

---

## 7. The API-level surface to reproduce

### 7.1 Which 32-bit-era Android versions matter

- RetroBug's target cohort = APKs whose `lib/` contains only `armeabi-v7a`/`armeabi`, typically
  `minSdk 8-16`, `targetSdk 10-23`. Google Play has required 64-bit native code for new apps/updates
  since 2019-08 (`Support 64-bit architectures`, developer.android.com/google/play/requirements/64-bit),
  and since Android 11-ish the N/CDD no longer requires 32-bit support at all — that is the whole
  supply of "APKs that cannot run" RetroBug addresses.
- Play target-API policy (verified from the target-API page, Aug 2026 milestone): from
  **2026-08-31** new apps/updates must target **API 36 (Android 16)**; existing apps must target
  **≥ API 35** "to remain available to new users on devices running an Android version higher than
  the app's target" — i.e. legacy 32-bit apps are being *delisted by policy* as well as by ABI.
  [Meet Google Play's target API level requirement](https://developer.android.com/google/play/requirements/target-sdk).
  Useful for RetroBug's positioning; also note `armeabi` (v5/v6) and `mips` builds must be assumed
  present in the very old tail.
- `ABI`-related: `armeabi-v7a` implies ARMv7+VFPv3-D16 minimum, and (per NDK docs) NEON is expected
  on "most" devices and was **required by the CDD from Marshmallow/API 23**
  ([BuildSystemMaintainers.md:87-93](https://android.googlesource.com/platform/ndk/+/refs/heads/master/docs/BuildSystemMaintainers.md)).
  So `AT_HWCAP` advertisement must be consistent with §5.5's decoder completeness.

### 7.2 Which system libraries guest libs link against — **use AOSP's own answer**

`native_bridge_support.mk` splits guest libraries into two categories, and this is the best available
authoritative list of "what a native bridge must provide":

**Unmodified ("original") guest libraries — pass-through, no ABI translation needed
(`native_bridge_support.mk:63-78`):**
`libandroidicu`, `libcompiler_rt`, `libcrypto`, `libcutils`, `libdl`, `libdl_android`, `libicu`,
`liblog`, `libm`, `libsqlite`, `libssl`, `libstdc++`, `libsync`, `libutils`, `libz` (+ Renderscript
`libRS`, `libRSDriver`, `libnative_bridge_guest_libRSSupport`, arm32-only `libclcore_neon.bc`,
lines 118-131).

**Modified guest libraries that need a host-side proxy counterpart
(`native_bridge_support.mk:83-103` + `berberis_config.mk:25-51`):**
`libc`, `libnativehelper`(JNI), `libandroid`, `libandroid_runtime`, `libEGL`, `libGLESv1_CM`,
`libGLESv2`, `libGLESv3`, `libOpenSLES`, `libOpenMAXAL`, `libaaudio`, `libamidi`, `libbinder_ndk`,
`libcamera2ndk`, `libjnigraphics`, `libmediandk`, `libnativewindow`, `libneuralnetworks`,
`libvulkan`, `libwebviewchromium_plat_support`.

**Full guest `/system/lib*/<isa>/` distribution list** (what RetroBug must actually ship for
arm32, `berberis_config.mk:56-128`) — includes everything above plus `ld-android.so`, `libc++.so`,
`libicu.so`/`libicuuc`/`libicui18n`, `libbase.so`, `libnative_bridge_vdso.so`,
`libandroid_runtime.so`, `libvndksupport.so`, `libwebviewchromium_plat_support.so`.

Corrections/notes vs the brief: `libstdc++` on Android is a **legacy 4-symbol compat shim**, not
"aka libc++"; the C++ runtime is `libc++.so` (system, static-linked by NDK as `libc++_shared.so`);
`libOpenSLESEs` is a typo for `libOpenSLES.so`; `libGLESv3.so` **is** in the list; `libz`/`libpng`:
`libz` yes (see list), `libpng` is **not** a system lib exposed to apps (apps bundle it);
`libsqlite` exists as a system lib but was never NDK-public (apps bundle SQLCipher instead).
`libcrypto`/`libssl` **are** in the guest list — but linking them was `targetSdk`-restricted over
time, so many APKs bundle their own copies (which RetroBug then simply emulates; no wrapper needed).

**Real-world APK evidence is thin in this session.** The one quantitative study I could reach
([arXiv 1911.09716, "Too Quiet in the Library"](https://arxiv.org/html/1911.09716v2)) reports a
200-app sample of which **145 (72.5%) contained native libraries**, with FFmpeg/OpenSSL/OpenCV among
the most frequent — but that sample is *selected for having native code*, so it must not be
extrapolated. See §8.

### 7.3 Where to get a legally distributable set of 32-bit bionic system libraries

**Recommendation: build them from AOSP source; do not extract from devices.**

- `bionic` (libc/libm/libdl/linker) is BSD-3-clause-style: `bionic/linker/NOTICE` +
  `bionic/linker/MODULE_LICENSE_BSD` marker
  ([listing](https://android.googlesource.com/platform/bionic/+/refs/heads/main/linker/)); individual
  files carry the 2-clause BSD header (e.g. `libc/private/bionic_tls.h:1-27`).
- `libnativehelper` / `native_bridge_support` / `binary_translation`: AOSP
  `package { default_applicable_licenses: ["Android-Apache-2.0"] }`
  (`android_api/libc/Android.bp:23-25`).
- `libc++` (LLVM): Apache-2.0 **with LLVM exception** for the runtime; `system/libcxx` in AOSP carries
  the same. Ship the NOTICE + license text with the APK/asset bundle.
- `libsqlite`/`libz`(zlib)/`libpng`: public domain / zlib licence — fine.
- `libicu`/`libandroidicu`: ICU (BSD-ish) + Unicode — fine but big; RetroBug can drop them if it
  doesn't run guest framework libs (it doesn't: DEX runs on host).
- Extracting `.so` files from a **device** risks (a) vendor-specific GPL/proprietary blobs in
  `/vendor/lib` (GPU drivers, `libsqlite3` forks), (b) OEM firmware licence terms, (c) no
  ability to patch (e.g. to add the `CRT_LEGACY_WORKAROUND` / `native_bridge` flags that make guest
  code work, `android_api/libc/Android.bp:76-78`). Building from AOSP also gives RetroBug the
  `.arm.map` version scripts it needs for symbol-version compat (`libc.arm.map`, `linker.arm.map`).
- Practical build: check out AOSP `main` (or a stable `android15-release`-era tag), add
  `PRODUCT_SOONG_NAMESPACES += frameworks/libs/native_bridge_support/android_api/libc` style
  namespaces (`android_api/libc/Android.bp:17-21` says guest libs are built only for products that
  opt in), and build `libnative_bridge_guest_*` for `TARGET_ARCH=arm` /
  `TARGET_RELEASE` with `multilib`. RetroBug then ships those arm32 ELF files as APK assets
  (~2-4 MB compressed). **Unverified:** whether arm32 `libnative_bridge_guest_*` still builds on
  `main` — the arm32 variant files exist (`stubs_arm.cpp`, `init_static_tls_arm.S`,
  `libc.arm.map`, `NATIVE_BRIDGE_PRODUCT_PACKAGES_ARM`), but the riscv-only
  `berberis_config.mk` suggests arm32 guest build coverage may be stale. **Task: prove it with a
  build before committing to reuse.**

---

## 8. Apps with no native code at all (zero-emulation cohort)

**No reliable published number was found in this session** for "share of 32-bit-only APKs that
contain no `lib/` at all". What exists: the 72.5% figure in §7.2 (biased sample), and various
store-analytics vendors' "X% of apps ship native libs" numbers that don't separate 32-bit-only from
multi-ABI. **Treat "most old pure-Java apps need no emulation" as plausible but unmeasured.**

Why the cohort is *not* the easy win it looks like, honestly: an API-10 pure-DEX app running on a
modern host ART hits §6.5 (non-SDK blocks, PendingIntent, removed optional libs) — i.e. "zero
emulation" ≠ "zero work". RetroBug's reproduced runtime layer is still needed for that cohort, just
without the translator.

**Measurement method RetroBug should run (one week of work, reproducible):**
1. Corpora: (a) F-Droid's `index-v1.json` (public, ~50k APKs, includes `nativeAbis` per package —
   actually free signal: F-Droid records `nativeLibrary` ABIs per release); (b) `Aptoide`/`Yalp`
   mirrors or the `apkeep` tool for Play; (c) the user's own legacy library. Prefer (a) for legality.
2. For each APK: unzip `lib/**` listing, `AndroidManifest.xml` via `apkanalyzer`/`aapt2 dump
   badging` → `minSdkVersion`, `targetSdkVersion`, `uses-split`, `uses-library`; record ABIs present
   in `lib/`, presence of `classes.dex`/`classes*.dex`, total dex vs native bytes, and whether
   `arm64-v8a` exists.
3. Buckets that matter to RetroBug: (i) no `lib/` at all → no emulation; (ii) `arm64-v8a` present →
   installable natively today; (iii) `armeabi-v7a`-only → **RetroBug's core cohort**; (iv)
   `armeabi`/`mips`-only → decoder tail work; (v) splits where `split_config.arm64_v8a.apk` is
   missing from the store.
4. Cross-tabulate (iii) against `targetSdkVersion` and last-update year → gives the *usable* RetroBug
   TAM and tells RetroBug which API-era shims (§6.5) to build first.
5. Also count, within (iii), how many `lib/armeabi-v7a/*.so` files are `DT_NEEDED`-only on NDK
   libs (→ proxy libs cover them) vs. ones that bundle their own OpenSSL/ffmpeg (→ pure emulation).

---

## Emulate vs reproduce boundary table

| Surface | Run **natively** on host | **Emulate** (translate ARM32) | **Reproduce** in RetroBug (write it) |
|---|---|---|---|
| `classes*.dex`, ART JIT/AOT, GC, `String`, reflection | ✅ host ART | — | — |
| Boot classpath / `libcore` / `framework.jar` | ✅ host | — | per-targetSdk compat shims (§6.5) |
| `Resources`/`AssetManager`/`ResTable` | ✅ host (`AssetManager2`) | — | guest-path redirection |
| Binder to system services | ✅ host (RetroBug's own uid) | — | proxying/spoofing for guest components (§6.2/6.3) |
| `Activity`/`Service`/`Broadcast` lifecycle | host framework | — | `Instrumentation`-based hosting + `ContextWrapper` |
| `java.lang.System.loadLibrary` / `Runtime.nativeLoad` | ✅ host ART | — | **`ClassLoader.findLibrary()` override → arm64 shadow `.so`** (§1.2) |
| `dlopen()` of app `.so` | host `dlopen` loads the **shadow** lib only | ✅ guest `dlopen`/`android_dlopen_ext` inside RetroBug | guest linker config (`ld.config.arm.txt` equivalent) |
| 32-bit `linker` + namespace/reloc semantics | ❌ | ✅ (real AOSP arm linker under translation, §4.3) | namespace config + targetSdk hacks feed-in |
| Guest ELF segments, `.ARM.exidx` unwinding | ❌ | ✅ | frame maps for tombstones |
| A32/T32 + VFPv3 + NEON instructions | ❌ | ✅ (Berberis fork + ARM32 backend) | — |
| `TPIDRURW`/`r9` TLS, kuser page `0xffff0000` | ❌ | ✅ (guest `tp` in `ThreadState`; 4 KB helper page) | helper implementations (§3.4, §5.6) |
| Guest `libc.so`(arm32 bionic), `libm`, `libdl`, `libz`, `liblog`, `libstdc++`, `libsqlite`, `libcrypto/ssl` | ❌ | ✅ (built from AOSP, §7.3) | syscall/proxy back-ends |
| `libc++_shared.so` / `libgnustl_shared.so` / `libstlport_shared.so` bundled by app | ❌ | ✅ (they're just guest `.so`) | — |
| NDK API libs: `libandroid`, `libEGL`, `libGLESv1_CM/2/3`, `libOpenSLES`, `libOpenMAXAL`, `libaaudio`, `libamidi`, `libvulkan`, `libmediandk`, `libcamera2ndk`, `libbinder_ndk`, `libnativewindow`, `libjnigraphics`, `libneuralnetworks`, `libwebviewchromium_plat_support`, `libandroid_runtime` | host implementation exists (arm64) | guest-side *shim* only | **ABI proxy libs auto-generated from headers** (§1.5/§7.2 — reuse `android_api` generator) |
| `libnativehelper`/`JNIEnv` vtable | host ART | guest-side table | per-shorty thunks + handle tables (§2) |
| JNI handles (`jobject`/`jclass`/`jmethodID`/`jfieldID`) | host values | 32-bit aliases | **handle/shadow tables** (§2.2) |
| `GetStringUTFChars`/`GetPrimitiveArrayCritical`/direct buffers | host memory | guest-window memory | low-4 GB allocation window (§2.5) |
| Linux syscalls (ARM EABI numbers) | ❌ (wrong ABI) | ✅ trap + dispatch | typed handlers per call (§5.3) |
| `cacheflush` / `BKPT` / `set_tls` (`0xf000x`) | n/a | ✅ mandatory | code-cache invalidation (§5.2) |
| `auxv` (`AT_HWCAP/2`, `AT_PLATFORM`, `AT_PAGESZ`, `AT_BASE`, `AT_RANDOM`) | ❌ | guest reads it | **construct AArch32 auxv** (§5.5) |
| Signal delivery to guest handlers | host sigchain owns SIGSEGV | guest frames/sigframes | `SigchainAction`-like registration (normal-APK: `sigaction` in RetroBug's own .so; bridge: `getSignalHandler`, §1.3) |
| `mmap` executable memory for translated code | ✅ host `mmap`/`memfd_create` | — | code cache + `mprotect`/`cacheflush` invalidation policy |
| seccomp | host app filter applies to RetroBug | — | nothing (stay inside the filter, §5.7) |
| `Build.CPU_ABI`/`SUPPORTED_ABIS`/`os.arch` | host values leak | — | spoof (bridge mode: free via `getAppEnv`; app mode: reflection/instrumentation, fragile) |

---

## What requires no privilege (works in an ordinary installed APK)

1. Loading guest DEX with `DexClassLoader`/`PathClassLoader` and executing it on host ART.
2. **Overriding `ClassLoader.findLibrary()`** to return arm64 shadow-lib paths (protected API).
3. `dlopen()` of RetroBug's own arm64 `.so` (normal `System.loadLibrary` of RetroBug's lib).
4. Emulating A32/T32 code and managing a translated code cache (same privilege class as ART's JIT,
   which already allocates executable memory in every app process).
5. Guest ELF loading, guest linker, guest relocation, guest `libc`, guest syscall emulation — all
   pure user-space work inside RetroBug's own address space.
6. JNI marshalling, `RegisterNatives` with host stubs (public JNI), guest `JNIEnv` proxy tables.
7. Instrumenting `Context` behaviour for guests through public `ContextWrapper`
   (filesDir, packageInfo-ish, `registerReceiver`, `getSystemService` interception).
8. Extra process slots via `android:process=":guestN"` services declared in RetroBug's manifest.
9. Foreground-service-based long-running guests; WorkManager for deferrable ones.
10. Reading guest APKs the user selects (SAF), extracting `lib/armeabi-v7a/*` into app-private
    storage, and executing the *emulated* code from it (no `execve`, so the Android 10
    "no exec from data dir" rule is not triggered).
11. `dlopen`-ing RetroBug-generated arm64 shadow libs **if placed in the app's native lib dir**;
    placement in `code_cache`/`files` is the open question (OQ-3).

## What requires system integration (therefore out for a normal APK)

1. **Being a native bridge at all**: `-XX:NativeBridge=` is zygote-only (`AndroidRuntime.cpp:1112`);
   the bridge `.so` must be loadable from the **system** namespace by simple name
   (`native_bridge.cc:178-208,260`).
2. `unshare(CLONE_NEWNS)` + bind-mounting `/proc/cpuinfo`
   (`native_bridge_art_interface.cc:103-107`, `native_bridge.cc:306-362`) — needed for faithful
   `/proc/cpuinfo` spoofing.
3. Setting `ro.dalvik.vm.native.bridge` / `ro.product.cpu.abilist32` (readonly `ro.` props) —
   i.e. **declaring the device as 32-bit-capable**, which is what makes a 32-bit APK installable at
   all (`pm install` on an arm64-only device fails with `INSTALL_FAILED_NO_MATCHING_ABIS`).
4. Making the framework *treat the hosted code as a real installed package* (real
   `ApplicationInfo`, `Task`/`ActivityRecord`, `PackageManager` entries, `PermissionManager` grants,
   `NotificationManager`, other apps seeing it) — needs `system_server` cooperation or a modified
   framework.
5. Installing an APK with `PRIMARY_ABI` = armeabi-v7a into a device's app list (needs
   `PackageManagerService` changes = ROM).
6. ART's `getAppEnv` ABI spoofing (`SetupEnvironment`, `native_bridge.cc:422-479`) — bridge-only
   privilege (writes `Build.CPU_ABI` static fields via JNI *from libnativebridge*; possible for
   RetroBug only via hidden-API reflection).
7. Per-app seccomp policy relaxation, or `app_data_file:file execute`/`execmem` changes.
8. `wrap.<package>` `LD_PRELOAD` injection (needs `adb shell`).
9. Anything with `execve`/`fork` of guest binaries (`native_bridge_guest_app_process` is a
   system-image facility; Termux's Android-10 breakage shows app-data exec is denied).
10. `ptrace`-based guest debug / `process_vm_readv`.
11. Pre-granting runtime permissions to guests (needs `pm grant` via adb, or Device Policy).

---

## Open questions / unverified

- **OQ-1 (JNI, §1.2):** Where is `Runtime.nativeLoad` implemented in ART `main`?
  `runtime/native/java_lang_Runtime.cc` is gone and `RegisterRuntimeNativeMethods()` doesn't register
  `Runtime`. Everything else in the chain is verified. *Action: grep `art/runtime/jni/` and libcore's
  `luni/src/main/native/Register.cpp`.*
- **OQ-2 (Native bridge, §1.3):** The task brief's symbol names (`native_bridge_get_trampoline`,
  `dlopen_passthrough`, `native_bridge_code_cache_callbacks`, `native_bridge_unlock`,
  `nativebd`/`IBridge`) are Android 7–10 era; `nativebd` and `IBridge` are absent from
  `frameworks/native` `main`. Need to confirm whether **any** binder-level bridge service survived
  (e.g. for RISC-V/`berberis` on ChromeOS) before writing them off. *Action: search
  `frameworks/native/aidl` and `system/apex` for `IBridge`.*
- **OQ-3 (Privilege, §6.1/1):** Can an app `dlopen()` an arm64 `.so` it wrote into (a)
  `<app_lib_dir>` (`/data/app/…/lib`, labelled `apk_data_file` per
  `system/sepolicy/private/file_contexts:611`), (b) `code_cache`, (c) `files`? Android 10+ denies
  execute-on-app-writable-file; the exact labels/allow rules could **not** be fetched this session
  (`app_domain.te`/`appdomain.te` paths 404'd). **This decides whether shadow libs are generated at
  install time on-device or must be pre-built per-APK on RetroBug's servers.** *Action: fetch
  `system/sepolicy` with the correct filenames, then write a 10-line probe app.*
- **OQ-4 (Berberis reuse, §7.3):** Do the `arm` variants in `native_bridge_support/android_api`
  (`stubs_arm.cpp`, `init_static_tls_arm.S`, `libc.arm.map`, `NATIVE_BRIDGE_PRODUCT_PACKAGES_ARM`)
  still **build** for an arm32 guest, and does `libnative_bridge_guest_libc` for arm link? The riscv
  distribution list suggests arm32 guest support may have rotted since Houdini-era removal.
  *Action: attempt `m libnative_bridge_guest_libc` with `TARGET_ARCH=arm` before planning around it.*
- **OQ-5 (Hidden API, §6.3):** Status of `HiddenApiBypass` (and `setHiddenApiExemptions`) on Android
  15/16. Primary sources do not state it; empirically test on a Matrix/Cloud device. RetroBug's UI
  plan must degrade gracefully.
- **OQ-6 (`libndk_translation`):** Its licence/source availability (ChromeOS ships it as a prebuilt;
  the anbox feature request and the Droid-NDK-Extractor repo prove existence, not licence) —
  could be a *reference implementation* of ARM32→x86_64 DBT if open. **Unverified.**
- **OQ-7 (Syscall details, §5.3/§5.4):** Not verified against primary sources this session:
  `struct compat_sigaction`/`compat_siginfo_t`/`compat_ucontext` field-level layouts, AArch32
  `va_list` exact byte layout for `-mfloat-abi=softfp`, whether Android arm bionic ever emits
  `kuser` calls, `AT_PLATFORM` value for AArch32 tasks on arm64, the exact per-release app seccomp
  deny-list, AArch64 `prlimit64`/`sync_file_range` numbers, and whether RetroBug's host may call
  `clone3` under the app filter.
- **OQ-8 (16 KB pages, §5.5):** Can RetroBug map 4 KB-aligned 32-bit ELFs on a 16 KB-page host
  (using `mmap` over-allocation + `PT_LOAD` slack), and do Android 16 16 KB devices even allow
  `mmap` of `apk_data_file` regions that are not 16 KB-aligned? `linker_phdr_16kib_compat.cpp` and
  the *"Propagate 16kB app compat flag"* commit in `native_bridge_support` imply upstream hit this;
  read both.
- **OQ-9 (x18 / shadow call stack, §3.1):** Does Android's arm64 build reserve `x18` (SCS) and/or use
  PAC such that RetroBug's translator cannot use it? Check `build/soong/cc/config/arm64_device.go`.
- **OQ-10 (§7.1):** Actual minSdk/targetSdk/ABI distribution of the *installable-today* legacy
  library. No primary dataset found; run §8's measurement.
