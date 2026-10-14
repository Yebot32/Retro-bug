# M1: Repackager + stub boot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** RetroBug turns a 32-bit APK into a re-signed APK that installs on a 64-bit-only phone, opens its own first activity, and — from inside the game's own process — loads the 32-bit library through the emulator, runs its constructors and `JNI_OnLoad`, and logs the report.

**Architecture:** The launcher (Java) rewrites the APK on the device: 32-bit `.so` files move to `assets/rb32/`, a prebuilt 64-bit stub is added under each 32-bit library's name next to the engine `librbguest.so`, the binary manifest gets the minimum edits, and apksig signs it with a key kept in AndroidKeyStore. When the game calls `System.loadLibrary`, the stub's `JNI_OnLoad` finds its own APK and hands the guest bytes to the engine's existing `Inspect()` (headless, Tier J). Java_* exports and the real JNI bridge are M3; M1's games stop at their first native call, by name.

**Tech Stack:** Java 8 source (javac → d8, min-api 26), apksig (from build-tools 37.0.0 `lib/apksigner.jar`), NDK C++20 via the existing CMake, Git Bash scripts.

**Spec:** `docs/superpowers/specs/2026-10-15-general-32bit-app-support-design.md` (M1).

## Global Constraints

- Package name unchanged by default (spec: rename is opt-in only; not built in M1).
- `targetSdkVersion` raised only to the install floor: **24** (verify on the OnePlus, Android 16 / sdk 36; if the installer still refuses, raise to the value its message names and record it).
- Original `META-INF/*` signature files are dropped; output signed v1+v2+v3 with apksig.
- An APK whose manifest has `android:extractNativeLibs="false"` is refused by name in M1 (stored-aligned `.so` packaging is not built yet).
- armeabi-v7a preferred over armeabi; an APK with neither is refused by name.
- Every refusal or skipped thing is a named line in the repack report (project rule: "refuse loudly").
- The engine library keeps its name `librbguest.so` (the spec's `librbruntime.so`; renaming is churn with no M1 value).
- M1's stub is one **prebuilt** library copied under each 32-bit library's name; it exports only `JNI_OnLoad`. The spec's generated stubs exporting every `Java_*` symbol arrive with M3, which is the first milestone that can answer those calls. Recorded as a known deviation.
- M1 boot reuses `Inspect()` unchanged: its 64 MiB window and per-run teardown stay until M2's extraction. Recorded as a known deviation from the spec's 4 GiB window.
- All 18 engine lanes stay green: `RB_SERIAL=R52Y106BECZ bash tools/run_engine_tests.sh arm64-v8a`.
- Devices: OnePlus `75ff8cd3` (64-bit only, acceptance), Tab S9 `R52Y106BECZ` (has native Gumball installed — do not install repackaged Gumball over it). Security dialogs (install confirmation, "install unknown apps") are the human's to answer.

## Review Focus

- The same APK repackaged twice → second install updates the first (same AndroidKeyStore key alias `retrobug-repack`), not "conflicting signature".
- A device where the original app is installed with its own signature → install fails; the report must say `INSTALL_FAILED_UPDATE_INCOMPATIBLE`-style status text, not "failed".
- RetroBug not yet allowed to "install unknown apps" → PackageInstaller returns pending/blocked; the UI must say which setting the human must allow, not hang.
- An APK carrying both `lib/armeabi/` and `lib/armeabi-v7a/` → only v7a libs move; armeabi entries are dropped and listed in the report.
- A 32-bit library name that is not `lib*.so` (e.g. `lib/armeabi-v7a/gdbserver`) → copied to `assets/rb32/` but gets no stub, and is listed.

---

### Task 1: Binary manifest editor (`AxmlEditor`) with host tests

**Files:**
- Create: `android/java/com/retrobug/launcher/AxmlEditor.java`
- Create: `tools/java-tests/AxmlEditorTest.java`
- Create: `tools/run_java_tests.sh` (builds a fixture manifest with `aapt2 link`, compiles launcher sources + tests with the host `javac` against `android.jar`-free sources, runs them with `java`, exits non-zero on any failure)
- Create: `tests/corpus/manifests/min_only/AndroidManifest.xml` (package `com.retrobug.fixture`, `<uses-sdk android:minSdkVersion="8"/>`, one `<application>` with one activity) and `tests/corpus/manifests/target22/AndroidManifest.xml` (same with `targetSdkVersion="22"` and `android:extractNativeLibs="false"` on `<application>`)

**Interfaces:**
- Produces:
  - `final class AxmlEditor` with `static AxmlEditor parse(byte[] axml) throws IOException`
  - `String packageName()`; `Integer minSdk()`; `Integer targetSdk()` (null when absent); `Boolean extractNativeLibs()` (null when absent)
  - `void setTargetSdk(int value)` — rewrites the `uses-sdk` attribute in place when present; inserts it (string pool entry `targetSdkVersion`, resource-map id `0x01010270`, attribute typed `TYPE_INT_DEC` 0x10) when absent; creates `<uses-sdk>` under `<manifest>` when that element is absent
  - `byte[] toBytes()`
  - The class must not import `android.*` (host-testable).

- [ ] **Step 1: Write the failing tests** in `AxmlEditorTest.java` (plain `main`, a `check(boolean, String)` helper counting failures, prints `N checks, M failures`):
  - `minOnlyReadsBack`: parse fixture `min_only` → `packageName()=="com.retrobug.fixture"`, `minSdk()==8`, `targetSdk()==null`.
  - `insertTargetSdk`: `setTargetSdk(24)` on `min_only`, `toBytes()`, re-parse → `targetSdk()==24`, `minSdk()==8`, `packageName()` unchanged; and `aapt2 dump badging` of an APK built around the edited manifest prints `targetSdkVersion:'24'` (the script zips the edited bytes into a minimal APK and runs aapt2; the test reads its output file).
  - `rewriteTargetSdk`: `target22` → `targetSdk()==22`, after `setTargetSdk(24)` → 24, byte length unchanged.
  - `readsExtractNativeLibs`: `target22` → `extractNativeLibs()==Boolean.FALSE`; `min_only` → null.
  - `refusesGarbage`: `parse(new byte[]{1,2,3})` throws `IOException` whose message contains `"not a binary XML"`.

- [ ] **Step 2: Run** `bash tools/run_java_tests.sh` — expected: compile failure (`AxmlEditor` missing), script exits non-zero.

- [ ] **Step 3: Implement `AxmlEditor`.** Chunk model per `frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h`: RES_XML_TYPE 0x0003 header, RES_STRING_POOL_TYPE 0x0001 (UTF-8 and UTF-16 pools both possible — Gumball's is UTF-16, check flag 0x100), RES_XML_RESOURCE_MAP_TYPE 0x0180, START_ELEMENT 0x0102 (attributes 20 bytes each: ns, name, rawValue, size=8, res0, dataType, data; `attributeCount` and `attributeStart`/`attributeSize` in the ext header). Inserting a string means: append to the pool, and because the resource map indexes the *first* N strings by position, a new attribute name with a resource id must be inserted at index == current resource-map length, shifting every later string index in all chunks by +1. Keep attributes sorted by resource id (aapt2 requires it; `targetSdkVersion` 0x01010270 sorts after `minSdkVersion` 0x0101020c). Recompute every chunk size.

- [ ] **Step 4: Run** `bash tools/run_java_tests.sh` — expected `... checks, 0 failures`, exit 0.

- [ ] **Step 5: Commit** `git add android/java/com/retrobug/launcher/AxmlEditor.java tools/java-tests tools/run_java_tests.sh tests/corpus/manifests && git commit -m "Repackager: binary manifest editor with host tests"`

### Task 2: Stub library + in-process runtime boot

**Files:**
- Create: `android/jni/runtime_boot.cpp`, `android/jni/runtime_boot.h`
- Create: `android/jni/stub.cpp`
- Modify: `android/jni/launcher.cpp` — move `ZipEntry` and `Inspect` declarations into `runtime_boot.h`-visible scope (declare `Inspect`/`RenderHooks`/`ZipEntry` in a new internal header `android/jni/inspect.h`, keep definitions where they are, drop them from the anonymous namespace)
- Modify: the CMake that builds `librbguest` (find with `grep -rn "RB_BUILD_LAUNCHER" CMakeLists.txt android cmake`) — add `runtime_boot.cpp` to `librbguest`, add target `rbstub` (SHARED, `stub.cpp`, links `librbguest` + `log` + `dl`, property `NO_SONAME ON` so each copy is known to the linker by its file name)
- Modify: `tools/build_launcher_apk.sh` — stage `librbstub.so` into the launcher APK at `lib/arm64-v8a/librbstub.so` (stripped like librbguest)

**Interfaces:**
- Consumes: `Inspect(app, libm, libc, libz, libstdcxx, label, tiers, apk_path, w, h, RenderHooks{})` and `ZipEntry(file_bytes, entry_name, &why)` as they exist in `launcher.cpp` today.
- Produces:
  - `extern "C" int rb_runtime_boot(JavaVM* vm, const char* apk_path, const char* lib_name)` in `librbguest.so`: reads `assets/rb32/<lib_name>` and `assets/rb32sys/{libc,libm,libz,libstdc++}.so` from `apk_path`, calls `Inspect(..., label=lib_name, tiers=2 /*Tier J*/, apk_path, 0, 0, {})`, writes every report line to logcat tag `RBRUNTIME` and the whole report to `<dataDir>/files/rb-report.txt` (dataDir derived from `/proc/self/cmdline` package → `/data/data/<pkg>`), returns `JNI_VERSION_1_6`. Idempotent per `lib_name` within a process (second call returns immediately). Missing entries are named report lines, never a crash.
  - `librbstub.so`'s `JNI_OnLoad(JavaVM*, void*)`: `dladdr(&JNI_OnLoad)` → own file basename (e.g. `libABK.so`); APK path via JNI `android.app.ActivityThread.currentApplication().getApplicationInfo().sourceDir`; calls `rb_runtime_boot`; returns its value.

- [ ] **Step 1: Write the failing check.** Add a launcher self-check reachable by intent: `am start -n com.retrobug.launcher/.LauncherActivity -e bootcheck <game-name>` makes the launcher call a new `static native String nativeBootCheck(String apkPath, String libName)` that wraps `rb_runtime_boot` against an APK whose libs are laid out as M1's output would be. For this task the test input is produced by a host script `tools/make_bootcheck_apk.sh <in.apk> <out.apk>` (zip-level: copies `lib/armeabi-v7a/*.so`→`assets/rb32/`, `build/device-libs/{libc,libstdc++}.so` + `build/guest-libs/{libm,libz}.so`→`assets/rb32sys/`), pushed to the launcher's games dir via the existing picker path. Expected report line to assert on: `RESULT: startup finished with no fault` for Angry Birds Go (tiers=2).
- [ ] **Step 2: Run** it before implementing → expected: `nativeBootCheck` missing (`UnsatisfiedLinkError` in logcat).
- [ ] **Step 3: Implement** `inspect.h`, `runtime_boot.cpp`, `stub.cpp`, CMake and build-script changes.
- [ ] **Step 4: Verify on the tablet**: `bash tools/build_launcher_apk.sh` (exit 0), install, run bootcheck for Angry Birds Go → `adb logcat -d -s RBRUNTIME` contains `RESULT: startup finished with no fault`; run `RB_SERIAL=R52Y106BECZ bash tools/run_engine_tests.sh arm64-v8a` → all lanes `0 failures`; Gumball run unchanged (`RB_WAIT=20 bash tools/run_launcher_on_device.sh R52Y106BECZ gumball` still reaches `scene is now GumballMainMenu`).
- [ ] **Step 5: Commit** `git commit -m "Runtime: in-process boot entry and the 64-bit stub library"`

### Task 3: APK transform (`ApkRepackager`) with host tests

**Files:**
- Create: `android/java/com/retrobug/launcher/ApkRepackager.java` (no `android.*` imports)
- Create: `tools/java-tests/ApkRepackagerTest.java`; extend `tools/run_java_tests.sh` to build a fixture APK (`aapt2 link` of `min_only` + zip in `lib/armeabi/libfoo.so`, `lib/armeabi-v7a/libfoo.so`, `lib/armeabi-v7a/libbar.so`, `lib/armeabi-v7a/gdbserver`, `META-INF/CERT.RSA`, `META-INF/CERT.SF`, `META-INF/MANIFEST.MF`, `assets/x.txt`)

**Interfaces:**
- Consumes: `AxmlEditor` (Task 1).
- Produces:
  - `final class ApkRepackager` with `static Result transform(File inApk, Map<String, byte[]> runtimeFiles, File outUnsigned) throws IOException`
  - `runtimeFiles` keys are output entry names supplied by the caller: `lib/arm64-v8a/librbguest.so`, `stub` (the stub bytes, copied once per stub name), `assets/rb32sys/libc.so`, `assets/rb32sys/libm.so`, `assets/rb32sys/libz.so`, `assets/rb32sys/libstdc++.so`
  - `static final class Result { String packageName; int oldTargetSdk /* -1 absent */; int newTargetSdk; List<String> guestLibs; List<String> stubs; List<String> report; }`
  - Throws `IOException` with message starting `"refused: "` for: no 32-bit libs, `extractNativeLibs="false"`, an existing `lib/arm64-v8a/` entry.

- [ ] **Step 1: Write the failing tests:**
  - `movesV7aAndStubs`: output has `assets/rb32/libfoo.so`, `assets/rb32/libbar.so`, `assets/rb32/gdbserver`; `lib/arm64-v8a/libfoo.so` and `lib/arm64-v8a/libbar.so` byte-equal to the supplied stub; `lib/arm64-v8a/librbguest.so` present; no `lib/armeabi*/` entries; no `META-INF/` entries; `assets/x.txt` unchanged; `result.stubs == [libbar.so, libfoo.so]` (sorted).
  - `reportsDroppedAndUnstubbed`: `result.report` contains a line with `lib/armeabi/libfoo.so` and the word `dropped`, and one with `gdbserver` and `no stub`.
  - `bumpsTarget`: re-parse output manifest with `AxmlEditor` → `targetSdk()==24`, `result.oldTargetSdk==-1`.
  - `refusesNoLibs` / `refusesExtractFalse`: each throws, message starts `"refused: "`.
- [ ] **Step 2: Run** `bash tools/run_java_tests.sh` → expected FAIL (class missing).
- [ ] **Step 3: Implement** with `java.util.zip` (preserve each copied entry's compression method; `.so` written DEFLATED; set entry times to a fixed 2008-01-01 for reproducibility).
- [ ] **Step 4: Run** → `0 failures`.
- [ ] **Step 5: Commit** `git commit -m "Repackager: APK transform with host tests"`

### Task 4: Signing, install and UI in the launcher

**Files:**
- Create: `android/java/com/retrobug/launcher/RepackSigner.java`, `android/java/com/retrobug/launcher/RepackInstaller.java`
- Modify: `android/java/com/retrobug/launcher/LauncherActivity.java` — "Make installable" button per game row; intent extra `-e repack <name-substring>` runs the same path for scripted runs; shows the `Result.report` and installer status text
- Modify: `android/AndroidManifest.xml` — `<uses-permission android:name="android.permission.REQUEST_INSTALL_PACKAGES"/>`
- Modify: `tools/build_launcher_apk.sh` — add `$BT/lib/apksigner.jar` to the d8 inputs (program classes) and to javac's classpath

**Interfaces:**
- Consumes: `ApkRepackager.transform` (Task 3); `librbstub.so` / `librbguest.so` / guest libs read from the launcher's own APK (`getApplicationInfo().sourceDir`: `lib/arm64-v8a/librbstub.so`, `lib/arm64-v8a/librbguest.so`, `assets/guest/*.so`).
- Produces:
  - `RepackSigner.sign(File unsigned, File signed) throws Exception` — AndroidKeyStore RSA-2048 alias `retrobug-repack` (created once; purposes SIGN, digests SHA-256 + SHA-512, padding PKCS1), apksig `ApkSigner.Builder` with v1, v2, v3 enabled, `minSdkVersion` 26.
  - `RepackInstaller.install(Context, File apk, Callback)` — PackageInstaller session (MODE_FULL_INSTALL), status `BroadcastReceiver`; on `STATUS_PENDING_USER_ACTION` starts the confirm intent; reports `EXTRA_STATUS` + `EXTRA_STATUS_MESSAGE` verbatim; if `!getPackageManager().canRequestPackageInstalls()` says so and opens `ACTION_MANAGE_UNKNOWN_APP_SOURCES` for RetroBug instead of starting a session.
  - Outputs are kept at `getExternalFilesDir(null)/repack/<label>.apk` (pullable with adb) plus `<label>.txt` (the report).

- [ ] **Step 1: Write the failing device check:** `bash tools/build_launcher_apk.sh`, install on the OnePlus, `am start ... -e repack angry-birds-go` → expected today: no such extra handled (no `repack/` dir appears: `adb shell ls /sdcard/Android/data/com.retrobug.launcher/files/repack` fails).
- [ ] **Step 2: Implement** the three classes, the manifest permission and the build-script change.
- [ ] **Step 3: Verify the artifact:** pull `repack/angry-birds-go-1.10.1.apk`; `apksigner.bat verify --verbose` → `Verified using v2 scheme (APK Signature Scheme v2): true` and v3 true; `aapt2 dump badging` → `package: name='com.rovio.angrybirdsgo'`, `targetSdkVersion:'24'`, `native-code: 'arm64-v8a'`.
- [ ] **Step 4: Verify the review-focus paths:** repack the same game twice and install twice → second install succeeds as an update; on the tablet (native Gumball installed) installing repackaged Gumball reports the installer's incompatible-signature status text verbatim.
- [ ] **Step 5: Commit** `git commit -m "Repackager: apksig signing, PackageInstaller install, launcher UI"`

### Task 5: Acceptance on the OnePlus + handoff

**Files:**
- Modify: `HANDOFF.md` (M1 status, commands, the known deviation), `docs/superpowers/specs/2026-10-15-general-32bit-app-support-design.md` (M1 marked done with evidence)

- [ ] **Step 1:** Repack + install Angry Birds Go and Gumball on the OnePlus (human taps the confirmations). `adb shell pm list packages` shows both packages.
- [ ] **Step 2:** Launch each by its own launcher icon. Expected in `adb logcat -d -s RBRUNTIME`, with the **game's** pid (`adb shell pidof com.rovio.angrybirdsgo`): `constructors: ... ran in total` and `RESULT: startup finished with no fault` (Angry Birds Go) / Gumball's profile lines through `gumball: ... frame(s) driven` (headless). The app then stops at its first `native` method with `java.lang.UnsatisfiedLinkError` naming a `Java_*` symbol — that is M1's documented end point (M3 adds the exports); record the exact symbol.
- [ ] **Step 3:** All lanes green: `RB_SERIAL=R52Y106BECZ bash tools/run_engine_tests.sh arm64-v8a`; `bash tools/run_java_tests.sh` → `0 failures`.
- [ ] **Step 4:** Commit `git commit -m "M1 accepted: repackaged games boot their 32-bit library in their own process"`
