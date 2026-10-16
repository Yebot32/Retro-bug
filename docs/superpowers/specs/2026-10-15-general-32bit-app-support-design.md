# General 32-bit app support — design

Date: 2026-10-15. Status: design agreed with the human (sections 1–3), reviewed by a Plan subagent against
the code; its findings are folded in below (marked *review*). The human delegated further design review.

## Goal

Run arbitrary 32-bit (armeabi / armeabi-v7a) Android apps on 64-bit-only phones (OnePlus CPH2573) with
**no per-game code**: the app's own Java, UI, graphics, sound and touch. First acceptance app: Angry Birds
Go 1.10.1 (`com.rovio.angrybirdsgo`, `libABK.so`, Exient engine, GLES2, EGL, OpenSL ES, pthreads,
AAssetManager, libjnigraphics). Today it runs all 253 constructors and `JNI_OnLoad` inside the launcher.

Success: the human picks a 32-bit APK in RetroBug, confirms one install, and the app runs. After M4,
remaining failures should be "a thunk or syscall is missing", named in a report — never "write a driver".

Out of scope: Google sign-in / Play billing / licence or signature self-checks in repackaged apps (the
signing key changes); apps that generate ARM code at run time (Unity Mono JIT, emulators) — later.

## Why the current design cannot get there

RetroBug runs only the native `.so`. Each app's Java side is faked by hand in `android/jni/launcher.cpp`
(Gumball: 16 transcribed methods), and the guest `JNIEnv` (`core/abi/jni_env.cpp`) knows only classes the
launcher defines. Every app costs a profile. The fix is to run the app's real Java on ART and emulate only
its native code.

## Architecture

### Repackaging (on device, in the RetroBug app)

Input: an APK (plus splits, merged). Output: a re-signed APK that installs normally.

- Keep `classes*.dex`, resources, assets, manifest. **Package name unchanged by default** (*review*: the
  32-bit original cannot be installed on a 64-bit-only phone anyway; renaming breaks provider authorities,
  custom permissions, relative activity names, `getPackageName()` logic and the OBB path). Rename is an
  opt-in option for side-by-side installs.
- Move `lib/armeabi*/libX.so` → `assets/rb32/libX.so` (armeabi-v7a preferred over armeabi).
- Add `lib/arm64-v8a/librbruntime.so` (the engine) and one **generated stub** `lib/arm64-v8a/libX.so` per
  32-bit library.
- Manifest (binary AXML rewriter): bump `targetSdkVersion` only to the platform's install floor; keep
  `extractNativeLibs` consistent with how `.so` files are stored (uncompressed + 16 KiB aligned, or
  extracted).
- Sign v1+v2+v3 with **apksig** (runs on Android). Install through `PackageInstaller` — the human confirms.
- A **static repackage report**: Java_* exports per lib, system imports with no thunk yet, OBB/split
  presence, custom native loaders (ReLinker, `System.load` of an extracted path — these bypass stubs and
  are reported by name), targetSdk change.

### Stubs (*review*: generated, exporting the real symbols)

Each stub is a tiny AArch64 ELF written at repackage time that exports exactly the guest library's
`Java_*` symbols, `JNI_OnLoad`, and `ANativeActivity_onCreate` if present. Each export is a capture
trampoline that loads its symbol index and jumps into `librbruntime.so`'s dispatcher. ART therefore binds
natives lazily, as it would natively — no eager `FindClass`/`RegisterNatives` (class-init ordering
hazards). `JNI_OnLoad` of the stub boots the runtime (idempotent across stubs), loads the guest library
and its bundled dependencies from `assets/rb32` through the `GuestLinker`, runs constructors, then calls
the guest `JNI_OnLoad` **synchronously on the same thread** (so ART's library-classloader rule applies to
the guest's `FindClass`), returning `JNI_VERSION_1_6` if the guest has none. Guest `RegisterNatives` is
forwarded to the real class with host trampolines. Argument shapes come from the DEX method descriptors
(parsed at repackage time into a per-stub shorty table).

### Runtime (`librbruntime.so`)

- **Memory**: one 4 GiB biased window per process (the only form `untrusted_app` allows — `docs/30`
  §Step 8: the largest low window is 1856 MiB). *Review*: `pages_` must become lazily allocated / two-level
  before multi-process apps (today ≈48 MiB committed per window); thunks passing pointers must check the
  range is contiguous host memory (`IsFastmem`) and refuse by name otherwise (16 KiB-page hosts).
- **Global Guest Lock (GGL)** (*review*, first and mandatory): at most one host thread executes guest code
  at a time. Released around blocking syscalls (futex wait, nanosleep, read, poll, …), every host thunk call
  and every JNI call into Java, re-acquired on return. This makes the current single-threaded engine
  (non-atomic exclusive store `jit.cpp` ~746, per-executor exclusive monitors, unlocked page-table
  mutation, singleton monitor state) correct immediately. Truly parallel executors are a later milestone
  gated on: host-atomic CAS for exclusives (both tiers + kuser cmpxchg), a map rwlock with no in-place
  remap of live granules, per-executor invalidation queues, and a shared/bounded code cache.
- **Thread contexts**: per host thread — guest register state, guest stack, executor, monitor thread
  state (tid, `clear_child_tid`, robust list, signal mask), JNI local-ref frame. Shared — memory, fd table,
  mmap allocator, global refs — under locks.
- **Guest threads**: guest `pthread_create` → bionic `clone` → the monitor starts a host thread with a new
  context (TLS from `clone`'s argument); `futex` wait/wake become real (host futex on the guest word, or a
  condition variable keyed by guest address when the page is out of line); `exit` performs
  `clear_child_tid` + wake; guest threads that call JNI are attached to the real VM and detached at exit.
- **Adopted threads** (*review*): ART threads (UI, GLThread, audio callbacks) entering guest code get a
  synthesized guest bionic `pthread_internal_t` + TLS block in the tablet libc's layout, linked into its
  thread list; guest TLS-key destructors run at host thread exit via a host pthread key.
- **JNI bridge**: the guest `JNIEnv`/`JavaVM` tables forward to the calling thread's real `JNIEnv`.
  `jobject`/`jclass` are 32-bit guest handles in per-thread local tables (Push/PopLocalFrame around every
  trampoline) and a global table; `jmethodID`/`jfieldID`/`jclass` are a **bijection** (stable handle per
  real id). Strings/arrays copied into guest memory and back per release mode; guest MUTF-8 sanitized
  before `NewStringUTF`. Exceptions forwarded. `FindClass` keeps native semantics (no classloader fix-up).
  `GetDirectBufferAddress` goes through the host-memory window service.
- **Host-memory window service** (*review*): makes host memory visible inside the guest window — memfd
  aliasing where possible, shadow-and-sync at call boundaries otherwise. Used by direct buffers,
  `glMapBufferRange`, `AAsset_getBuffer`, `ANativeWindow_lock`, `AHardwareBuffer_lock`.
- **Thunks**: per-library signature tables (`"ret:args"`, generalizing `android/jni/gl_forward.cpp`) and one
  generic AAPCS32-softfp → AAPCS64 caller with pointer translation; hand-written shims where a table cannot
  express the call: `glShaderSource` (pointer arrays), client-side vertex arrays (dereferenced at draw
  time), OpenSL ES object/interface vtables, `ANativeActivity` callback structs. Callbacks into the guest
  (OpenSL buffer queues, ALooper fd callbacks, AChoreographer) re-enter on the calling host thread under the
  GGL. `eglGetProcAddress` and `dlsym` return guest thunk addresses. Guest `dlopen` of the app's own
  libraries resolves to `assets/rb32`. Libraries: GLES1/2/3, EGL, libandroid (AAssetManager/AAsset,
  ANativeWindow, AConfiguration, ALooper, AInputQueue, ASensor, AChoreographer), OpenSL ES, libjnigraphics,
  liblog, libdl.
- **Monitor additions**: virtualized `/proc/cpuinfo`, `/proc/self/auxv` (AT_HWCAP: VFP/NEON for ARMv7),
  `/proc/self/maps`; real fd-backed file access within the app's own sandbox (open/read/lseek/close/stat on
  real paths, mmap of files); signals: guest `rt_sigaction` recorded, crash handlers never installed on the
  host; the runtime chains through ART's sigchain if it ever installs its own handlers.
- **Errors**: unchanged rule — anything unanswered is a named trap. The runtime writes its report to logcat
  (`RBRUNTIME`) and to a file in the app's private dir; the RetroBug app shows the last report per app.

### targetSdk compatibility shims (*new*, found in M1)

The install floor forces targetSdk >= 24, which switches on post-API-21 behaviour changes the old Java was never
written for: implicit service intents throw (`Service Intent must be explicit`; Gumball's billing `bindService`
hit this on the OnePlus), background-service limits at 26+, and others. Approach to be designed; candidate is an
injected DEX/Application-level shim set chosen per detected API use at repackage time. The repack report should
list the detected risky calls. Scheduled before M3's acceptance (Gumball needs it to reach its activity).
*DONE 2026-10-16 (branch `targetsdk-shims`)*: a repackage-time plan (`ShimPlan`) adds a provider + shim dex that restores the
original targetSdk in-process, relaxes StrictMode network / file-URI and asks for dangerous permissions; the repack report
lists risky DEX calls. Gumball now passes its first-run billing bind on the OnePlus — see HANDOFF.md.

## Milestones (*review*: threads before the JNI bridge — ART calls natives from UI and GL threads on frame 1)

1. **M1 Repackager + stub boot. DONE 2026-10-15.** AXML rewriter, apksig signing, stub ELF generator (exports + capture
   trampolines), runtime boot in-process (4 GiB biased window, GuestLinker over `assets/rb32`,
   constructors, guest `JNI_OnLoad` with today's env), report file. *Acceptance*: repackaged Gumball and
   Angry Birds Go install on the OnePlus, open their real first activity, and report "constructors ran,
   JNI_OnLoad returned" from inside their own process.
   *Evidence*: both install on the OnePlus (min/target 24) and boot in their own process: Angry Birds Go 258 constructors, `JNI_OnLoad` returned, "startup finished with no fault", then the designed `UnsatisfiedLinkError` at `isDeviceAmazon`; Gumball 24 constructors, headless 1200 frames to the main menu, then a targetSdk-compat crash (implicit billing `bindService`) -> see shims component.
   *Status, plainly*: neither game reaches its first activity yet. Angry Birds Go stops at the expected `UnsatisfiedLinkError` in `onCreate`; Gumball at the targetSdk-compat crash. The 64 MiB Inspect window (not the spec's 4 GiB) remains until M2. The in-process boot reuses the launcher's `Inspect()` (incl. the hand-written Gumball profile, run synchronously in `JNI_OnLoad`); a boot-only mode is due at the start of M2.
2. **M2 Multi-entry core.** *(DONE 2026-10-16, branch `m2-multi-entry-core`: accepted on the tablet and the OnePlus — see HANDOFF.md.)* Thread contexts, GGL, clone/futex/exit, adopted ART threads with synthesized
   guest pthreads. *Tests*: fixture rows (pthread create/join, mutex contention, condvar, TLS keys and
   destructors) on both tiers, expected values from the tablet running the same `.so` natively.
3. **M3 JNI bridge.** *(IN PROGRESS 2026-10-16, branch `m3-jni-bridge`: bridge, stubs and GLES1 slots done; the test APK matches the tablet's native run 10/10; Gumball icon-launch acceptance waits on the OnePlus — see HANDOFF.md.)* Forwarding env/VM, handle tables, trampolines from stub exports, guest
   RegisterNatives. *Acceptance*: Gumball runs from its own `JNIGLSurface` with no profile (GLES1 thunks).
   *Tests*: an instrumented test APK whose Java calls a 32-bit fixture `.so` through the bridge (strings,
   arrays, callbacks, exceptions, globals, threads), expectations from the tablet natively.
4. **M4 Thunk layer.** Generic caller + GLES2/3/EGL, libandroid, OpenSL ES, jnigraphics, host-memory
   window service. *Acceptance*: Angry Birds Go reaches its title screen with sound. Retire per-game
   profiles from the launcher (keep it as a diagnostics tool).
5. **M5 Breadth.** (targetSdk compatibility shims land earlier, before M3 acceptance.) Run the tablet's 31 installed 32-bit apps through the repackager; fix what each names;
   keep a pass/fail table in `docs/`. Bound the code cache per process.
6. **Later.** Parallel executors (see GGL), shared code cache, NativeActivity polish, apps that JIT.

## Testing rules (unchanged from the project)

Every behaviour gets a row that can fail both ways; Tier I and Tier J must agree; device results are the
evidence (OnePlus for 64-bit-only behaviour, the SM-X710 tablet as the native 32-bit oracle). All existing
18 lanes stay green at every commit.
