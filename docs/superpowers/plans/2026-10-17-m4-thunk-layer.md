# M4 Thunk Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (inline; the human chose native execution
> for M3 and asked for M4 to run unattended). Steps use checkbox (`- [ ]`) syntax.

**Goal:** a repacked Angry Birds Go reaches its title screen with sound on the 64-bit-only OnePlus, through generic
thunks for libandroid (AAsset*, ANativeWindow*), EGL, GLES2, OpenSL ES and jnigraphics — no per-game profile.

**Architecture:** every library the guest imports from is answered by `HostSlots` (android/jni/host_slots.*), now one
`Thunks` registry: a name → `"ret:args"` signature table per library (generalizing `gl_forward.cpp`'s), one generic
AAPCS32-softfp → AAPCS64 caller with pointer translation (guest addresses are host memory in the biased window, so
a translated pointer is a real host pointer), a 32-bit **handle table** for host pointers the guest only passes back
(EGLDisplay/Context/Surface/Config, AAssetManager*, AAsset*, AAssetDir*, ANativeWindow*), and hand-written shims for
calls a table cannot express (OpenSL ES object/interface vtables and buffer-queue callbacks, `glShaderSource`, the
EGL/GL string queries, `AAsset_getBuffer`). Data imports (`SL_IID_*`) bind to guest cells holding guest copies of the
interface GUIDs. Callbacks into the guest (OpenSL buffer queue) re-enter on the calling host thread (adopted) under
the GGL, exactly as the M3 inbound dispatcher does.

**Tech stack:** C++20 (NDK r28), the M2 GuestProcess/GGL, the M3 JNI bridge, the device's libEGL/libGLESv2/
libOpenSLES/libandroid/libjnigraphics.

**Spec:** `docs/superpowers/specs/2026-10-15-general-32bit-app-support-design.md` (Thunks; Host-memory window
service; Milestones: M4).

## Global Constraints

- Anything unanswered stays a named trap (each import its own trap slot — `GuestProcess::TrapName`).
- Every behaviour gets a row that can fail both ways; Tier I and Tier J agree; device results are the evidence: the
  SM-X710 tablet runs the bridge test app natively as the oracle, the OnePlus is the 64-bit-only acceptance device.
- All existing lanes stay green at every commit, on both devices before the milestone is called done.
- Never replace the tablet's natively installed originals; integrate on a work branch; no merge to `main`, no push
  without the human's explicit OK.

## Review Focus

1. GL client-side arrays: `glVertexAttribPointer`/`glDrawElements` with no buffer bound pass a guest pointer the
   driver reads at draw time; with a buffer bound the "pointer" is an offset and must not be translated.
2. A host pointer that does not fit in 32 bits never reaches the guest raw — always a handle; a guest handle the
   table never issued is a named refusal.
3. OpenSL buffer-queue callbacks arrive on an audio thread ART never attached: adopted, GGL taken, guest callback
   run, no deadlock with a guest thread holding the GGL while it calls `Enqueue`.
4. `AAsset_getBuffer` returns guest memory valid until `AAsset_close`; `AAsset_read` past the end, seek, and length
   match the device.
5. EGL/GL strings (`eglQueryString`, `glGetString`, info logs) are copied into guest memory, never host pointers.

---

### Task 1: Slot space and the generic thunk caller

**Files:** `core/include/rb/abi/abi.h` (`kMaxSlots` 1024 → 4096), `core/runtime/guest_process.cpp` (host-entry
window sized from `kMaxSlots`), `android/jni/launcher.cpp` (its static_asserts), new `android/jni/thunks.{h,cpp}`
(`ThunkCaller::Call(spec, fn, MachineState&, GuestMemory&, HandleTable&, err)`; spec letters: `v` void, `i` int32,
`u` uint32, `f` float, `d` double (softfp: an aligned register pair or 8-byte stack slot), `l` int64 (pair), `p`
guest pointer (0 stays 0), `o` GL pointer-or-offset (translated only when it is guest memory), `h` handle in,
`H` handle out (return), `s` string out (copied to guest memory)), `HandleTable` (host void* ↔ guest u32,
`0xC000'0000 | n`, typed per family), and a device lane `rb_thunk_tests` (android/jni/tests/thunk_test.cpp +
tests/corpus/thunk_fixture.c) built by `tools/run_engine_tests.sh`.
- [ ] Rows (RED first): a slot past 1024 is reachable and returns; doubles and int64 land in aligned pairs and on the
  stack in order (a fixture calls a host function with `(int, double, int, long long, float, double)` and the host
  side checks each); a 64-bit host pointer returned as `H` round-trips through `h`; an unknown handle is refused by
  name; `o` with a small value stays an offset.
- [ ] Commit "Thunks: 4096 host slots, generic AAPCS32 caller, handle table".

### Task 2: EGL and GLES2 forwarding

**Files:** `android/jni/thunks_gl.cpp` (EGL table with handles; GLES2 table — every GLES2 entry point; GLES1 table
moved here from gl_forward.cpp), `android/jni/host_slots.cpp` (Resolve answers `egl*`/`gl*` from the tables;
`eglGetProcAddress` returns guest slot addresses; strings copied).
- [ ] Rows in `rb_thunk_tests` on both tiers (headless pbuffer EGL in the test binary): the guest fixture initialises
  EGL, makes a pbuffer context, compiles a vertex+fragment shader (`glShaderSource` with 2 strings), draws a
  triangle from a client-side array, then from a VBO, `glReadPixels` → the expected colours; `eglQueryString`/
  `glGetString` return guest strings; `glGetShaderInfoLog` of a bad shader names the error.
- [ ] Commit "Thunks: EGL and GLES2 forwarded".

### Task 3: libandroid (AAsset*, ANativeWindow*) and jnigraphics

**Files:** `android/jni/thunks_android.cpp`; bridge test app (`tests/apk/jnibridge`) gains an `assets/` file and rows.
- [ ] `AAssetManager_fromJava(env, guest jobject)` → the bridge's host object → host `AAssetManager*` → handle.
  `AAssetManager_open/openDir`, `AAsset_read/seek/seek64/getLength/getLength64/getRemainingLength/close/getBuffer`
  (copied into a guest block freed at close), `AAssetDir_getNextFileName` (guest string), `AAsset_openFileDescriptor`
  (host fd + start/length — the fd is for passing to OpenSL ES; guest `read()` on it is refused by name until the
  monitor serves host fds).
- [ ] Bridge-app rows `assets` (read, seek, getBuffer hash, openDir listing) and `bitmap` (AndroidBitmap_getInfo/
  lockPixels on a Java Bitmap): native oracle on the tablet, bridged check equal on tablet and OnePlus.
- [ ] Commit "Thunks: libandroid assets and jnigraphics".

### Task 4: OpenSL ES

**Files:** `android/jni/thunks_sles.cpp`.
- [ ] Guest-side object model: each host `SLObjectItf`/interface gets a guest cell `{vtable*}` whose vtable entries
  are host slots for that interface's methods; `self` maps back through the handle table. `SL_IID_*` data imports
  bind to guest cells pointing at guest GUID copies; `GetInterface`/`CreateAudioPlayer` translate guest IID arrays
  to host IIDs by GUID. Data source/sink structs translated (locators: AndroidSimpleBufferQueue, AndroidFD, URI,
  OutputMix; formats: PCM, MIME). Buffer-queue `Enqueue` passes the translated guest pointer (host memory).
  `RegisterCallback`: a host trampoline that adopts the audio thread and calls the guest callback under the GGL.
- [ ] Rows in `rb_thunk_tests`: create engine, output mix, a PCM buffer-queue player; enqueue 3 buffers; the guest
  callback runs 3 times (on the audio thread), `GetState` counts match; volume/play state round-trip.
- [ ] Commit "Thunks: OpenSL ES engine, players and buffer queues".

### Task 5: Angry Birds Go acceptance, docs

- [ ] Repack ABGo on the OnePlus, launch by icon, follow each named trap to its fix (in the owning task's code, each
  with its row), until the title screen draws with sound (screenshot; audio verified by `dumpsys audio` showing the
  app's player active / the buffer-queue callback count in the report).
- [ ] All lanes, Java tests, bridge check on both devices; final whole-branch review on the most capable model; one
  fix pass; HANDOFF/spec/NEXT-AI-HANDOFF updated; commit "M4 accepted: Angry Birds Go title screen with sound".
