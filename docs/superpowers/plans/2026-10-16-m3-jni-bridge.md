# M3 — JNI bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A repackaged game's own Java calls its 32-bit native code and back through a forwarding JNI bridge, so Gumball runs from its own `JNIGLSurface` with no per-game profile.

**Architecture:** Outbound (guest → Java): a forwarding guest `JNIEnv`/`JavaVM` in guest memory whose entries are host slots that translate 32-bit guest handles to real JNI references and call the calling thread's real `JNIEnv`, GGL released. Inbound (Java → guest): generated per-game AArch64 stubs export the guest's `Java_*` symbols; each export (and every guest `RegisterNatives` binding, via a trampoline pool in `librbguest.so`) captures AAPCS64 arguments and calls a dispatcher that marshals them per the method's shorty into an AAPCS32-softfp guest call on the adopted thread. GLES1 is forwarded by host slots, generalizing the launcher's `gl_forward`.

**Tech Stack:** C++20 (NDK r28, arm64), AArch64 assembly (one `.S`), Java 8 (on-device repackager), the existing engine (`core/`: AbiContext host slots, GuestProcess, GGL), Git Bash tooling.

**Spec:** `docs/superpowers/specs/2026-10-15-general-32bit-app-support-design.md` (Stubs; Runtime: JNI bridge; Milestones: M3). Also `HANDOFF.md` (M2 section) and `NEXT-AI-HANDOFF.txt` §4 "M3 (JNI bridge) MUST".

## Global Constraints

- `jobject`/`jclass` are 32-bit guest handles in per-thread local tables (Push/PopLocalFrame around every trampoline) and a global table; `jmethodID`/`jfieldID`/`jclass` are a **bijection** (stable handle per real id).
- Strings/arrays copied into guest memory and back per release mode; guest MUTF-8 sanitized before `NewStringUTF`. Exceptions forwarded. `FindClass` keeps native semantics (no classloader fix-up).
- `GetDirectBufferAddress` goes through the host-memory window service — that service is M4; in M3 it answers only for buffers whose memory is guest memory and refuses by name otherwise.
- Stubs: tiny AArch64 ELF written at repackage time exporting exactly the guest library's `Java_*` symbols and `JNI_OnLoad` (and `ANativeActivity_onCreate` if present — exported, dispatched as a refusal-by-name until M4); lazy ART binding, no eager `FindClass`/`RegisterNatives` by the stub. Argument shapes from the DEX method descriptors, parsed at repackage time into a per-stub shorty table.
- `JNI_OnLoad` of the stub boots the runtime (idempotent), then calls the guest `JNI_OnLoad` **synchronously on the same thread**, returning `JNI_VERSION_1_6` if the guest has none. Guest `RegisterNatives` is forwarded to the real class with host trampolines.
- M3 MUST (NEXT-AI-HANDOFF §4): release the GGL around every call into real Java (the JNIEnv, host-class, dl and log slots hold it in M2); JniEnv tables locked if they grow after setup; define `rb_runtime_process()` during a boot; adopted-thread exit runs `__cxa_thread_finalize` and `__free_dynamic_tls`; the stub derives its APK from its own `dladdr` path (not the hidden ActivityThread API); an API-level check for the on-device Java (`javac --release 8` in `tools/build_launcher_apk.sh`).
- Testing rules: every behaviour gets a row that can fail both ways; Tier I and Tier J agree; device results are the evidence (OnePlus 75ff8cd3 for 64-bit-only behaviour, the SM-X710 R52Y106BECZ tablet as the native 32-bit oracle); all existing lanes stay green at every commit (`RB_SERIAL=<serial> bash tools/run_engine_tests.sh arm64-v8a`, `bash tools/run_java_tests.sh`).
- Every problem arrives as a named report line (RBRUNTIME), never a silent wrong value.
- The tablet's natively installed 32-bit originals (ABGo 1.8.7, Gumball Lite 1.1) are never replaced; installs on the OnePlus need the human's tap; never bypass security prompts.

## Review Focus

1. Re-entrancy: guest → Java (GGL released) → Java calls a native → guest again on the same thread, and on another thread meanwhile. Expected: no deadlock, nested guest call runs on the same adopted GuestThread (nested CallGuest restores state), results correct. Test: Task 6 `callbacks` row (Java callback that calls a native that calls Java again, 3 deep) and Task 4 `reentrant_call_through_java` (fake Java).
2. A native first called on an ART thread that never entered the guest (GLThread, a `new Thread()`), then the thread exits. Expected: adopted on first call, its local frame popped, retired at exit with no leak. Test: Task 6 `threads` row (8 Java threads × 100 native calls, then join; the bridge's live local refs and adopted-thread count return to baseline).
3. Locals: a guest making >512 local refs in one native call, a guest returning a local, a guest using a deleted or stale handle, or a handle from another thread's frame. Expected: growth works like ART (EnsureLocalCapacity honored), a returned local becomes the caller's real local, a stale/foreign handle is a named refusal — never a host crash. Test: Task 1 rows `stale_local_is_refused_by_name`, `foreign_thread_local_is_refused`, `many_locals_in_one_call`.
4. A pending Java exception while the guest keeps calling env functions, and an exception thrown by guest `ThrowNew` then the native returns. Expected: exactly ART's behaviour (the exception propagates to the Java caller; only exception-safe entries are allowed while pending — others are named, not crashed). Test: Task 2 `pending_exception_rules`, Task 6 `exceptions` row.
5. Strings that are not plain ASCII: embedded NUL, supplementary characters (emoji), invalid MUTF-8 from the guest. Expected: Java→guest gives MUTF-8 exactly as ART's `GetStringUTFChars`; guest invalid bytes are sanitized (as ART CheckJNI would reject) and named, not passed to `NewStringUTF`. Test: Task 2 `mutf8_round_trips` and Task 6 `strings` row.

---

## File Structure

- `core/include/rb/abi/host_java.h` — `HostJava`: the abstract real-Java interface the bridge calls (one virtual per JNI operation the bridge forwards, in host types `HostRef`/`HostId`). Lets core tests use a fake.
- `core/abi/jni_forward.{h→core/include/rb/abi/jni_forward.h, cpp}` — `ForwardingJni`: guest `JNIEnv` + `JavaVM` tables in guest memory, slots, handle tables, marshalling. Task 1 (refs, ids, calls, fields), Task 2 (strings, arrays, exceptions, frames, monitors, RegisterNatives, VM).
- `core/tests/jni_forward_test.cpp` + `core/tests/fake_java.{h,cpp}` — lane `rb_jni_forward_tests` with an in-memory fake Java (classes, methods, fields, strings, arrays, exceptions).
- `tests/corpus/jni_bridge_fixture.c` — the 32-bit guest fixture exercised by the core lane and, natively, by the test APK (Task 6).
- `android/jni/trampolines.S` + `android/jni/inbound.{h,cpp}` — AArch64 capture trampolines (pool for RegisterNatives; the stub-export entry `rb_stub_dispatch`) and the inbound dispatcher (AAPCS64 capture → shorty marshalling → guest call). Task 3.
- `android/jni/art_java.{h,cpp}` — `ArtJava : HostJava` over the real `JNIEnv` of the calling thread (`JavaVM::GetEnv`). Task 4.
- `android/jni/runtime_boot.cpp`, `android/jni/stub.cpp` (kept only as the prebuilt fallback), `android/jni/host_slots.cpp` — wiring. Task 4, Task 7.
- `android/java/com/retrobug/launcher/StubElfWriter.java`, `NativeMethods.java` (DEX native-method descriptors), `ApkRepackager.java` — generated stubs. Task 5.
- `tests/apk/jnibridge/` (Java + manifest) + `tools/build_bridge_apk.sh`, `tools/run_bridge_oracle.sh`, `tools/run_bridge_check.sh`, `tests/corpus/oracle-jnibridge.txt` — Task 6.
- `android/jni/gl_forward.{h,cpp}` (moved under the runtime's host slots) — Task 7.

Slot map after M3 (`AbiContext::kMaxSlots` raised 512 → 1024): 0..232 the JniEnv host-class model (launcher only), 256..319 GL (launcher) / GLES1 (runtime, Task 7), 320..335 dl, 336..635 log, **640..872 forwarding JNIEnv (index i at 640 + i), 880..887 forwarding JavaVM (invoke-interface index i at 880 + i)**.

---

### Task 1: Forwarding JNIEnv — handles, ids, class/method/field lookup, calls and fields

**Files:**
- Create: `core/include/rb/abi/host_java.h`, `core/include/rb/abi/jni_forward.h`, `core/abi/jni_forward.cpp`, `core/tests/fake_java.h`, `core/tests/fake_java.cpp`, `core/tests/jni_forward_test.cpp`, `tests/corpus/jni_bridge_fixture.c`
- Modify: `core/include/rb/abi/abi.h` (`kMaxSlots = 1024`), `core/CMakeLists.txt` (`jni_forward.cpp` in `rb_abi`), `core/tests/CMakeLists.txt` (lane `rb_jni_forward_tests`), `tools/run_engine_tests.sh` (build `libjnibridge_fixture.so` like `libpthread_fixture.so`, push to `/data/local/tmp/`, add the lane to `SUITES`)

**Interfaces:**
- Produces:
  - `using HostRef = std::uintptr_t;` (an opaque real reference; 0 = null), `using HostId = std::uintptr_t;` (a real jmethodID/jfieldID).
  - `class HostJava` (pure virtual, all called **without** the GGL): `FindClass(const std::string& name) -> HostRef`, `GetObjectClass(HostRef) -> HostRef`, `GetSuperclass`, `IsAssignableFrom(HostRef, HostRef) -> bool`, `IsInstanceOf`, `IsSameObject`, `GetMethodID(HostRef cls, name, sig, bool is_static) -> HostId`, `GetFieldID(cls, name, sig, bool is_static) -> HostId`, `Call(char ret_kind /*V Z B C S I J F D L*/, bool is_static, bool nonvirtual, HostRef obj_or_cls, HostRef cls_for_nonvirtual, HostId m, const std::vector<jvalue_bits>& args) -> std::uint64_t` (reference results as HostRef bits), `NewObject(cls, m, args) -> HostRef`, `AllocObject(cls) -> HostRef`, `GetField(char kind, bool is_static, HostRef, HostId) -> std::uint64_t`, `SetField(char kind, bool is_static, HostRef, HostId, std::uint64_t)`, `NewLocalRef/DeleteLocalRef/NewGlobalRef/DeleteGlobalRef/NewWeakGlobalRef/DeleteWeakGlobalRef(HostRef)`, `PushLocalFrame(int) -> int`, `PopLocalFrame(HostRef) -> HostRef`, `ExceptionCheck() -> bool` (Task 2 adds the rest). `struct jvalue_bits { std::uint64_t bits; }`.
  - `class ForwardingJni` (`core/include/rb/abi/jni_forward.h`): `ForwardingJni(mem::GuestMemory&, AbiContext&, GuestAddr where, HostJava&, std::string* err)`; `GuestAddr env() const` (guest `JNIEnv*`, same value on every thread — per-thread state is keyed by the host thread); `GuestAddr vm() const` (Task 2); `bool ok() const`; `static constexpr std::uint32_t kEnvSlotBase = 640, kVmSlotBase = 880;`; `GuestAddr ToGuestLocal(HostRef)` / `HostRef FromGuest(GuestAddr, std::string* err)` (used by the inbound dispatcher, Task 3); `void PushFrame()` / `HostRef PopFrame(GuestAddr result)` (one per inbound call); counters `live_locals()`, `live_globals()`, `refusals()`.
  - Handle encoding (guest-visible, never dereferenced by a correct guest): locals are `kLocalBase (0xF000'0000) + 4·n` per thread table, globals `kGlobalBase (0xE000'0000) + 4·n`, weak globals `kWeakBase (0xD800'0000) + 4·n`, method/field ids `kIdBase (0xD000'0000) + 4·n` (bijection map both ways). All above any 4 GiB guest data the bridge hands out, page-unmapped, so a deref faults by name. A local's table is per host thread; its frame stack mirrors Push/PopLocalFrame.
  - `tests/corpus/jni_bridge_fixture.c` exports guest functions the test calls with `(JNIEnv*, ...)`: `fx_find_and_call(env)` (FindClass `rb/Fixture`, GetStaticMethodID `twice(I)I`, CallStaticIntMethod 21 → 42), `fx_fields(env, obj)` (Get/SetIntField, Get/SetDoubleField, static object field), `fx_locals(env, n)` (makes n locals, deletes half, returns count still valid), `fx_virtual_vs_nonvirtual(env, obj)`, `fx_new_object(env)`. Built armeabi-v7a `-mthumb -mfloat-abi=softfp` like the pthread fixture (both must be callable natively by Task 6).

- [ ] **Step 1: Write the failing tests** in `core/tests/jni_forward_test.cpp`, each on both tiers (`for tier : {1, 2}`), booting a `GuestProcess` with libc/libm/libz as `guest_process_test.cpp` does, loading `libjnibridge_fixture.so`, constructing `FakeJava` (classes `rb/Fixture` with static `twice(I)I`, instance fields `i:I`, `d:D`, static `s:Ljava/lang/Object;`, a subclass overriding `who()I` 2 vs super 1) and `ForwardingJni` over it:
  - `find_class_and_static_call`: `fx_find_and_call` returns 42; FakeJava saw `FindClass("rb/Fixture")` once and `twice(21)`.
  - `ids_are_a_bijection`: two `GetMethodID` of the same method give the same guest id; different methods different ids; `FakeJava` id ↔ guest id round-trips.
  - `fields_round_trip`: `fx_fields` sets i=7, d=2.5, s=obj; FakeJava's object has them; returns their read-back sum encoding (`7 + (int)(2.5*2)` = 12).
  - `virtual_vs_nonvirtual`: returns `who()` virtual = 2, `CallNonvirtualIntMethod(super)` = 1 → `21`.
  - `many_locals_in_one_call`: `fx_locals(env, 2000)` returns 1000 and `live_locals()` returns to its baseline after the frame pops.
  - `stale_local_is_refused_by_name`: a guest call reusing a deleted local stops with `StopReason::Fault` whose detail contains `"stale local reference"`; process still usable after.
  - `foreign_thread_local_is_refused`: a local made in thread A's frame used from thread B → detail contains `"another thread's local reference"`.
  - `ggl_released_during_java`: FakeJava's `twice` blocks until another host thread has run a guest call (proves the GGL was released in the slot; times out at 5 s with a named failure).
- [ ] **Step 2: Run** `RB_SERIAL=R52Y106BECZ bash tools/run_engine_tests.sh arm64-v8a` — expected: build fails (no `jni_forward.h`), then once headers exist the new lane fails every row.
- [ ] **Step 3: Implement** `host_java.h`, `fake_java.{h,cpp}`, and `ForwardingJni` for: GetVersion (0x10006), FindClass, GetSuperclass, IsAssignableFrom, Throw stubs left to Task 2 (refused by index until then), New/DeleteGlobalRef, DeleteLocalRef, IsSameObject, NewLocalRef, EnsureLocalCapacity, AllocObject, NewObject{,V,A}, GetObjectClass, IsInstanceOf, GetMethodID, Call<T>Method{,V,A}, CallNonvirtual<T>Method{,V,A}, GetFieldID, Get/Set<T>Field, GetStaticMethodID, CallStatic<T>Method{,V,A}, GetStaticFieldID, Get/SetStatic<T>Field, NewWeakGlobalRef/DeleteWeakGlobalRef, GetObjectRefType. Every slot `SlotLock::kReleasesGgl`; handle tables guarded by their own leaf mutexes (per-thread table needs none). Argument decoding (variadic, `va_list` as the guest's stacked-args pointer, `jvalue*` array of 8-byte guest slots) reuses the decoding `JniEnv::CallHost` already does (`ArgForm`), factored into a shared helper `DecodeJniArgs(const MachineState&, const mem::GuestMemory&, ArgForm, const std::string& sig, unsigned first_arg, std::vector<std::uint64_t>* out, std::string* err)` in `core/abi/jni_args.{h,cpp}` used by both. Unimplemented indices: refused by name `"JNIEnv entry <index> (<name>) is not forwarded in M3"`.
- [ ] **Step 4: Run** the lanes on the tablet — expected: `rb_jni_forward_tests` N checks, 0 failures; all other lanes unchanged.
- [ ] **Step 5: Commit** `git commit -m "ABI: forwarding JNIEnv — handles, ids, calls and fields"`.

### Task 2: Forwarding JNIEnv — strings, arrays, exceptions, frames, monitors, RegisterNatives, JavaVM

**Files:**
- Modify: `core/include/rb/abi/host_java.h`, `core/include/rb/abi/jni_forward.h`, `core/abi/jni_forward.cpp`, `core/tests/fake_java.{h,cpp}`, `core/tests/jni_forward_test.cpp`, `tests/corpus/jni_bridge_fixture.c`

**Interfaces:**
- Consumes: Task 1's `HostJava`, `ForwardingJni`, handle encoding.
- Produces:
  - `HostJava` additions: `NewStringUTF(const std::string& mutf8) -> HostRef`, `GetStringUTF(HostRef) -> std::string` (MUTF-8), `GetStringChars(HostRef) -> std::u16string`, `NewString(const std::u16string&) -> HostRef`, `NewArray(char kind, int len, HostRef elem_cls) -> HostRef`, `GetArrayLength(HostRef) -> int`, `GetArrayRegion(char kind, HostRef, int start, int len, void* out) -> bool`, `SetArrayRegion(...)`, `GetObjectArrayElement/SetObjectArrayElement`, `Throw(HostRef) -> int`, `ThrowNew(HostRef cls, const std::string&) -> int`, `ExceptionOccurred() -> HostRef`, `ExceptionDescribe()`, `ExceptionClear()`, `FatalError(const std::string&)`, `MonitorEnter/MonitorExit(HostRef) -> int`, `RegisterNatives(HostRef cls, const std::vector<NativeBinding>&) -> int` with `struct NativeBinding { std::string name, sig; void* host_fn; }`, `UnregisterNatives(HostRef) -> int`, `NewDirectByteBuffer(void* host_addr, std::int64_t cap) -> HostRef`, `GetDirectBufferAddress(HostRef) -> void*`, `GetDirectBufferCapacity(HostRef) -> std::int64_t`.
  - `ForwardingJni::set_native_binder(std::function<void*(GuestAddr guest_fn, const std::string& sig, std::string* err)>)` — Task 4 plugs the trampoline pool in; until then RegisterNatives refuses by name `"no native binder"`.
  - Guest `JavaVM` table (invoke interface: 3 reserved, DestroyJavaVM, AttachCurrentThread, DetachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon) at `vm()`; GetEnv answers `env()` with JNI_OK for versions ≤ 0x10006, `JNI_EVERSION` above; Attach* return env (the host thread is already attached by ART or the runtime attaches it in Task 4 through `HostJava::AttachCurrentThread()`).
  - Release-mode rules: `Get<T>ArrayElements`/`GetStringUTFChars`/`GetStringChars`/`GetPrimitiveArrayCritical`/`GetStringCritical` return guest-arena copies (isCopy = JNI_TRUE); `Release*` with mode 0 copies back and frees, `JNI_COMMIT` (1) copies back and keeps, `JNI_ABORT` (2) frees without copy-back. Arena: guest memory `MapFree`d in 1 MiB chunks, freed on release.
- [ ] **Step 1: Write the failing tests** (both tiers): `mutf8_round_trips` (Java string `"a\0b😀é"` → guest `GetStringUTFChars` bytes equal ART's MUTF-8 `61 C0 80 62 ED A0 BD ED B8 80 C3 A9`; guest bytes back through `NewStringUTF` equal; invalid guest bytes `FF 41` → the string the fake receives is `"�A"` and a note names the sanitizing), `string_lengths_and_regions` (GetStringLength/UTFLength/Region/UTFRegion), `arrays_every_kind` (New/Get/Set/Region for Z B C S I J F D and object arrays; release modes 0/1/2 observably different on the fake's array), `pending_exception_rules` (after `ThrowNew`, `ExceptionCheck` true, `ExceptionOccurred` gives a handle of the thrown class, a non-exception-safe call like `CallIntMethod` is refused with `"while an exception is pending"`, `ExceptionClear` then calls work), `monitor_pairs`, `register_natives_reaches_the_binder` (fake binder records guest fn + sig; the fake's class has the binding), `java_vm_get_env` (GetEnv 0x10006 → JNI_OK and env; 0x10008 → -3 JNI_EVERSION), `direct_buffer_over_guest_memory` (`NewDirectByteBuffer(guest ptr)` gives the fake the host address of that guest memory; `GetDirectBufferAddress` of it returns the guest pointer; of a host-only buffer → refusal containing `"host-memory window service (M4)"`).
- [ ] **Step 2: Run** the tablet lanes — expected: new rows fail.
- [ ] **Step 3: Implement** the remaining JNIEnv entries (all 233 indices are then either forwarded or named refusals; list the refused ones in a comment block — expected: none of the standard JNI 1.6 set) and the JavaVM table.
- [ ] **Step 4: Run** tablet lanes — all green; then `RB_SERIAL=75ff8cd3` — all green.
- [ ] **Step 5: Commit** `git commit -m "ABI: forwarding JNIEnv — strings, arrays, exceptions, RegisterNatives, JavaVM"`.

### Task 3: Inbound trampolines and dispatcher (Java → guest)

**Files:**
- Create: `android/jni/trampolines.S`, `android/jni/inbound.h`, `android/jni/inbound.cpp`, `android/jni/tests/inbound_test.cpp` (device-native arm64 lane `rb_inbound_tests`, built and run by `tools/run_engine_tests.sh` with the other suites)
- Modify: `android/CMakeLists.txt` (or the launcher's CMake target for `rbguest`) to assemble `trampolines.S`.

**Interfaces:**
- Consumes: `ForwardingJni::{PushFrame, PopFrame, ToGuestLocal, FromGuest, env}` (Tasks 1–2), `GuestProcess` (adoption happens in `AbiContext::ThreadForThisHost`).
- Produces:
  - `struct InboundTarget { GuestAddr fn; std::string shorty; /* JNI shorty: return then args after env/obj, e.g. "IJLF" */ bool is_static; std::string name; };`
  - `class Inbound` (`android/jni/inbound.h`): `explicit Inbound(GuestProcess&, ForwardingJni&)`; `void* BindPool(const InboundTarget&, std::string* err)` → a host function pointer from a pool of `kPoolSize = 4096` trampolines (refusal `"inbound trampoline pool exhausted (4096)"`); `std::uint64_t Dispatch(const InboundTarget&, const CapturedArgs&)`.
  - `struct CapturedArgs { std::uint64_t x[8]; double d[8]; const std::uint64_t* stack; };` filled by the assembly capture routine `rb_inbound_capture` (saves x0–x7, d0–d7, sp-relative stack pointer, then calls `rb_inbound_entry(std::uint32_t index, CapturedArgs*)`; result returned in x0 and duplicated into d0 for F/D).
  - `extern "C" std::uint64_t rb_stub_dispatch(const void* stub_tag, std::uint32_t export_index, CapturedArgs*)` — the entry generated stubs reach (Task 5); `stub_tag` points at the stub's descriptor table (`RbStubTable`: magic `'RBST'`, version 1, library name, count, entries `{name offset, shorty offset, flags}`).
  - Marshalling (AAPCS64 → guest AAPCS32 softfp, per shorty): JNIEnv* → `fwd.env()`; jobject/jclass/array/string (L) → `ToGuestLocal` in the frame pushed for this call; Z B C S I → 32-bit (sign/zero-extended per kind); J → 64-bit (guest even/odd pair/stack per AAPCS32, which `abi::Shorty`/`PlaceArgs` already implements); F → float bits from the next d register (or stack slot past 8), D → double bits likewise. Return: V; I-family; J; F/D as float/double bits put in x0 and d0; L → `PopFrame(result)` gives a real local. A pending guest fault → the dispatcher throws a `java.lang.Error` (via `HostJava::ThrowNew`) whose message is the named fault, and returns 0.
- [ ] **Step 1: Write the failing tests** `inbound_test.cpp` (device arm64, real `GuestProcess` + `FakeJava` + `ForwardingJni`, fixture functions added to `jni_bridge_fixture.c`: `Java_rb_Fixture_mix(JNIEnv*, jclass, jint a, jlong b, jfloat c, jdouble d, jint e, jlong f, jfloat g, jdouble h, jint i, jint j)` returning a deterministic jlong mix, `..._echoObject(env, cls, jobject o)` returning `o`, `..._fails(env, cls)` dereferencing 0): call the `BindPool` pointers as real C function pointers with those AAPCS64 signatures: `mix` equals the value the same C code computes natively on the host (compile `mix_reference` into the test), `echoObject` returns a real local equal (`IsSameObject`) to the argument and `live_locals()` returns to baseline, `fails` leaves a pending exception whose message contains `"guest memory read at 0x0"`; both tiers (`set_executor`).
- [ ] **Step 2: Run** — fails (no `inbound.h`).
- [ ] **Step 3: Implement** `trampolines.S` (pool: each entry `mov w9, #i; b rb_inbound_capture`, 4096 entries, 8 bytes each; `rb_inbound_capture` as above), `inbound.cpp`.
- [ ] **Step 4: Run** tablet and OnePlus lanes — green.
- [ ] **Step 5: Commit** `git commit -m "Runtime: inbound trampolines and the Java-to-guest dispatcher"`.

### Task 4: Real-ART wiring in the in-app runtime

**Files:**
- Create: `android/jni/art_java.h`, `android/jni/art_java.cpp`
- Modify: `android/jni/runtime_boot.{h,cpp}`, `android/jni/host_slots.cpp` (dl/log slots → `kReleasesGgl` where they call into the host and touch no guest state across the release), `core/runtime/guest_process.cpp` (adopted-thread exit: `__cxa_thread_finalize` then `__free_dynamic_tls` via `CallOn`, before `pthread_key_clean_all`, matching bionic's `pthread_exit` order), `core/tests/guest_thread_test.cpp`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces:
  - `class ArtJava : public HostJava` — `explicit ArtJava(JavaVM*)`; each method gets the calling thread's `JNIEnv*` with `vm->GetEnv` (attaching with `AttachCurrentThread` if needed and recording it for detach at thread exit); `HostRef` is the `jobject` bits; ids are the real `jmethodID`/`jfieldID`.
  - `rb_runtime_boot(JavaVM* vm, const char* apk, const char* lib)` now: creates `ArtJava` + `ForwardingJni` + `Inbound` with the scope (once per process), passes `(fwd.vm(), 0)` to the guest `JNI_OnLoad`, and sets the native binder to `Inbound::BindPool`. `rb_runtime_process()` takes `g_mu` (documented: callable any time; blocks during a boot).
  - `extern "C" std::uint64_t rb_stub_dispatch(...)` (Task 3) resolves `(stub_tag, export_index)` to the guest export by name in the booted library (boots it first if this stub's `JNI_OnLoad` has not run — ART can call a `Java_*` only after `System.loadLibrary`, so this is a named refusal if it happens).
- [ ] **Step 1: Write the failing tests**: `guest_thread_test` rows `adopted_exit_runs_thread_finalizers` (a fixture `thread_local` C++ object with a destructor, from `tests/corpus/pthread_fixture.c`'s new `pt_tls_dtor_count()`; an adopted thread that touched it runs the destructor once at exit; both tiers; oracle from `tools/run_pthread_oracle.sh`) and `reentrant_call_through_java` in `jni_forward_test.cpp` (FakeJava method that calls back into a bound native on the same host thread: result correct, no deadlock within 5 s).
- [ ] **Step 2: Run** — fail.
- [ ] **Step 3: Implement** the above.
- [ ] **Step 4: Run** lanes on both phones — green; launcher `-e bootcheck angry-birds-go` on the tablet still 258 constructors, `JNI_OnLoad` 0x10006 — now with `vm` non-zero and `JNI_OnLoad: guest JavaVM at 0x…` logged.
- [ ] **Step 5: Commit** `git commit -m "Runtime: real-ART JNI bridge in the in-app runtime"`.

### Task 5: Generated stubs at repackage time

**Files:**
- Create: `android/java/com/retrobug/launcher/StubElfWriter.java`, `android/java/com/retrobug/launcher/NativeMethods.java`, `tests/java/StubElfWriterTest.java`, `tests/java/NativeMethodsTest.java` (follow the layout `tools/run_java_tests.sh` already compiles)
- Modify: `android/java/com/retrobug/launcher/ApkRepackager.java` (write a generated stub per 32-bit library instead of copying `librbstub.so`; keep the copy as the fallback when the library exports no `Java_*` and no `JNI_OnLoad` — then nothing would call it anyway), `android/jni/stub.cpp` (prebuilt fallback: derive the APK from `dladdr` — `<sourceDir>/lib/arm64/<lib>` → `<sourceDir>/base.apk` — removing the ActivityThread call), `tools/build_launcher_apk.sh` (`javac --release 8`)

**Interfaces:**
- Consumes: `rb_stub_dispatch` and `RbStubTable` layout (Task 3), `DexScanner` (existing).
- Produces:
  - `NativeMethods.of(byte[] dex) -> Map<String /*Java_ mangled name*/, String /*JNI descriptor*/>` — every `native` method of every class (`access_flags & 0x100`), with the JNI short name and, for overloaded natives, the long name (`__` + mangled args) exactly as ART looks them up.
  - `static String shortyOf(String descriptor)` — return then params, references → `L`, arrays → `L`.
  - `StubElfWriter.write(String libName, List<Export> exports) -> byte[]` with `record`-like `final class Export { String symbol; String shorty; boolean isStatic; }` (static from the DEX access flags). The ELF: ET_DYN, EM_AARCH64, one PT_LOAD R+X and one PT_LOAD RW, PT_DYNAMIC, DT_NEEDED `librbguest.so`, DT_SONAME = `libName`, `.dynsym`/`.dynstr`/`.hash` (SysV), one `R_AARCH64_GLOB_DAT` against `rb_stub_dispatch` (GOT slot), `JNI_OnLoad` exported too (as export index 0, flag `kStubOnLoad`: the dispatcher calls `rb_runtime_boot` with the APK and library names derived from the stub's own `dladdr` path). The GOT relocation is against `rb_stub_capture`, the assembly entry in `trampolines.S` that saves x0–x7/d0–d7/sp into `CapturedArgs` and calls the C++ `rb_stub_dispatch(x9, w10, &captured)`. Each export's code (16 bytes): `adr x9, RbStubTable; mov w10, #index; ldr x16, <GOT slot>; br x16` — x9/x10/x16 are AAPCS64 scratch registers, so the caller's arguments in x0–x7/d0–d7/stack are untouched.
- [ ] **Step 1: Write the failing tests**: `NativeMethodsTest` over the Gumball and ABGo DEX files already used by the shim tests (`JNIGLSurface.jniClearTextures` → `()V`, `AngryBirdsGoActivity.isDeviceAmazon` → `()Z`, counts 12 and 92 as RBRUNTIME reported); `StubElfWriterTest` parses the produced bytes with a small reader in the test (header, phdrs, dynamic tags, symbols resolve to 16-byte code inside the R+X segment, the GOT relocation names `rb_stub_dispatch`, the table holds every shorty) and `llvm-readelf -d -s --dyn-relocations` output (when the NDK is present) lists exactly the exports.
- [ ] **Step 2: Run** `bash tools/run_java_tests.sh` — fails.
- [ ] **Step 3: Implement** both classes and the repackager change; add `-e stubcheck <name>` to `LauncherActivity` (repack with `install 0`, then `dlopen` each generated stub from the output directory in the launcher's own process with `RTLD_NOW` and `dlsym` every export — reported as `stubcheck: <lib>: N exports resolved, dispatch at 0x…`).
- [ ] **Step 4: Run** Java tests green; on the OnePlus `-e stubcheck base` and `-e stubcheck gumball`: every export resolves; `llvm-readelf` on the pulled stubs matches.
- [ ] **Step 5: Commit** `git commit -m "Repackager: generated AArch64 stubs exporting the guest's Java_* symbols"`.

### Task 6: Bridge test APK with a native oracle

**Files:**
- Create: `tests/apk/jnibridge/AndroidManifest.xml` (package `com.retrobug.jnibridge`, minSdk 19, targetSdk 19 so the tablet runs it natively unshimmed), `tests/apk/jnibridge/java/com/retrobug/jnibridge/BridgeActivity.java`, `tools/build_bridge_apk.sh` (armeabi-v7a `lib/armeabi-v7a/libjnibridge.so` from `tests/corpus/jni_bridge_fixture.c` + its `Java_com_retrobug_jnibridge_*` natives; signs with a throwaway debug key generated by the script), `tools/run_bridge_oracle.sh` (installs natively on R52Y106BECZ, runs, writes `tests/corpus/oracle-jnibridge.txt` with device/libc provenance like the pthread oracle), `tools/run_bridge_check.sh <serial>` (stages the APK in the launcher, `-e repack jnibridge`, human confirms install, launches, compares its logcat `BRIDGE:` lines to the oracle; exit 0 only when every row matches)
- Modify: `tests/corpus/jni_bridge_fixture.c` (the `Java_com_retrobug_jnibridge_*` entry points), `HANDOFF.md` (commands)

**Interfaces:**
- Consumes: everything above.
- Produces: rows printed as `BRIDGE: <row>=<value>` and a final `BRIDGE: done <n> rows`: `strings` (round trip of `"a\0b😀é"` and a 64 KiB string, hash), `arrays` (every primitive kind + object array + release modes), `callbacks` (native → Java → native → Java, 3 deep, result), `exceptions` (Java exception through a native, native ThrowNew caught in Java, message), `globals` (global/weak global survive a GC request; weak cleared after the referent dies), `threads` (8 Java threads × 100 native calls; sum; then `BridgeActivity` asks the runtime for live locals via a `Java_..._liveLocals` native that returns `rb` counters only when bridged — oracle row ignores it), `register_natives` (guest JNI_OnLoad registers `nativeAdd(II)I` on a class, Java calls it), `fields_and_statics`, `overloads` (two natives with the same name, long-name lookup).
- [ ] **Step 1:** Build the APK; run `tools/run_bridge_oracle.sh` on the tablet → the oracle file (commit it).
- [ ] **Step 2:** `tools/run_bridge_check.sh 75ff8cd3` — expected before Tasks 1–5 work end to end: rows differ (this step is run once Tasks 1–5 are in; record the first diff if any as the task's evidence).
- [ ] **Step 3:** Fix what the diff names, in the owning task's code (each fix gets the core-lane row that reproduces it).
- [ ] **Step 4:** `tools/run_bridge_check.sh 75ff8cd3` exit 0 (every row equals the tablet's native value) and the same on the tablet (bridged copy installed under the repacked package name `com.retrobug.jnibridge`, which is not natively installed there — the oracle run uninstalls its native copy first; that app is ours, not one of the human's originals).
- [ ] **Step 5: Commit** `git commit -m "Tests: JNI bridge test APK, native oracle, on-device check"`.

### Task 7: GLES1 in the runtime, Gumball acceptance, docs

**Files:**
- Modify: `android/jni/gl_forward.{h,cpp}` (usable from the runtime: an `EglTarget`-free mode that calls the current thread's GLES1 entry points — the game's `GLSurfaceView` made the context current on its GL thread), `android/jni/host_slots.cpp` (`Resolve` answers `libGLESv1_CM` names via `GlForward::Known`, slots 256..319, `kReleasesGgl`), `HANDOFF.md` (M3 section), the spec (M3 status), this plan's checkboxes, `NEXT-AI-HANDOFF.txt`

**Interfaces:**
- Consumes: Tasks 1–6.
- Produces: no new API.
- [ ] **Step 1: Write the failing check**: on the OnePlus, `-e repack gumball` (human confirms), launch by icon — expected before this task: `UnsatisfiedLinkError` gone (Tasks 4–5) but `glXxx` calls refused by name (`imports nothing answers` lists GL names).
- [ ] **Step 2: Implement** GLES1 slots; per-call refusals name the function.
- [ ] **Step 3: Run** the acceptance on the OnePlus: Gumball launched by its icon reaches `GumballMainMenu` drawn on screen (screenshot `adb exec-out screencap -p`), RBRUNTIME shows no fault, its own `JNIGLSurface` drives `jniInit/jniRun/jniTouch` (a tap on "Karate Chop Challenge" by the human reaches the mini-game). Angry Birds Go launched by its icon gets past `isDeviceAmazon()` and records its new end point (expected: an EGL/GLES2 or OpenSL import refused by name — M4's work).
- [ ] **Step 4: Run** all lanes on both phones, Java tests, `tools/run_bridge_check.sh` on both — green; final whole-branch review on the most capable model; one fix wave.
- [ ] **Step 5: Commit** `git commit -m "M3 accepted: Gumball runs from its own Java through the JNI bridge"`; ask the human about merging to `main`.
