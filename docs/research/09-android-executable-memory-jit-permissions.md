# 09 — Android executable memory / JIT permissions

**Question:** can RetroBug (an unprivileged, user-installable APK) emit AArch64 host code and execute it in-process?
**Method:** AOSP source (`system/sepolicy`, `bionic`, `art`) read directly, plus shipping-emulator source and project docs. Every claim below is cited. Where I could not find a primary source, it says so.

Research date: 2026-09. Trees read: `system/sepolicy` @ `refs/heads/main` (Android 16-era) and @ `refs/heads/android15-release`; `bionic` @ `refs/heads/main` and @ `android-8.0.0_r4`; `art` @ `refs/heads/main` / `master`.

---

## Verdict: can RetroBug JIT inside a normal Android app?

**YES — conditionally, and the condition is not the one the project brief assumed.**

AOSP SELinux **affirmatively grants** `execmem` to `appdomain` (the attribute every untrusted app inherits), and the in-tree comment names the reason as *application JIT compilers*:

```
# WebView and other application-specific JIT compilers
allow appdomain self:process execmem;
```
— [`system/sepolicy/private/app.te:189`](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/private/app.te) (identical rule + comment in
[`android15-release/private/app.te`](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/android15-release/private/app.te);
the same comment already exists in Android 7.1 vendor policy, see [OnePlus OSS `app.te` @ QC8996_N_7.1](https://github.com/OnePlusOSS/android_system_sepolicy/blob/oneplus/QC8996_N_7.1/app.te))

Corollaries, all verified in-tree:

* There is **no `neverallow` for app `execmem`**. I grepped the whole `main` `private/`+`public/` trees: the only `execmem` rules are *allows* (appdomain, app_zygote, webview_zygote, zygote, hal_drm, system_server_startup, a conditional system_server) plus `neverallow`s for `perfetto`/`traced`/`traced_probes`. [`private/app_neverallows.te`](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/private/app_neverallows.te) (324 lines) contains **zero** `execmem`/`execstack` rules.
* What *is* universally forbidden: `neverallow * self:process { execstack execheap };` with the comment "# Do not allow making the stack or heap executable" ([`private/domain.te:1342`](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/android15-release/private/domain.te)). So: you may `mmap` your own code arena; you may **not** make your stack or your `malloc` heap executable.
* The premise "file-backed exec from your own data dir is allowed, anonymous exec is not" is **backwards**:
  * anonymous `mmap(PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)` → allowed (needs `process:execmem`, which appdomain has).
  * `app_data_file` (i.e. `/data/data/<pkg>/…`, files/, cache/) is **not executable** by modern-target apps: appdomain only gets `create_file_perms` = `{ create rename setattr unlink rw_file_perms }` and `{ getattr map read write }` on `app_data_file` — no `execute` ([`private/app.te`](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/private/app.te), [`public/global_macros`](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/public/global_macros)); and
    `neverallow { all_untrusted_apps -untrusted_app_25 -untrusted_app_27 -runas_app } { app_data_file privapp_data_file }:file execute_no_trans;`
    ([`private/app_neverallows.te`](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/private/app_neverallows.te)).
  * `apk_data_file` **is** executable: `allow appdomain apk_data_file:file { getattr open read ioctl lock map x_file_perms; }` where `x_file_perms` = `{ getattr execute execute_no_trans map }` ([`public/global_macros`](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/public/global_macros)), and `/data/app(/.*)? u:object_r:apk_data_file:s0` ([`private/file_contexts`](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/private/file_contexts)). That is how your own bundled `.so` loads — and it is exploitable for a *shipped, offline-translated* code cache (see Fallback ladder #4).

**The exact conditions RetroBug must satisfy**

1. Allocate code arenas with `mmap`/`memfd_create` — never `execstack`, never `mprotect(PROT_EXEC)` on the C heap.
2. Do **not** build the "write a file into `getFilesDir()`, then mmap it `PROT_EXEC`" pattern as the primary mechanism — `app_data_file` has no `execute`. (It also can't be made to work by lowering `targetSdkVersion` unless you accept the legacy `untrusted_app_27`/`_25` domain and its entire legacy behaviour set.)
3. Assume **OEM variance** and ship a runtime probe (attempt a tiny RWX mmap + execute a 2-instruction stub; if it fails, switch to the memfd dual-view allocator; if *that* fails, fall back to the interpreter core). AOSP policy is not the deployed policy on every Samsung/Xiaomi/etc. build — I found no per-OEM data in this pass.
4. Be page-size agnostic (Android 15+ 16 KB devices, §5).
5. Confirm Google Play's dynamic-code clauses against our distribution model before launch (§4.f, flagged unverified).

**Not found (searched, no evidence of a newer lock-down):** no release in which app `execmem` was revoked; no `PR_SET_MDWE` / `MAP_JIT` (Darwin-only) / seccomp argument filter on `PROT_EXEC` in the app policy; no manifest permission, `ApplicationInfo` flag, `VMRuntime` API or `PackageManager` private flag that grants or gates JIT-ability. Executable anonymous memory in an app is simply *already permitted*; there is nothing to ask for.

---

## 1. W^X / SELinux `execmem` for `untrusted_app`

### 1.1 The rules (verbatim, `refs/heads/main`)

| Rule | Location | Meaning |
|---|---|---|
| `allow appdomain self:process execmem;` | [`private/app.te:189`](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/private/app.te) | apps may map anonymous executable memory |
| `allow app_zygote self:process execmem;` | `private/app_zygote.te:21` | |
| `allow webview_zygote self:process execmem;` | `private/webview_zygote.te:42` | V8 in WebView |
| `allow zygote self:process execmem;` | `private/zygote.te:120` | |
| `allow hal_drm self:process execmem;` | `private/hal_drm.te:12` | |
| `ifelse(target_requires_insecure_execmem_for_swiftshader, 'true', 'allow system_server self:process execmem;', 'neverallow system_server self:process execmem;')` | `private/system_server.te:1467-1469` | direction of travel: system domains *lose* execmem |
| `allow system_server_startup self:process execmem;` | `private/system_server_startup.te:7` | |
| `neverallow perfetto/traced/traced_probes self:process execmem;` | resp. `:113`, `:88`, `:130` | "Disallow mapping executable memory" |
| `neverallow * self:process { execstack execheap };` | `private/domain.te:1342` (+ comment at `:1340`: "We would also like to minimize execmem but it seems to be …") | stack/heap exec is dead for every domain |

`execmem`, `execstack`, `execheap` are `class process` permissions ([`private/access_vectors:302-304`](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/private/access_vectors)).

### 1.2 What actually happens on a denial

`neverallow` is a **build-time/CTS** constraint, not a runtime one — a device shipping a policy that violates it cannot pass, so we can treat the AOSP allow as the enforced floor. A runtime `avc: denied { execmem }` makes the LSM hook return `-EACCES`, i.e. `mmap`/`mprotect` fail with `errno == EACCES` (or `EPERM`); it does **not** `SIGSEGV`. A `SIGSEGV`/`SIGBUS` with `si_code == SEGV_ACCERR` at a code address instead means you jumped into memory you *thought* was executable (mapping/permission bug on our side), and `SIGSYS` + `code == SYS_SECCOMP` means seccomp (§2).
*Caveat:* I did not re-read `security/selinux/hooks.c` (`selinux_mmap_file` / `selinux_file_mprotect`) in this pass — the fetch failed. The precise hook-to-permission mapping (anon → `process:execmem`; file-backed re-protect → `file:execmod`) is stated here from SELinux's documented class semantics and must be re-verified against the device kernel: see §Open questions.

### 1.3 Version history (what tightened, and what did not)

* **Android 7.1/8.0 → today:** `allow appdomain self:process execmem;` with the "WebView and other application-specific JIT compilers" comment is present in an Android 7.1 vendor tree, in `android15-release`, and in `main`. It has **never been removed**. [OnePlus OSS](https://github.com/OnePlusOSS/android_system_sepolicy/blob/oneplus/QC8996_N_7.1/app.te), [android15-release/app.te](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/android15-release/private/app.te)
* **Android 8.0 (O):** the app **seccomp** filter is added — this, not W^X, was the real "NDK can't do that any more" event ([Android Developers Blog, "Seccomp filter in Android O", 2017-07-20](https://android-developers.googleblog.com/2017/07/seccomp-filter-in-android-o.html)).
* **Android 10+ (planning-era tightening, per [AOSP sepolicy history](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/private/domain.te)):** `execmem` minimised for *system* domains; apps exempted because of WebView/app JITs. `system_server`'s is now conditional/neverallowed.
* **`targetSdkVersion` effect on execmem:** none found. The only `targetSdk`-dependent thing in this area is the legacy-domain exception in the `app_data_file:file execute_no_trans` neverallow (`-untrusted_app_25 -untrusted_app_27`), i.e. *data-dir* execution is permitted for apps targeting old SDKs and blocked for modern ones. That is a reason **not** to design around data-dir files.
* **`/data` is not mounted `noexec`.** Proven by construction, not by an fstab read: the app's own native libraries and its `base.apk` live under `/data/app/…` and are mapped `PROT_EXEC` by the linker on every launch; policy grants exactly that (`apk_data_file:file … x_file_perms`). (Not independently verified against a live device's `/proc/mounts` — flagged below.)
* **Shared storage (`/sdcard`) is a separate, real restriction.** Apps' view of shared storage is a FUSE/sdcardfs mount that does not permit executing files (`Termux reports: "Executable fails to run from shared storage"`, [r/termux 2025-11](https://www.reddit.com/r/termux/comments/1orseh0/executable_fails_to_run_from_shared_storage/); [android.stackexchange: execute permissions on fuse/fat](https://android.stackexchange.com/questions/71668/execute-permissions-on-fuse-fat-filesystem)). This is orthogonal to our design: RetroBug must never put a code cache on shared storage.

### 1.4 Practical answer for RetroBug

Use `mmap` anonymous RWX (or the memfd dual-view below) for the translation cache. Do not use `malloc`/stack. Do not use `getDir()`/`getFilesDir()`. Do not use `/sdcard`.

---

## 2. seccomp-bpf app policy

**Mechanism.** bionic installs the filter for app processes: `set_app_seccomp_filter()` in [`bionic/libc/seccomp/seccomp_policy.cpp`](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/seccomp/seccomp_policy.cpp); the BPF program is the generated `arm64_app_filter` (`extern const struct sock_filter arm64_app_filter[]`, [`libc/seccomp/seccomp_bpfs.h`](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/seccomp/seccomp_bpfs.h)); default action for a non-allowed syscall is `SECCOMP_RET_TRAP` → `SIGSYS` (signal 31, `si_code == SYS_SECCOMP`) — crashes the app ([Android O blog](https://android-developers.googleblog.com/2017/07/seccomp-filter-in-android-o.html), [libuv #2105 for a real crash report](https://github.com/libuv/libuv/issues/2105)). Filter installed in **zygote**, so it is inherited by every forked app and **cannot be removed** by the app. It is **not** gated on `targetSdkVersion` (blog: applies to all apps). Android O blocked 17 syscalls on arm64, 70 on arm ([blog](https://android-developers.googleblog.com/2017/07/seccomp-filter-in-android-o.html)).

**Syscalls that matter to us.**

* `mmap` — allowed (present in the app allowlist with an arch qualifier; note arm64 has no `mmap2`: [`libc/SECCOMP_ALLOWLIST_APP.TXT`](https://android.googlesource.com/platform/bionic/+/HEAD/libc/SECCOMP_ALLOWLIST_APP.TXT), [`libc/SECCOMP_ALLOWLIST_COMMON.TXT`](https://cs.android.com/android/platform/superproject/+/master:bionic/libc/SECCOMP_ALLOWLIST_COMMON.TXT), [`android-8.0.0_r4/libc/SECCOMP_WHITELIST.TXT`](https://android.googlesource.com/platform/bionic/+/android-8.0.0_r4/libc/SECCOMP_WHITELIST.TXT)).
* `memfd_create` — allowed for **all** archs since Android 8 (verbatim: `memfd_create: int memfd_create(const char* name, unsigned int flags) all`, [android-8.0.0_r4 whitelist](https://android.googlesource.com/platform/bionic/+/android-8.0.0_r4/libc/SECCOMP_WHITELIST.TXT)) and demonstrably used from inside app processes by ART's own JIT code cache (§3.4).
* `mprotect` — I could not pull the exact line in this pass (the fetched allowlist excerpts were partial), but it must be allowed: bionic (`MTE`/`mprotect` guard pages), ART (`JitCodeCache` RX⇄RWX transitions) and every emulator allocator call it from app processes on shipping devices. Treat as "allowed, line not quoted here".
* `ptrace`, `personality` — not confirmed either way by my fetches. `ptrace` is separately foreclosed by SELinux (below), so the seccomp status is moot for our design.
* `MAP_JIT` — **does not exist on Android/Linux**; it is a Darwin mechanism (XNU only). No Android analogue.
* Argument-value filtering: the app filter is a syscall-**number** allowlist. I found **no evidence** of any Android seccomp rule inspecting `prot`/`flags` for `PROT_EXEC`. (Absence-of-evidence; flagged in Open questions.)

**ptrace (for the later in-process debugger).** SELinux confines who may ptrace an app: `neverallow { domain -appdomain … } appdomain:process ptrace;` (quoted from [`private/app.te`](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/private/app.te); see also the `sys_ptrace` capability neverallow in [`private/domain.te`](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/private/domain.te), whose allowlist is `{-vold, -dumpstate, -system_server, -storaged, …}` and does **not** include `appdomain`). Also relevant historically: a Project Zero report that ptrace could be used to defeat the app seccomp filter ([issue 42450789](https://project-zero.issues.chromium.org/42450789)) — i.e. Google treated app-reachable ptrace as a bug and closed it. **Design consequence:** RetroBug's debugger must be **in-process** (`sigaction`/`SA_SIGINFO` handler, `ucontext_t` register access, `setjmp`/longjmp resume, guest-side watchpoints in the shadow page table). Plan on cross-process ptrace being unavailable, as it is on modern Android.

**Guest syscalls are unaffected.** RetroBug traps guest ARMv7 `svc` itself; the guest never issues host syscalls, so the guest's syscall set is entirely under our control (including `cacheflush`, `personality`, etc.).

---

## 3. What shipping projects actually do (the decisive evidence)

### 3.1 PPSSPP — ordinary Play APK, ARM64 host JIT, plain anonymous RWX ✔
[`Common/MemoryUtil.cpp`](https://raw.githubusercontent.com/hrydgard/ppsspp/master/Common/MemoryUtil.cpp) allocates executable host memory with
`mmap(map_hint, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0)`.
PPSSPP is distributed as a normal user-installable APK ([Google Play listing](https://play.google.com/store/apps/details?id=org.ppsspp.ppsspp)) and its MIPS→ARM64 recompiler is the normal Android path. **This alone answers the headline question: yes, an unprivileged app can allocate and run generated AArch64 code on current Android.** (The "JIT is default-on for Android ARM64" detail is well known but I did not quote it from a primary source in this pass.)

### 3.2 Winlator — x86 DBT inside an APK, explicitly "no root" ✔
[`winebox64/winlator` README](https://github.com/winebox64/winlator): "Root is not required"; runs Windows apps via "Wine and Box86/Box64" on "Android 9.0 or later". Box86/Box64 are dynarecs that emit host ARM64 code at runtime inside the app process. Mobox uses the same engine family via `proot` (no root for the DBT itself; `proot` uses `ptrace` **as a tracer on its own child**, which is the part that is fragile on modern Android — see §2/§4 notes; Winlator/Mobox instead ship a patched `libproot`/`ptrace` shim).
Caveat: I did not read box64's allocator source; box64's Android dynarec crash reports exist ([box64 #361](https://github.com/ptitSeb/box64/issues/361) — SIGSEGV under Termux/proot, with no mmap/mprotect failure reported, and the maintainer notes FEX works) so the failure mode there is *not* an execmem denial.

### 3.3 Termux QEMU (TCG) — works without root, ~5× slower than native ✔
[ivonblog, "emulate an x86_64 Linux VM with QEMU in Termux"](https://ivonblog.com/en-us/posts/termux-qemu-system-linux/): TCG runs inside Termux (unprivileged, no root); "most Android phones do not have a KVM kernel module (only Pixel phones can use pKVM to accelerate QEMU after root)"; "programs inside the virtual machine run at less than about one fifth the speed of Termux native packages." No execmem/`mmap` errors reported — i.e. **QEMU's TCG code cache is permitted in an app process**. Same author's qemu-user write-up: [Termux qemu-user emulating x86_64](https://ivonblog.com/en-us/posts/termux-proot-distro-qemu-emulation/).
⇒ The brief's expectation ("Termux qemu reportedly fails due to execmem") was **not confirmed**; where qemu-on-Android fails, the reports are about missing symbols/packages ([termux-packages #26501](https://github.com/termux/termux-packages/issues/26501)) or performance, not `execmem`.

### 3.4 ART itself — the crux, resolved ✔
ART's in-app JIT code cache ([`art/runtime/jit/jit_memory_region.cc`](https://android.googlesource.com/platform/art/+/master/runtime/jit/jit_memory_region.cc)):
* **dual-view** path: `memfd_create` at ~line 107, two `MAP_SHARED` aliases over the same fd — one `kProtRW`, one `kProtRX` (lines ~138-142, 165); chosen when `if (mem_fd.get() >= 0)` (line 135), i.e. it *degrades* to
* **single-view**: `MAP_PRIVATE | MAP_ANON` (line ~192). The cache transitions RX→RWX to patch code, then back.
**So: ART is not privileged in any special way — it JITs because `appdomain` holds `process:execmem`, the same permission any app has.** ART is, however, the best model for a *W^X-clean* implementation (dual aliases, never simultaneously W and X), and is future-proof against any later MDWE-style hardening.

WebView/V8 is the other in-app JIT explicitly named by the policy comment.

### 3.5 Dynarmic / Citra / yuzu — **NOT VERIFIED in this pass**
I could not fetch Dynarmic's host allocator (`https://raw.githubusercontent.com/merryhime/dynarmic/main/src/common/host_memory.cpp` → 404; path/branch differs). Widely reported that Citra/yuzu Android builds ran Dynarmic's AArch64 back-end inside a normal APK, and that no `MAP_JIT`-like help was needed, but **do not cite this in the architecture doc until someone reads the file** (see Open questions). Same for FEX-Emu on Android (box64 #361 says only "FEX works").

### 3.6 Houdini / Intel's ARM translator and Android's native bridge — system path, not an app ✔(shape) / (details unverified)
The native bridge is a **system-image** feature: it is configured by the `ro.dalvik.vm.native.bridge` system property and its translator `.so`s live in `/system/arm/lib/…`, installed by the ROM (on Android-x86 by the `/system/bin/enable_nativebridge` script writing into `/system`: [gist of that script](https://gist.github.com/41a5d8ba498ceecca28e9d1069a32ede), [libhoudini install discussion](https://stackoverflow.com/questions/49634762/how-to-install-libhoudini-on-a-custom-android-x86-rig), [Integrate Houdini to emulator](https://utzcoz.github.io/2020-03-15/Integrate-Houdini-to-emulator.html)). A sideloaded APK cannot register one. Note it runs **per app process** (it is loaded into each guest app's process by the linker), not as a privileged daemon — so it is architecturally similar to RetroBug's design, differing only in *where it is installed and who configures it*. I did not fetch the AOSP native-bridge doc page (search attempts were rejected); the mechanism description above is from secondary sources plus the property name, and should be re-verified against `source.android.com/docs/core/architecture/*.nativebridge`.
Also note: 32-bit ARM support itself is being removed from Android (Play 64-bit requirement: [developer.android.com/google/play/requirements/64-bit](https://developer.android.com/google/play/requirements/64-bit)), which is precisely RetroBug's market gap.

---

## 4. Known workarounds and their real cost

Ranked by evidence quality. Since the primary path is legal (§1), most of these are **hedge** strategies, not necessities.

| # | Strategy | Works on current Android? | Cost / evidence |
|---|---|---|---|
| 1 | Anonymous `mmap RWX` code arena | **Yes** | Zero cost. `allow appdomain self:process execmem` + PPSSPP/box64/QEMU-TCG/ART-single-view in production. Wastes the W^X property (some hardening vendors dislike; Play doesn't). |
| 2 | **`memfd_create` + `MAP_SHARED` dual alias (RW + RX), seal it later** | **Yes** — this *is* ART's mechanism | Small complexity: one fd, two mappings, and a "publish" handoff when a block is finalized (`mprotect` RX-alias ⇄ RW-alias, or write-via-RW-alias + `dc/icache` maintenance). Needs only `memfd_create` (allowed since Android 8 for all archs) + `mmap` + `mprotect`. Recommended **primary** implementation for RetroBug because it is what ART ships and it survives any future MDWE enforcement. |
| 3 | "Write into a file under `getFilesDir()`, then `mmap PROT_EXEC` read-only" | **No (policy)** | `app_data_file:file` is granted only `create_file_perms`/`{getattr map read write}` — **no `execute`** — to appdomain, and `execute_no_trans` is `neverallow`ed for modern-target untrusted apps. Do not design around it. |
| 3b | Same trick, but with files under `/data/app/…` (`apk_data_file`) — i.e. **offline-translated code cache shipped inside the APK** | **Yes** (per policy: `x_file_perms` gives `execute`+`map` on `apk_data_file`) | Read-only: the cache can be *shipped*, never *grown*. Needs a desktop-side translator + per-guest-APK cache keying, and cannot be updated at runtime (a downloaded blob would land in `app_data_file` → not executable). Interesting **Tier-AOT** for popular guest APKs; also a legitimate store-compliant way to pre-translate. Not verified empirically — flag. |
| 4 | `MEMFD_CREATE` with `MFD_CLOEXEC`/seals, single RWX mapping | Yes (fd-based exec of a *memfd* mapping) | No measured advantage over #1/#2. (`MFD_EXEC` is a newer Linux flag; do not depend on it — Android kernels vary. Unverified.) |
| 5 | Pure interpreter / threaded (indirect-threaded) dispatch — no execmem at all | **Yes, always** (this is the iOS-forced design) | Precedent: UTM SE and iSH on iOS ([UTM docs](https://docs.getutm.app/installation/ios/), [UTM SE discussion](https://taoofmac.com/space/blog/2024-07-14/1430)). **Cost figure I could obtain:** TCG softmmu on an ARM64 Android host ≈ 5× slower than native ([ivonblog](https://ivonblog.com/en-us/posts/termux-qemu-system-linux/)) — that is *JIT*, not interpreter. A defensible interpreter-vs-DBT ratio **was not obtainable from any primary source in this pass**; expect 5–15× and treat it as a **Milestone-0 measurement, not an assumption**. |
| 6 | "JIT via a helper that already has execmem" (zygote-owned region, `artd`, isolated service, abusing `linker`) | **Unnecessary, and foreclosed** | Unnecessary because the app already has `execmem`. The realistic-looking variant *is* #2 (memfd created by our own process and shared only inside it). Sharing an executable memfd across processes is blocked for us anyway: `neverallow { domain -appdomain … } appdomain:process ptrace;` kills injection routes, `isolated_app` has no execmem grant and cannot host arbitrary guest code, and `artd`'s JIT pool is an ART-internal, binder-gated service for **ART's own** code cache (I did not verify the artd/JIT-pool interface exists as a usable surface — do not build on it). |
| 7 | Native-bridge / GSI / custom-ROM "power-user tier" | Yes, but different product | Requires an unlocked bootloader + user-flashed image; cannot be Play- or even APK-distributed. Divergence is large: build-id/boot image, `ro.dalvik.vm.native.bridge`, SELinux `platform_app`/`system_file` labels, per-ROM maintenance, and the guest runtime layer (our "reproduced Android runtime") would be replaced by the *real* Android runtime for that ABI. Verdict: acceptable only as a **last tier**, and it shares essentially **no** code with the app path beyond the ARMv7 front-end and the AArch64 back-end. |
| 8 | Manifest/permission/`ApplicationInfo` tricks | **None exist** | Verified by absence across `untrusted_app.te`, `app_neverallows.te`, `privapp_permissions`/`app_function_access` surfaces: there is no permission, no `android:isolatedProcess`-style flag (isolated services are *more* restricted, not less), and no `FLAG_ALLOW_*` bit that touches `execmem`. There is no "JIT API" to call because the supported API **is** POSIX `mmap`/`mprotect` from NDK code. Nothing in `dalvik.system.VMRuntime` or `PackageManager` `PRIVATE_FLAG_*` grants executable memory. (`dalvik.vm.*` properties are *device-owner* knobs, not app-accessible.) |
| 9 | Play-policy risk | **Open** | Google Play's "Device and Network Abuse" dynamic-code clauses target downloading *executable app code* (dex/native) from outside Play; RetroBug executing guest ARMv7 machine code that the user supplied is a different thing, but I could not retrieve the clause text (three searches failed/were rejected). **Must be resolved before launch.** [Play Console help](https://support.google.com/googleplay/android-developer/answer/17517561), [Android devs: Dynamic Code Loading](https://developer.android.com/privacy-and-security/risks/dynamic-code-loading) |

---

## 5. 16 KB page-size hosts

**Requirements (primary).** [`developer.android.com/guide/practices/page-sizes`](https://developer.android.com/guide/practices/page-sizes):

* "From Android 15, AOSP supports devices that are configured with a 16KB page size (16KB devices)." 16 KB devices today include Pixel 8/8 Pro/8a (Android 15 QPR1+), Pixel 9/9 Pro/9 Pro XL (QPR2+), Pixel 9a (Android 16+).
* Play gate: apps using native code must support 16 KB pages; the page states *"Starting February 1, 2027, if app updates do not support 16KB memory pages, you will not be able to publish them"* and that the requirement applies to apps targeting **Android 15 / API 35** or higher. (Earlier wording tied this to API 36/Nov 2025 — **re-verify the exact date/SDK pair** before publishing it in the architecture doc.)
* Hardcoded `PAGE_SIZE` is wrong: "the value is undefined in NDK r27+ when 16KB mode is active"; use `getpagesize()` or `sysconf(_SC_PAGESIZE)` (equivalently `getauxval(AT_PAGESZ)`, which is the *runtime* page size). ELF segments must be 16 KB aligned (`-Wl,-z,max-page-size=16384`, default from NDK r28; `zipalign -c -P 16 -v 4 APK`; AGP 8.5.1+). There is a linker back-compat mode for 4 KB-aligned `.so`s (`bionic.linker.16kb.app_compat.enabled`), verified via `adb shell getconf PAGE_SIZE`.
* "The kernel still uses full pages for `mmap`" — i.e. **mapping/protection granularity is the host page size**, whatever the ELF alignment trickery.

**Why this bites RetroBug specifically** (design consequence, our reasoning, not a cited claim):

1. The **guest** is ARMv7 Linux/Android and believes `PAGE_SIZE == 4096`. Its `mmap`/`mprotect`/`munmap` granularity, its `AT_PAGESZ` aux vector entry, its `sysconf(_SC_PAGESIZE)`, its `p_align`, and guest-JIT page-flip patterns are all 4 KB.
2. On a 16 KB host we cannot honour per-4 KB-protection changes: one host page covers four guest pages. Therefore RetroBug **must** have a **shadow page table**: host mappings made with a permissive union of protections (or fully RW anon regions for guest data), with 4 KB-granular guest permissions enforced in software — in the interpreter by a permission check on each access, and in the DBT by *block-granular* code-cache invalidation plus (for guest W^X) trapping writes via `userfaultfd`/`mprotect` on the coarse host granularity. This is the same problem QEMU user-mode solved with `page_set_flags`/`tb_flush_page`; expect it to be a real, non-trivial subsystem.
3. **Do not hardcode 4096 anywhere in host code.** All arena sizes, alignments, `mprotect` ranges, and code-cache granularity must come from a `HostPageSize()` helper (`getauxval(AT_PAGESZ)`), while a separate `GuestPageSize()` constant stays 4096. Never pass one where the other is expected — that is the class of bug that broke PPSSPP/React-Native/opencv apps on 16 KB devices ([opencv #27618](https://github.com/opencv/opencv/issues/27618), [repack #1191](https://github.com/callstack/repack/issues/1191)).
4. Guest 32-bit ELF loading is *our* loader, so the "ELF `p_align` < host page" problem is ours to solve: map the whole segment range once at host granularity and apply guest protections in the shadow table.
5. **Recommendation: fix guest page size at 4 KB** (do not make it configurable); it is what every real armeabi-v7a Android build used, and a configurable guest page size buys nothing except bugs. Report 4096 to the guest in `AT_PAGESZ`.

---

## 6. Memory ordering / cache maintenance for code caches

**AArch64 host: `mmap`/`mprotect` are NOT sufficient.** The AArch64 I-cache is not coherent with the D-cache, and stores to code are only guaranteed visible to the *other* execution/translation stages after explicit maintenance:
`DC CVAU`/`DC CIVAC` (clean D-cache to PoU) → `IC IVAU` (or `IC IALLU`) → `DSB ISH` → `ISB`.
The supported way to get this is `__builtin___clear_cache(begin, end)` (backed by compiler-rt's `__clear_cache`, which emits exactly the `IC IVAU`-style loop on AArch64, and by the ICache-maintenance requirements in the ARM ARM). **Verified in this pass:** that this is a real, Android-relevant concern — e.g. ByteDance's inline-hook library ships an "trying to execute non-executable memory" failure mode on Android 16 arm64-v8a ([android-inline-hook #111](https://github.com/bytedance/android-inline-hook/issues/111)), which is the signature of missing/incorrect cache maintenance or a stale I-cache line after patching. **Not verified:** I did not read `compiler-rt/lib/builtins/clear_cache.c` or bionic's `__clear_cache` source in this pass. Action: read them during Milestone 0 and confirm the sequence includes `DSB ISH`+`ISB`.

Android-specific gotchas to design in:
* `__builtin___clear_cache` **is** required even though the mapping was created `PROT_EXEC` in one `mmap` call — the permission and the cache state are unrelated.
* After patching a jump/inline-cache **on another thread**, `IC IVAU` broadcast + `DSB ISH` handles cross-core visibility, but the *other* thread also needs an `ISB` on its path out of the patched block — hence the standard DBT pattern: never patch a block in place; emit a new block and swap the pointer atomically, with a "block invalidated" generation counter.
* Freshly `mmap`'d pages do not need cleaning (they're not in the D-cache), but they **do** still need I-cache invalidation for the addresses you are about to run, because the I-cache may hold stale lines from previous mappings.
* **Do not** enable `PROT_BTI`, `PROT_MTE`, or PAC-enabled instruction encodings (`_hint_`/`paciasp`) on code pages unless we deliberately emit the corresponding prologues — Android kernels expose these as `mprotect` flags and they change fault/execute behaviour (see the upstream thread about avoiding `mprotect(PROT_BTI…)`: [linux-arm-kernel Nov 2020](https://lists.infradead.org/pipermail/linux-arm-kernel/2020-November/614792.html)). On Pixel-class devices MTE is enabled for some processes and can fault JIT writes; the mitigation is to keep the code cache in our own memfd with explicit `PROT_MTE`-off and to not mix tagged stores with untagged loads. *(Partially verified — treat as a hardening TODO, not a fact.)*

**Guest side (RetroBug must implement this regardless of host behaviour):** ARMv7 has no hardware I/D-cache coherency and Linux/ARM exposes `__ARM_NR_cacheflush` (`= __ARM_NR_BASE + 2 = 0x0f0002`, per `arch/arm/include/uapi/asm/unistd.h` — re-verify against the guest's kernel headers) as the *only* portable self-modifying-code primitive. Guest JITs (V8, ART, .NET compacts, games' own code patchers) call it constantly. RetroBug must:
1. trap `svc` `#0` with `R7 == 0xf0002` and treat `[R0,R0+R1)` as a "guest code was modified" notification → invalidate translated blocks overlapping that range;
2. also handle guests that modify code *without* calling `cacheflush` (allowed-but-racy on ARMv7, done in practice by packers and by some game engines): track writes to pages that contain translated code and invalidate those blocks (shadow page table + write-watch; coarse-grained is fine, and the guest's own `mprotect` gives us a cheap hook);
3. on a **riscv** host we would additionally need `syscall(SYS_riscv_flush_icache)` per core (that is exactly what AOSP's Berberis does); on **AArch64** no syscall is needed — `IC IVAU` from EL0 suffices. Keep a `FlushHostICache(hostAddr, len)` seam in the design anyway so a future riscv/x86 back-end is a one-function port.

---

## Fallback ladder (ordered, use top-down)

1. **`memfd_create` + `MAP_SHARED` dual-view (RW alias for emit, RX alias for execution), sealed after first publish.** ART's own mechanism → maximal survivability, no W+X pages. **Primary implementation.**
2. **Anonymous `mmap(RWX)` arena** if dual-view allocation fails (no `memfd_create` — very old OEM kernel, or a policy oddity). Same `process:execmem` permission, so a device that refuses #2 is a device that refuses #1's RX alias anyway.
3. **Single-alias "emit RW → `mprotect` RX → freeze"** with no further writes to that block (block-finalised allocator + new-block-on-patch). Still needs `execmem` for the anon→RX transition, but is the most W^X-compatible shape if some future `PR_SET_MDWE` policy appears.
4. **Offline translation tier:** ship pre-translated guest code caches **inside the APK** and `mmap` them `PROT_READ|PROT_EXEC` from `apk_data_file` (granted via `x_file_perms`). Read-only / never-grown; requires a PC-side translator and a cache keyed by guest APK hash. Store-compliant and JIT-free. *(Policy-derived, not empirically verified — spike it.)*
5. **Threaded (indirect-threaded) interpreter core, zero execmem.** Mandatory for the "locked-down OEM" tier and the design we must keep working anyway (cold blocks, exception tables, deopt). Needs its own performance target measured on-device.
6. **Native-bridge / GSI power-user tier** — only if everything above is blocked on a target device class; treat as a separate product, minimal code reuse.

If 1–3 all fail on a real device, that device's OEM policy has diverged from AOSP: log `dmesg`/`avc: denied { execmem }` (readable via `logcat -b events`/`pstore`? — no: apps cannot read `dmesg`; use the probe's `errno` + `strerror` and, for our own diagnostics, record the exact failure), report it, and route that device to tier 5.

---

## Open questions / unverified

1. **Kernel-side hook semantics** — exact SELinux permission consulted for (a) anonymous `mmap PROT_EXEC`, (b) `mprotect` → exec on anon, (c) `mprotect` → exec on a writable file mapping (`file:execmod`?). `security/selinux/hooks.c` fetch failed this pass. → Read `selinux_mmap_file` / `selinux_file_mprotect` on the actual device kernel.
2. **Dynarmic / Citra / yuzu / FEX on Android** — `src/common/host_memory.cpp` (and its Android branch) not retrieved (404). Must be re-read before the architecture doc asserts anything about Dynarmic. Also: DuckStation/RPCS3-style Android JIT claims unverified here (PPSSPP covers the argument anyway).
3. **`mprotect` line in the current app seccomp allowlist** — not quoted; the file exists at `bionic/libc/SECCOMP_ALLOWLIST_{COMMON,APP}.TXT` and needs a clean read for `mprotect`, `ptrace`, `personality`, `process_vm_*`, `userfaultfd`.
4. **Whether Android's seccomp filter inspects `prot`/`flags` arguments at all** — no evidence found either way.
5. **Exact Play requirement text/dates** for 16 KB pages (API 35+ vs API 36+, Feb 2027 vs Nov 2025) and the dynamic-code-loading clause that would govern shipping a runtime that *executes user-supplied* ARMv7 code. Needs a policy read, and a legal/compliance call.
6. **OEM variance** — no data collected for Samsung/Xiaomi/OPPO/vivo/Huawei policies overwriting `app.te`. This is the single largest residual risk to the "YES" verdict; resolved cheaply by an on-device probe matrix across ~6 shipping devices in Milestone 0.
7. **`/data` mount flags on a real device** (`adb shell mount | grep /data`) — inferred, not observed.
8. **`artd`/ART-Service JIT pool** — whether Android 16 moved in-app JIT code-cache allocation to a privileged service (`artd` exists as a domain in the policy tree, and `allow artd … execmem` was not observed in my grep hits). If ART ever *does* get its code cache from a service, our "appdomain has execmem" conclusion still stands (the rule is present in `main`), but the framing "ART does exactly what we do" would need softening.
9. **Interpreter-vs-DBT slowdown figure for a threaded ARMv7 interpreter on AArch64** — no primary source found. **Must be measured, not assumed.** (Reference point we do have: TCG ≈ 5× vs native on an Android ARM64 host, from §3.3.)
10. **BTI/MTE/PAC interaction** with our code cache on Pixel-class 16 KB / MTE devices — reasoned, not tested.
