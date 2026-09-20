# RetroBug — Guest library inventory: what the NDK's 32-bit stubs can and cannot generate

Closes the [`docs/20`](20-implementation-plan.md) §5 M0 row **"Audio proxy strategy locked"** ("one stub-lib sweep
confirms §1's finding for every remaining guest library, so no interface-vtable API is ever assumed
symbol-generatable"). It settles that row and **partially falsifies the conclusion** drawn in `docs/20` §1 — see §6.

Evidence tags are `docs/10`'s header convention (its legend, above §1): **[1L]** read/ran it myself here,
**[1A]** sub-agent read a primary source, **[INFER]** reasoned from documented behaviour, **[UNVERIFIED]**
load-bearing and unchecked.

## 1. The question, and what would have made this row a lie

The M0 row is not asking "which libraries exist". It is asking a narrower and more dangerous question: **for which
guest libraries is an export-symbol list sufficient input to a 32-bit forwarding stub?**

The trap is that "it has a `.dynsym` in the NDK stub dir" and "a generated stub can serve it" are different
properties, and the first looks like proof of the second. `docs/20` §1's correction 1 found this once —
`libOpenSLES.so` exports 3 functions, and the other 50+ symbols are `SL_IID_*` *data*, and the API itself lives
behind `GetInterface`-handed vtables — and that finding was then generalised in §1 to "audio is the only category
where the generator approach breaks down". §6 shows that generalisation does not survive the sweep. An engineer
reading §1 alone would classify `libandroid`, `libmediandk`, `libvulkan`, `libz` and `libdl` as "generate it" and
discover at C12/C13 integration time that they are not.

**A library is symbol-generatable only if every value that crosses the guest/host boundary is either a scalar, or a
pointer to memory whose layout *both* sides already agree on because one side allocated it.** Function pointers,
structs-of-function-pointers the guest dereferences, structs the guest *laying out* inline, and size/version
self-assertion arguments all break it, and only two of those five failure modes are audio.

## 2. How this was obtained

All measurements ran on this machine 2026-09-20. Git Bash; no Python.

```bash
NDK="$LOCALAPPDATA/Android/Sdk/ndk/28.2.13676358"
BIN="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin"
BASE="$NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/arm-linux-androideabi"
INC="$NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include"

# the sweep: API-level dirs are 21..35; the union of .so names across them is 27 libraries
find "$BASE" -maxdepth 2 -name '*.so' | sed 's|.*/||' | sort -u | wc -l        # → 27
ls "$BASE"/34/*.so | sed 's|.*/||'                                             # → 26 in the newest dir
ls "$BASE"/*.a                                                                  # libc.a libm.a libz.a libdl.a
                                                                                # libstdc++.a libc++abi.a
                                                                                # libc++_static.a libc++experimental.a
                                                                                # libcompiler_rt-extras.a
# per-library FUNC / OBJECT / UND counts (the two columns that matter are FUNC and OBJECT)
"$BIN/llvm-readelf.exe" --dyn-syms "$BASE"/34/"$lib" | awk '$4=="FUNC"&&$7!="UND"{f++} $4=="OBJECT"&&$7!="UND"{o++} END{print f,o}'
# API presence range per library
for a in 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35; do [ -f "$BASE/$a/$lib" ] && echo $a; done
# classification evidence: which headers declare a function-pointer member or a callback typedef
grep -nE "^\s+[A-Za-z_]+\s+\(\*" "$INC/SLES/OpenSLES.h" "$INC/OMXAL/OpenMAXAL.h"
grep -cE "^struct (SL|XA)[A-Za-z]*Itf_ \{" "$INC/SLES/OpenSLES.h" "$INC/SLES/OpenSLES_Android.h" "$INC/OMXAL/OpenMAXAL.h"
```

Two facts about the stubs themselves, both load-bearing:

- The exported functions have **2-byte bodies**. `llvm-readelf --dyn-syms` reports `Size 2` for every FUNC in every
  stub, e.g. `libmediandk.so`'s `AMediaFormat_setRect` and `libEGL.so`'s `eglBindAPI`; `docs/research/10` §4.3
  objdumped one and got `bx lr`. **[1L]** So a stub carries *names and arities and nothing else* — it is a
  licence to enumerate, never a source of ABI information. Every layout and calling-convention judgement below
  comes from headers or from measured struct/enum definitions, not from the `.so`.
- Stub bodies being empty also means **`OBJECT` symbols carry no value either**: the 53 `SL_IID_*` in
  `libOpenSLES.so` are size-4 `OBJECT` entries at real-looking addresses, and a generated proxy cannot copy their
  *values* from the stub. **[1L]** See §6 item 3.

## 3. The inventory

Counts are defined, non-weak-symbol totals from `--dyn-syms` at API 21 and API 34 (API 35 differs from 34 only for
`libandroid` 352 and `libbinder_ndk` 148). "Range" is the first and last API-level dir the NDK ships that stub in.

| Library | Range | FUNC 21→34 | OBJECT 21→34 | Class | How I know |
|---|---|---|---|---|---|
| `libOpenSLES.so` | 21→35 | 3→3 | 0→**53** | **vtable/callback** | Exports are exactly `slCreateEngine`, `slQuerySupportedEngineInterfaces`, `slQueryNumSupportedEngineInterfaces` **[1L]** — confirms `docs/20` §1. `SLES/OpenSLES.h:105` `typedef const struct SLObjectItf_ * const * SLObjectItf;` = pointer-to-pointer-to-vtable; 45 `SL*Itf_` interface structs and 296 function-pointer members across `OpenSLES.h`+`OpenSLES_Android.h` **[1L]**. The 53 OBJECT are the `SL_IID_*` interface IDs. `docs/research/03` §4.2 is right that this is "a COM-like object graph", and right that `GetInterface` must return a stable pointer per IID. |
| `libOpenMAXAL.so` | 21→35 | 3→3 | 0→**41** | **vtable/callback** | Same shape: `xaCreateEngine` + 2 queries, 40 `XA*Itf_` structs, 279 fn-ptr members, `OMXAL/OpenMAXAL.h:153` `typedef const struct XAObjectItf_ * const * XAObjectItf`. **[1L]** |
| `libandroid.so` | 21→35 | 177→**330** | 0 | **vtable/callback + data-layout** — *not* symbols-only | The export list is flat and generates fine **[1L]**. The API is not. `android/native_activity.h:127` `typedef struct ANativeActivityCallbacks { void (*onStart)(…); … }` — **15** function pointers, in a struct the *platform* fills and the guest calls through (`:59 struct ANativeActivityCallbacks* callbacks;`). Plus `AChoreographer_frameCallback`/`…64`/`vsyncCallback`/`refreshRateCallback` (`choreographer.h:112,122,130,138`), `ALooper_callbackFunc` (`looper.h:179`), and by-value structs the guest reads: `ASensorEvent` (`sensor.h:612-659`), `ANativeWindow_Buffer` (`native_window.h:95-114`). `docs/20` §1 calls this a "large flat-symbol API"; true of the symbols, false of the surface. See §6. |
| `libmediandk.so` | 21→35 | 90→**161** | 24→**143** | **vtable/callback + by-value struct ABI** | `media/NdkMediaCodec.h:145-150` defines `AMediaCodecOnAsyncNotifyCallback` as a **struct of 4 function pointers**, and `:499` takes it **by value**: `AMediaCodec_setAsyncNotifyCallback(AMediaCodec*, AMediaCodecOnAsyncNotifyCallback callback, void* userdata)`. Four 32-bit words vs four 64-bit words is not the same AAPCS-vs-AArch64 argument expansion, so a generated forwarder cannot pass it through unchanged. **[1L]** Also `AMediaDataSource_setReadData`/`setGetSize`/`setClose`/`setGetAvailableSize` (`NdkMediaDataSource.h:67,75,92,108` — guest-supplied read/close, which the *host* decoder thread then calls), `AImageReader_ImageCallback` (`NdkImageReader.h:283,478`), `AMediaDrmEventListener` (`NdkMediaDrm.h:193,196,199`), `AMediaCodecOnFrameRendered` (`NdkMediaCodec.h:169`). The 143 OBJECT are `AMEDIAFORMAT_KEY_*` strings — data, not functions. **[1L]** |
| `libvulkan.so` | 24→35 | —→233 | 0 | **vtable/callback + proc-address** | 233 flat `vk*` exports **[1L]**, including `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`, which **return `PFN_vkVoidFunction`** — a pointer the guest then calls; a generated stub would hand back a host AArch64 address. `vulkan/vulkan_core.h:2926-2933` `VkAllocationCallbacks { PFN_vkAllocationFunction pfnAllocation; … }` is a struct of 5 guest function pointers threaded through *every* create/destroy call, and thousands of `PFN_vk*` typedefs (`:3913+`) define the calling convention. **[1L]** `docs/research/03`'s "Minimum viable API surface for milestone 1" defers Vulkan past M1, so this is a known-deferred item, not a surprise — but it is not a symbol-generatable item either. |
| `libz.so` | 21→35 | 85→93 | 0 | **data/struct-layout + callback** | `zlib.h:124-143`: `z_stream` contains `alloc_func zalloc`, `free_func zfree`, `voidpf opaque` and a private `struct internal_state *state`. Two independent breakages: the guest supplies allocator callbacks the host zlib invokes, and `zlib.h:1856-1864` shows `deflateInit_`/`inflateInit_`/`inflateInit2_` are macros passing **`ZLIB_VERSION` and `(int)sizeof(z_stream)`** as runtime arguments, which the callee compares against its own `sizeof`. A 32-bit guest's `sizeof(z_stream)` is not the host's, so a pass-through forwarder returns `Z_VERSION_ERROR`. **[1L]** The app also allocates `z_stream` **inline**, so its field offsets must be mirrored, not marshalled. This is the clearest non-audio counterexample in the sweep. |
| `libdl.so` | 21→35 | 8→13 | 0 | **proc-address surface** | `dlopen`/`dlsym`/`dlvsym`/`dlmopen` return pointers the guest calls. Every one must resolve to a **guest-executable trampoline address**, never the host symbol's address; `dlopen` must also gate on C06's namespace semantics (`docs/20` §C06). `link.h`'s `dl_iterate_phdr` takes a callback and exposes the *guest's* `dl_phdr_info`/`Elf32_Phdr` — a 32-bit ELF image the host linker never had. **[1L]** Small symbol count, disproportionate work. |
| `libEGL.so` | 21→35 | 47→73 | 0 | **symbols-only, minus one** | Flat exports **[1L]**, but `EGL/egl.h:145` `EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress (const char *procname);` with `:46 typedef void (*__eglMustCastToProperFunctionPointerType)(void);` and `:120 PFNEGLGETPROCADDRESSPROC`. That one symbol returns a callable pointer and so cannot be forwarded as data; and `docs/research/10` §3 corpus #11 (`gles3jni`) is chosen precisely because it exercises it. Opaque `EGLDisplay`/`EGLContext`/`EGLSurface` are fine — they are words in both directions. |
| `libGLESv1_CM.so` | 21→35 | 278→278 | 0 | **symbols-only** | Flat exports **[1L]**, no function-pointer member or callback typedef anywhere in `GLES/gl.h`/`GLES/glext.h`. Array arguments (`glVertexPointer`, `glLoadMatrixf`) are guest-owned pixel data — marshal, don't mirror. The *semantics* are the hard part, not the ABI, and `docs/research/03` §2 owns that. |
| `libGLESv2.so` | 21→35 | 202→204 | 0 | **symbols-only** | As above; `GLES2/gl2.h` has no fn-ptr members **[1L]**. Shader-source pointers are guest buffers. |
| `libGLESv3.so` | 21→35 | 367→418 | 0 | **symbols-only** | **[1L]**. `docs/research/03` `[C-3-1]` notes GLES3 symbols must also be visible to a `-lGLESv2` link — an export-table question, not an ABI one. |
| `libc.so` | 21→35 | **1094→1476** | **20→28** | **symbols-only, plus data-layout, plus built-not-generated** | Per `docs/20` §C11 this is built from AOSP, not generated — the stub is the *symbol authority*, nothing more. The 28 OBJECT are `stdin`/`stdout`/`stderr` (which are macros over `__sF`, itself an OBJECT: bionic's `FILE` array whose **element layout the guest must reproduce**), `environ`, `tzname`, `daylight`, `timezone`, `optarg`/`optind`/`opterr`/`optreset`/`optopt`, `_ctype_`, `_tolower_tab_`/`_toupper_tab_`, `sys_signame`/`sys_siglist`, `__stack_chk_guard`, `__progname`, `__isthreaded`, `error_*`. **[1L]** A generator keyed on "funcs" silently omits every one. `ucontext_t`/`siginfo_t`/`struct stat` are the 32-bit mirrors `docs/20` §C07 already flags. |
| `libm.so` | 21→35 | 220→284 | 2→2 | **symbols-only** | OBJECT are `signgam` and `__fe_dfl_env` **[1L]** — the same data-symbol caveat as libc, and `fenv_t` is a layout the guest sees. |
| `liblog.so` | 21→35 | 8→18 | 0 | **symbols-only for every legacy guest** | Flat **[1L]**. `android/log.h:233,238` do define `__android_logger_function` and `__android_aborter_function`, but their setters `__android_log_set_logger`/`set_aborter` are `__INTRODUCED_IN(30)` (`:265,297`) — past every 32-bit-era app. **[INFER]** No callback work needed at API ≤29; revisit if the corpus disagrees. |
| `libjnigraphics.so` | 21→35 | **4→39** | 0 | **symbols-only + JNI dependency** | `AndroidBitmap_getInfo`/`getDataSpace`/`lockPixels`/`unlockPixels` at 21; the other 35 are the modern `AImageDecoder*`/`AImageDecoderHeaderInfo*` family **[1L]**. `AndroidBitmapInfo` is plain int data. The real requirement is that the `jobject` handed in is a *host* Bitmap — so this is a C09 dependency, not an ABI hazard. |
| `libaaudio.so` | 26→35 | —→70 | 0 | **callback** | Flat exports and fully opaque `AAudioStream`/`AAudioStreamBuilder` (`aaudio/AAudio.h:898-899`, allocated by the host, `AAudio_createStreamBuilder(AAudioStreamBuilder**)` at `:953`) **[1L]** — no layout to mirror. But `:1408 AAudioStream_dataCallback`, `:1496 AAudioStream_errorCallback`, `:1541 AAudioStream_presentationEndCallback` are guest pointers the **host audio service thread** invokes. `docs/research/03` §4.1 notes AAudio is API 26+ and mostly irrelevant to 32-bit-era code, and includes the shim anyway; the reason it is *not* free is these three. |
| `libamidi.so` | 29→35 | —→14 | 0 | **symbols-only** | 14 flat exports **[1L]**, `amidi/AMidi.h` declares **zero** function-pointer members (verified by grep) — the `AMidiEvent` stream is a plain struct with a union payload. Also the only audio-family library with no callback: MIDI delivery is pull-based off an `ALooper` fd. |
| `libcamera2ndk.so` | 24→35 | —→71 | 0 | **callback** | `camera/NdkCameraCaptureSession.h:62,125,200,217,235,248,260,277,704,741` and `NdkCameraDevice.h:108,125` declare 12+ guest callbacks (`ACameraDevice_StateCallback`, `ACameraManager_CameraAvailabilityCallback`, per-capture result/failed/sequenceEnd), all invoked on camera-service threads. **[1L]** Legacy 32-bit camera users are `libandroid_runtime`/JNI, not this, so it is an M-later item — but it is a callback surface, not a generated proxy. |
| `libbinder_ndk.so` | 29→35 | —→107 | 0 | **vtable/callback** | `android/binder_ibinder.h:188 AIBinder_Class_define(descriptor, onCreate, onDestroy, onTransact)` — the "class object" *is* a hand-built function-pointer table (`:139,148,169`), and `:205 AIBinder_onDump`, `:635 onBinderDied`, `:654 onBinderUnlinked`; `binder_parcel.h:110,124,146,163` four more (`AParcel_stringAllocator` et al.). **[1L]** API 29+ so rare in the corpus, but the AIDL-in-C pattern is definitionally a vtable. |
| `libnativewindow.so` | 26→35 | —→31 | 0 | **symbols-only + data-layout** | Flat **[1L]**. Worth recording a negative: `android/native_window.h:82,88` declare `struct ANativeWindow;` **opaque** — the `android_native_window_t` member-function table (`perform`, `dequeueBuffer`, `queueBuffer`, `setSwapInterval`) is *not* in the NDK header set, so no NDK-compiled guest dereferences it. **[1L]** for the header; **[UNVERIFIED]** whether any shipped guest reached those members via a private header copy — that is a corpus question (§5). `ANativeWindow_Buffer` (`:95-114`, 5 fields + `uint32_t reserved[6]`) *is* a layout to mirror, and `docs/research/03` §3.3's `lock`/`unlockAndPost` path depends on it. |
| `libneuralnetworks.so` | 27→35 | —→69 | 0 | **symbols-only** | 69 flat exports **[1L]**; `android/NeuralNetworks.h` (2492 lines) contains **no** function-pointer member and no callback typedef, by grep **[1L]** — it is an opaque-graph C API (`ANeuralNetworksModel`/`_Compilation`/`_Execution`) with value structs (`ANeuralNetworksOperandType`, `_Result`, `_PerformanceInfo`) that need a 32-bit mirror. Corpus `nn-samples` is API-27-era, i.e. not a legacy 32-bit app. |
| `libicu.so` | 31→35 | —→306 | 0 | **not needed [INFER]** | First ships at API 31, so no `armeabi-v7a`-only APK built before NDK dropped arm32-first could have linked it; `docs/research/10` §3's corpus bottoms out at API 21 minSdk. **[1L]** for the presence range, **[INFER]** for the conclusion — confirm against the corpus `DT_NEEDED` dump (§5). It is flat ICU4C `u_*`/`ucol_*`/`uloc_*` if it ever is needed, with `UChar` (UTF-16) and `UVersionInfo` layout caveats. |
| `libnativehelper.so` | 31→35 | —→6 | 0 | **not needed** | 6 exports: `AFileDescriptor_create`/`getFd`/`setFd`, `JNI_CreateJavaVM`, `JNI_GetDefaultJavaVMInitArgs`, `JNI_GetCreatedJavaVMs` **[1L]**. RetroBug does not create a guest JVM — host ART owns it and `docs/20` §C09 supplies the `JNIEnv*`. First appears API 31. |
| `libsync.so` | 26→35 | —→3 | 0 | **not needed at library level → syscall layer** | 3 exports (`sync_merge`, `sync_file_info`, `sync_file_info_free`) **[1L]**, and that is misleading: `android/sync.h:86` onward defines the actually-used helpers (`sync_wait`, `sync_wait_timeout`, `sync_get_fence_info`) as **`static inline`**, so they compiled *into* the guest and issue `ioctl`s directly. Nothing for a proxy lib to forward; the work belongs to C07. Deprecated header. |
| `libstdc++.so` | 21→35 | 12→12 | 1→1 | **symbols-only (trivial)** | Pre-libc++ legacy shim: `operator new`/`delete` (+`nothrow`, +`[]`) weak, `__cxa_guard_acquire`/`release`/`abort`, `__cxa_pure_virtual`, and OBJECT `_ZSt7nothrow` **[1L]**. All live in guest libc/libc++ anyway. Note the OBJECT again. |
| `libc++.so` | 21→35 | **0** | **0** | **not a stub at all** | **19 bytes**, ASCII, contents `INPUT(-lc++_shared)` — a linker script, which `llvm-readelf` rejects: "The file was not recognized as a valid object file". **[1L]** It contributes **zero** authoritative symbols. See §6 item 2. |
| `libc++_shared.so` | top-level only | 1746 | 756 | **built / redistributed, not generated** | Not in an API dir and not a stub: a real 7 126 916-byte `armeabi-v7a` ELF **[1L]**. Its 756 OBJECT symbols include `_ZTV*`/`_ZTI*` — genuine C++ vtables — but they are **resolved entirely inside the guest** (guest code calling guest libc++), which is the distinction that matters: a vtable that never crosses the guest/host boundary is not a hazard, and the `docs/32` §1 measurement already treats this library as decoder-fuzz input rather than as a proxy target. |

Not NDK-stubbed, therefore with **no local symbol authority**: `libandroid_runtime.so`, `libgui.so`, `libutils.so`,
`libui.so`, `libbinder.so`, `libui-gfx`. `docs/20` §C11's "nothing is ever extracted from a device image" invariant
means these get no generated table at all. `docs/research/03` §5.2 is the one place a private library's ABI was read
first-hand (from AOSP `stubs_arm.cc` **[1A]**) and it lands the same verdict: `android_view_MotionEvent_getNativePtr`
returns a host C++ object and `obtainFromNative` takes a `std::unique_ptr` **by value** in a mangled signature, which
"cannot be lowered at all" — so `docs/20` §C13's "MotionEvent is a real 32-bit mirror object, not a pointer to the
host C++ object" is the correct handling and `docs/10` §9 tags it **[1A]**.

## 4. Must be hand-modelled before C12 / C13 can claim anything

Ranked by when it bites, not by size. C13 (`docs/20` §C13) currently claims only audio's tables plus the `MotionEvent`
mirror; these are the additions that sweep forces.

1. **The vtable machinery itself, once.** `docs/research/03` §4.2 item 1 already asks for it for OpenSL ES. The sweep
   finds six more customers (`libmediandk` by-value callback struct, `libcamera2ndk`, `libbinder_ndk`, `libaaudio`,
   `libandroid`'s `ANativeActivityCallbacks`, `libvulkan`), so it is a C11/C08 seam, not an audio-private hack.
   `docs/30` §Step 7b already built exactly this shape for the fake `JNIEnv` — guest memory holding a table of
   host-call slots, with offsets verified against hardware by building a second `JNIEnv` on the tablet — so this is
   a generalisation of something that is green, which is the good news in this document.
2. **Guest→host pointer-returning entry points, as a distinct generated-code case.** `dlsym`, `dlvsym`,
   `eglGetProcAddress`, `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`, `glGetString(GL_EXTENSIONS)`'s sibling
   extension pointers. Five symbols, four libraries, all of them invisible in a FUNC-count sweep.
3. **Host→guest callbacks arriving on threads RetroBug does not own.** `SLCallback`, `AMediaCodecOnAsync*`,
   `AMediaDataSource` read/close, `AAudioStream_dataCallback`, `ACamera*Callback`, `AIBinder_Class_onTransact`,
   `z_stream.zalloc/zfree`. Each is a `rb::abi::CallGuest` from a foreign host thread, and `docs/research/03` §4.2
   item 3's consequence (that thread must be translator-enterable and `AttachCurrentThread`-able, and loses its
   real-time class) applies to *all* of them, not only to audio.
4. **Struct layouts the guest allocates inline and then hands over.** `z_stream`, `AMediaFormat` keys-by-value,
   `ANativeWindow_Buffer`, `ASensorEvent`, `VkAllocationCallbacks` + the `Vk*CreateInfo` chain, `jvalue`,
   `ucontext_t`/`siginfo_t`/`struct stat` from libc, `AndroidBitmapInfo`, the `FILE` element behind `__sF`.
5. **Data symbols, as a category the generator must emit at all.** 53 + 41 interface IDs, 143 `AMEDIAFORMAT_KEY_*`,
   28 libc OBJECT, 2 libm, 1 libstdc++. `SL_IID_*` in particular are *inputs* to the vtable lookup, so item 1 cannot
   even be tested without them.

## 5. What remains unclassified

Deliberately left open rather than guessed. Every one of these needs either the corpus on disk or a header family
not yet read, and the honest state is "needs the same treatment at M1".

- **The private-library set above.** No stub, so no enumeration is possible locally. `docs/research/10`'s
  "Open questions / unverified" item 2 has the F-Droid `abi` scan unrun, so we do not yet know which private symbols a
  real archived `armeabi-v7a` APK references.
- **Which libraries the corpus actually needs.** `docs/research/10` §3 states a hard closure for one row only —
  hello-jni's is `liblog.so, libc.so, libm.so, libdl.so`, "four libraries, which is a milestone-sized job" — and names
  a library for a few others without pretending completeness: `native-audio`/`audio-echo` for `libOpenSLES.so`,
  `hello-gl2` for `libEGL`+`libGLESv2`, `gles3jni` for `eglGetProcAddress`, `bitmap-plasma`/`native-activity` for the
  `libandroid`/`android_app` 32-bit struct layout, `hello-vulkan` for `libvulkan`. **The third-party entry (§3 row 15,
  the F-Droid app) states no library need at all** — so the only non-NDK-sample corpus member contributes no evidence
  either way, and the sweep's "plausibly needed" set is broader than anything measured. Not padded: the corpus has not
  been downloaded on this machine.
- **`android_app` / `android_native_app_glue` — outside this sweep's enumeration, but settled by other means.**
  `docs/research/10` §2 tier T3 and §3 row 6 both make the `android_app` struct layout a first-order M2 risk, and
  `docs/research/03` §5.1 describes the `onInputEvent(struct android_app*, AInputEvent*)` path. There is **no stub for
  it because there is no shared library**: games calling `android_main` link `libnative_app_glue.a` *statically*, so
  the struct layout is baked into guest machine code and RetroBug must emit the official 32-bit layout rather than
  redefine it (`docs/research/03` §3.2, **[1A]**). Verified locally against r28.2 **[1L]**: the header is at
  `$NDK/sources/android/native_app_glue/android_native_app_glue.h` — **not** under `sysroot/usr/include/`, which is
  why a sysroot-tree sweep, however thorough, cannot see this class of guest requirement at all. `:89-99` gives
  `struct android_poll_source { int32_t id; struct android_app* app; void (*process)(…); }` and `:110,116,122` give
  `struct android_app { … void (*onAppCmd)(…); int32_t (*onInputEvent)(…); … }` with `ANativeActivity*`,
  `ALooper*`, `AInputQueue*`, `ANativeWindow*`, `ARect` and two embedded `android_poll_source` members. The three
  function pointers are **guest-to-guest** (both producer and consumer are guest code, since the glue runs on the
  emulated CPU), so they are a layout constraint, not a trampoline constraint — the same distinction as
  `libc++_shared`'s vtables. What *is* a crossing hazard on this path is that RetroBug's `libandroid.so` must return
  guest-valid `android_poll_source*` data pointers out of `ALooper_pollOnce` and honour `LOOPER_ID_MAIN`/
  `LOOPER_ID_INPUT` (`docs/research/03` §5.1).
- **The rest of `libandroid.so`'s header families**: `surface_control.h`, `surface_texture.h`, `window.h`,
  `hardware_buffer.h` (checked: no fn ptrs **[1L]**), `imagedecoder.h`, `sharedmem.h`, `trace.h`, `thermal.h`,
  `font.h`, `data_space.h`, `configuration.h`, `crash_detail.h`, `permission_manager.h`, `multinetwork.h`.
  `libandroid.so` is 330 exports across ~30 headers; this document verified eight of them. Five carry a callback or
  layout constraint (`native_activity.h`, `choreographer.h`, `looper.h`, `sensor.h`, `native_window.h`), and three grep
  clean (`asset_manager.h`, `hardware_buffer.h`, and `input.h` for function pointers — its hazard is the opaque
  `AInputEvent`, not a table). **[1L]** for all eight. The prior on the unchecked remainder is not optimistic.
  `AChoreographer` and `ASurfaceControl` are argued out of scope by
  `docs/research/03` §3.3 on API-level grounds **[INFER]**.
- **Whether any shipped guest touches the private `ANativeWindow` function table.** See the `libnativewindow` row.
  **[UNVERIFIED]**, and a corpus `llvm-objdump` question, not a header question.
- **Nothing here was executed.** Every number in §3 is `llvm-readelf`/grep output on an NDK stub or NDK header
  **[1L]**. Not one classification has been confirmed by loading a generated stub under the emulator and calling
  through it — that is M1's work, and §6's rule exists precisely because this document cannot close that gap.
  In particular the `z_stream` sizeof-argument failure (§3, §4 item 1) is a header reading, not a crash I have seen.

## 6. The finding, and the two places it contradicts `docs/20`

`docs/20` §1's *measurements* are correct and I re-confirmed every one: `libOpenSLES.so` and `libOpenMAXAL.so` do
export 3 functions each, `libc.so` does have 1094 functions at API 21, and the 21→24→29→33→35 growth in the API
table is real. Its *generalisation* is not, and `docs/20` §C13 already contradicts it internally.

1. **§1's "audio is the *only* category where the generator approach breaks down" is false**, for `armeabi-v7a`
   guests: `libandroid`, `libmediandk`, `libvulkan`, `libz`, `libdl`, `libEGL` (one symbol), `libaaudio`,
   `libcamera2ndk`, `libbinder_ndk` each break it in a way the sweep verified from headers **[1L]**. `docs/20` §C13
   already agrees with this document and not with §1 — it says C13 covers "**`rb::audio` / `rb::input`**", hand-modelled
   interface tables *and* the `MotionEvent` mirror — so this is a §1-vs-§C13 contradiction inside one file, and §1's
   clause is the one that should go. The correct discriminator is §1's opening sentence of this section: not
   "audio vs not-audio", but whether any value crossing the boundary is a guest-callable pointer or a layout both
   sides must agree on. Consequence for the §8 risk R-4: it was revised upward for audio alone and should now
   cover graphics (`eglGetProcAddress`), media (`AMediaCodec`'s by-value callback struct), and `libz` too.
2. **§1's "Authoritative 32-bit guest ABI symbol lists … plus `libc++`" overstates the authority.** `libc++.so` in
   those directories is a 19-byte `INPUT(-lc++_shared)` linker script with no `.dynsym` **[1L]**, so the C++ ABI
   surface — the one a corpus built with `APP_STL := c++_shared` depends on (`docs/research/10` §4.2) — has **no
   stub table at all** and must come from `libc++_shared.so`'s own `.dynsym` or from AOSP, per §C11's
   built-from-source bucket. Related, and the finding with the widest blast radius: a generator that enumerates
   `FUNC` covers `libmediandk` at 161/304 and `libOpenSLES` at 3/56, i.e. **silently emits a half-complete library**.
   `--dyn-syms` must be filtered on `OBJECT` as well, or C11's invariant that the ABI surface is "derived, never
   typed by hand" holds only for functions.

**The rule.** A guest library is *symbol-generatable* only when its exports take and return scalars and opaque
words, and no struct it receives was laid out by the guest under a different pointer width. Everything else —
interface tables, callback pointers the host invokes, proc-address entry points, data symbols, inline-allocated
structs, and size/version self-assertion arguments — is hand-modelled, has a per-entry calling convention that no
stub body can teach us (stub bodies are `bx lr`, 2 bytes), and **must be named by library and by symbol in the C11
build recipe before C12 or C13 claims coverage for it.** "It's in the NDK stub dir" is evidence that a *symbol list*
exists. It is not evidence that a proxy can be generated from it, and it is never, on its own, a reason to schedule
a library as generated work.
