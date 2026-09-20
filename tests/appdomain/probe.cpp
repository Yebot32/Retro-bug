/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The app-domain probe: what a **JIT is allowed to do from inside an installed Android app**.
 *
 * Why this exists at all. `tools/va_probe.c` measured the operations RetroBug's code cache needs — an
 * anonymous executable mapping, an `RW` → `RX` transition, a `memfd` that can be executed, and a 4 GiB
 * low reservation — and every answer came back from the **`shell`** domain. An app runs as
 * `untrusted_app`, which is a different SEPolicy domain with a different set of permissions, and
 * Android's W^X work targeted *apps* specifically. So those numbers, while true, are not the numbers
 * the design depends on, and `docs/10` U-2 and U-13 both say so in their own rows. This is the same
 * battery, run in the domain that matters.
 *
 * Two rules this file follows because "the syscall returned 0" is not the claim we need:
 *
 *   1. **Actually execute the page.** A mapping can be granted and the instruction fetch can still
 *      fault (NX on the TLB entry, an architecture-level guard, or a kernel that permits `PROT_EXEC`
 *      on `MAP_ANONYMOUS` but not on `memfd`). So where a mapping is executable, this writes a real
 *      `mov w0, #42; ret` and calls it, under a `SIGSEGV`/`SIGBUS`/`SIGILL` guard. An `ok` that
 *      survives to report a wrong return value is a finding in itself.
 *   2. **Report the domain, not just the result.** Without the SELinux context and the uid printed at
 *      the top, a green run here is indistinguishable from a run that quietly happened in `shell`
 *      because someone adb-pushed and executed this binary. The context line is what makes the rest of
 *      the output mean what it says.
 *
 * Built into `librbprobe.so` and loaded by `ProbeActivity`, so it runs in the app's own process.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <jni.h>
#include <android/log.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#define TAG "rbprobe"
#define LOG(...)                       \
  do {                                 \
    char _b[512];                      \
    std::snprintf(_b, sizeof _b, __VA_ARGS__); \
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", _b); \
    std::fputs(_b, stderr);            \
    std::fflush(stderr);               \
  } while (0)

namespace {

/// The 42-returning stub, for whichever ABI this is built as.
///
/// The 32-bit form matters more than it looks: this library is normally built arm64 because RetroBug's
/// *host* is arm64, but the page being executed here is the page a *guest* would run, and `armeabi-v7a`
/// is the only thing that answers U-13 for the shape the code cache actually has to serve.
#if defined(__arm__)
/// `movs r0, #42 ; bx lr`, Thumb encoding.
constexpr unsigned char kReturn42[] = {0x2Au, 0x20u, 0x70u, 0x47u};
/// A Thumb function is entered with bit 0 of the target set — `BX` reads it, and a caller that passes an
/// even address asks for ARM code, which in a Thumb-only page is an illegal instruction rather than a
/// wrong answer. Every other bit of the address is the address.
constexpr std::uintptr_t kThumbTag = 1u;
#else
constexpr std::uint32_t kReturn42[2] = {0x5280'0540u, 0xD65F'03C0u}; /* mov w0, #42 ; ret */
constexpr std::uintptr_t kThumbTag = 0u;
#endif

volatile sig_atomic_t faulted = 0;
// Bionic declares these in the global namespace only — `std::setjmp` does not exist here even in a C++
// translation unit — so the C names are used unqualified rather than the C++ header's promise.
jmp_buf fault_jmp;

std::uint64_t total_ok = 0, total_denied = 0, total_faulted = 0;

void OnFault(int sig) {
  faulted = sig;
  longjmp(fault_jmp, 1);
}

/**
 * Executes the stub at `at` and says what came back.
 *
 * Returns true only when the page really ran and returned 42. A `false` with "returned N" means the
 * mapping was granted and the execute was granted but the code is not what was written, which would be
 * a far worse bug than a denial and is reported as one.
 */
bool TryExecute(const char* label, void* at) {
  struct sigaction sa{};
  sa.sa_handler = OnFault;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  struct sigaction old_segv{}, old_bus{}, old_ill{}, old_sys{};
  sigaction(SIGSEGV, &sa, &old_segv);
  sigaction(SIGBUS, &sa, &old_bus);
  sigaction(SIGILL, &sa, &old_ill);
  // SIGSYS is Android's seccomp response, and it is the reason this list is not just segv/bus: the
  // default action is SECCOMP_RET_TRAP, so a blocked syscall **kills the process** rather than
  // returning EPERM. Reading AOSP's sepolicy for this probe is what caught it — without the handler, a
  // seccomp block would present as "the probe stopped answering" instead of naming the signal.
  sigaction(SIGSYS, &sa, &old_sys);

  bool good = false;
  if (setjmp(fault_jmp) == 0) {
    faulted = 0;
    const auto fn = reinterpret_cast<int (*)()>(
        reinterpret_cast<std::uintptr_t>(at) | kThumbTag);
    const int got = fn();
    good = (got == 42);
    if (!good) ++total_faulted;
    LOG("%-52s EXECUTED returned=%d %s", label, got, good ? "ok" : "*** WRONG VALUE ***");
  } else {
    ++total_faulted;
    LOG("%-52s MAPPED BUT FAULTED signum=%d (%s)", label, static_cast<int>(faulted),
        strsignal(static_cast<int>(faulted)));
  }

  sigaction(SIGSEGV, &old_segv, nullptr);
  sigaction(SIGBUS, &old_bus, nullptr);
  sigaction(SIGILL, &old_ill, nullptr);
  sigaction(SIGSYS, &old_sys, nullptr);
  return good;
}

void Report(const char* label, void* p) {
  if (p == MAP_FAILED) {
    ++total_denied;
    LOG("%-52s DENIED  errno=%d (%s)", label, errno, std::strerror(errno));
  } else {
    ++total_ok;
    LOG("%-52s ok      addr=%p", label, p);
  }
}

/**
 * What occupies the low 4 GiB of this process, and how large the biggest free gap there is.
 *
 * Defined only for the arm64 build, along with the ladder that calls it. A 32-bit process's entire
 * address space *is* the 4 GiB, so "can a contiguous 4 GiB window be had below 4 GiB" answers itself, and
 * every comparison against `1 << 32` in here is tautologically true or false once addresses are 32 bits —
 * including the one that would print "guest 32-bit space reachable without a bias: all of it". A
 * tautology in a measurement file is worse than a gap, because it reads like a result.
 * `MAP_FIXED_NOREPLACE` answered `EEXIST` at 0x10000, 0x10000000 and 0x80000000 — for sizes down to 512
 * MiB — in the app domain, while the same binary from `adb shell` reserved at 0x10000 without trouble.
 * `EEXIST` says "something is there"; it does not say what, how much, or whether it can be given back.
 * Those are precisely the questions the identity-vs-biased address model turns on, so this reads
 * `/proc/self/maps` and answers them directly instead of leaving the inference to a reader.
 */
#if !defined(__arm__)
constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;

void DescribeLowAddressSpace() {
  FILE* f = std::fopen("/proc/self/maps", "re");
  if (f == nullptr) {
    LOG("%-52s errno=%d (%s)", "open /proc/self/maps", errno, std::strerror(errno));
    return;
  }
  char line[512];
  // `unsigned long long` throughout rather than `uintptr_t`: the map file's addresses are 64-bit on an
  // arm64 kernel, and in the 32-bit build of this file `uintptr_t` is 32 bits, which makes every
  // comparison against `1 << 32` below either always true or always false. Parsing with `%lx` into a
  // 32-bit `unsigned long` on arm would also have been a format mismatch.
  unsigned long long lowest = ~0ull, highest_low_end = 0, count_low = 0, bytes_low = 0;
  unsigned long long prev_end = 0;
  unsigned long long biggest_gap = 0, biggest_gap_at = 0;
  bool first = true;
  int printed = 0;
  /** Considers one candidate gap; kept as a lambda so the space *before* the first mapping and
   * *after* the last is measured the same way as the spaces between them. Leaving the tail out is how
   * a 324 MiB window below 4 GiB would go uncounted, and the tail is a real candidate. */
  auto consider = [&](unsigned long long from, unsigned long long to) {
    if (to <= from || to - from <= biggest_gap) return;
    biggest_gap = to - from;
    biggest_gap_at = from;
  };
  while (std::fgets(line, sizeof line, f) != nullptr) {
    unsigned long long start = 0, end = 0;
    if (std::sscanf(line, "%llx-%llx", &start, &end) != 2) continue;
    if (start >= (1ull << 32)) continue;  // only the identity-addressable half matters here
    ++count_low;
    bytes_low += (end > (1ull << 32) ? (1ull << 32) : end) - start;
    if (start < lowest) lowest = start;
    if (end > highest_low_end) highest_low_end = end;
    if (!first) consider(prev_end, start);
    first = false;
    if (end > prev_end) prev_end = end;
    if (printed < 16) {
      ++printed;
      // The permissions field only — the path would be the longest thing here and the *owner* is
      // already visible from the address and the flags. Copied rather than truncated in place, because
      // the delimiter is not guaranteed to exist on a malformed line.
      char perms[8] = {};
      const char* sp = std::strchr(line, ' ');
      if (sp != nullptr) {
        std::size_t i = 0;
        while (i < 4 && sp[1 + i] != '\0' && sp[1 + i] != ' ') {
          perms[i] = sp[1 + i];
          ++i;
        }
      }
      LOG("  maps %08lx-%08lx %s", static_cast<unsigned long>(start),
          static_cast<unsigned long>(end < (1ull << 32) ? end : (1ull << 32)),
          perms[0] ? perms : "(unreadable)");
    }
  }
  std::fclose(f);
  if (count_low == 0) {
    LOG("  low 4 GiB: nothing mapped at all — identity addressing is available");
    return;
  }
  // The space between the last low mapping and the 4 GiB line, and the space between the page floor and
  // the first mapping. `0x1000` was `EPERM`, so the floor is real and above zero; the gap search starts
  // at it rather than at 0.
  consider(prev_end, 1ull << 32);
  consider(0x10000u, lowest);
  LOG("  low 4 GiB: %llu mappings, %llu MiB occupied, span 0x%lx..0x%lx",
      static_cast<unsigned long long>(count_low),
      static_cast<unsigned long long>(bytes_low / (1024u * 1024u)),
      static_cast<unsigned long>(lowest), static_cast<unsigned long>(highest_low_end));
  // How much of a window can actually be had, at the largest gap.
  //
  // The number the address model needs is not "is 4 GiB available" (it is not) but "what is the biggest
  // contiguous low window I can commit to at startup" — because that is the ceiling on how much of the
  // guest's 32-bit space can be reached without a bias addition. Bisection rather than a ladder: the
  // answer is between 0 and 4 GiB and each probe costs one `mmap`.
  LOG("  %-50s %llu MiB at 0x%lx", "largest free gap below 4 GiB",
      static_cast<unsigned long long>(biggest_gap / (1024u * 1024u)),
      static_cast<unsigned long>(biggest_gap_at));
  const unsigned long long gap = biggest_gap_at;
  const unsigned long long gap_size = biggest_gap;
  if (gap != 0 && gap_size >= (16u << 20)) {
    // Bounded by the gap the search found, not by 4 GiB: bisecting past it would just rediscover that
    // the neighbouring mapping is there, and the answer would be the same but slower.
    unsigned long long lo = 0, hi = gap_size > (4ull * kGiB) ? (4ull * kGiB) : gap_size;
    // Logged every step, because the first version of this loop never terminated: `mid` is rounded down
    // to a 16 MiB granularity, so once `hi - lo` got below 32 MiB the rounded midpoint equalled `lo`,
    // the map succeeded, `lo` did not move, and the `while` condition stayed true forever. The process
    // produced no signal and no tombstone — it simply stopped answering, which is the worst kind of
    // hang to debug on a device nobody is watching. A search must break when a step stops making
    // progress, not merely be capped.
    for (int step = 0; lo + (16u << 20) < hi; ++step) {
      const unsigned long long mid = (lo + hi) / 2ull & ~((16ull << 20) - 1ull);
      if (mid == lo || mid == hi) break;
      void* t = mmap(reinterpret_cast<void*>(static_cast<std::uintptr_t>(gap)), mid, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
      if (t == MAP_FAILED) {
        hi = mid;
        LOG("  bisect %d: %llu MiB DENIED errno=%d", step,
            static_cast<unsigned long long>(mid / (1024u * 1024u)), errno);
      } else {
        lo = mid;
        munmap(t, mid);
      }
    }
    LOG("  %-50s %llu MiB at 0x%llx", "largest contiguous low window obtainable",
        static_cast<unsigned long long>(lo / (1024u * 1024u)), gap);
    LOG("  %-50s %s", "guest 32-bit space reachable without a bias",
        lo >= (4ull * kGiB) ? "all of it" : "PARTIAL — biased guest base is required on Android");
  }
}
#endif  // !__arm__: see the note on the definition above

int MemfdCreate(const char* name, unsigned flags) {
  return static_cast<int>(syscall(SYS_memfd_create, name, flags));
}

}  // namespace

#if defined(__arm__)
/**
 * One ARM EABI syscall, raw, with the seccomp answer classified rather than assumed.
 *
 * `docs/20` §C07 requires `execve`/`fork` to be refused "with the same errno a real 32-bit Android would
 * give", and `docs/10` §10.2 lists them as seccomp denials — which, depending on the filter's action, is
 * either an errno (`SECCOMP_RET_ERRNO`) or a signal that kills the process (`SECCOMP_RET_TRAP`). Those
 * are different implementations, and which one applies is not readable from a document: from `adb shell`
 * the answer is "neither, nothing is filtered here", because the shell is not the app domain. So each
 * number is asked once inside the app, with the same `SIGSYS`/`SIGILL`/`SIGSEGV` guard the execute tests
 * use, and the row says which of the three happened.
 *
 * One bit of that answer generalises: if a *single* number traps, the filter's action is TRAP and the
 * others trap too — which is why the list avoids anything that would fork, exec or attach. `ptrace` is
 * asked last and with an invalid request, because `PTRACE_TRACEME` succeeds in some domains and a
 * traced process behaves differently from that point on. `fork`/`vfork`/`clone` are not asked at all: a
 * child inherits this process's stdio buffer and prints the whole probe twice, which is a confusing
 * artifact to leave in a measurement file when the same conclusion is available from calls that cannot
 * have side effects.
 */
std::uint32_t RawEabi(unsigned nr, std::uint32_t a0, std::uint32_t a1, std::uint32_t a2,
                      std::uint32_t* r0out, std::uint32_t* cpsrout) {
  register unsigned r0 asm("r0") = a0;
  register unsigned r1 asm("r1") = a1;
  register unsigned r2 asm("r2") = a2;
  register unsigned r3 asm("r3") = 0;
  unsigned cpsr = 0;
  // r7 is the syscall-number register in ARM EABI *and* clang's Thumb frame pointer, so it cannot be an
  // asm operand at all — "write to reserved register". Parked and restored inside the block, which is
  // also what the guest's own stubs do. `svc` and `mrs` stay in one statement because the carry bit is
  // one of the things being reported and any C expression between them can set flags.
  //
  // r2 is passed explicitly rather than left alone because the first version of the `execve` case did
  // exactly that, and the garbage it held was an invalid `envp` pointer: `execve` then answered EFAULT
  // about the argument instead of the EACCES answer about the *file* that the whole test was there to
  // get. A syscall probe that leaks a register into the call it is measuring is not measuring the call.
  asm volatile("mov r3, r7\n"
               "mov r7, %[nr]\n"
               "svc #0\n"
               "mrs %[c], cpsr\n"
               "mov r7, r3\n"
               : "+r"(r0), "+r"(r3), [c] "=r"(cpsr)
               : [nr] "r"(nr), "r"(r1), "r"(r2)
               : "memory", "cc");
  *r0out = r0;
  *cpsrout = cpsr;
  return 0;
}

void AskGuest(std::uint32_t nr, std::uint32_t a0, std::uint32_t a1, const char* label,
              std::uint32_t a2 = 0) {
  struct sigaction sa{};
  sa.sa_handler = OnFault;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NODEFER;  // the mask is restored by the longjmp below, so no double-block wedge
  struct sigaction old_sys{}, old_segv{}, old_ill{};
  sigaction(SIGSYS, &sa, &old_sys);
  sigaction(SIGSEGV, &sa, &old_segv);
  sigaction(SIGILL, &sa, &old_ill);
  if (setjmp(fault_jmp) == 0) {
    faulted = 0;
    std::uint32_t r0 = 0, cpsr = 0;
    RawEabi(nr, a0, a1, a2, &r0, &cpsr);
    LOG("  r7=0x%05x %-26s RETURNED r0=0x%08x (%d) C=%d", nr, label, r0, static_cast<int>(r0),
        (cpsr >> 29) & 1);
  } else {
    LOG("  r7=0x%05x %-26s TRAPPED signal=%d%s", nr, label, static_cast<int>(faulted),
        faulted == SIGSYS ? "  <- SECCOMP_RET_TRAP: there is no errno for this refusal" : "");
  }
  sigaction(SIGSYS, &old_sys, nullptr);
  sigaction(SIGSEGV, &old_segv, nullptr);
  sigaction(SIGILL, &old_ill, nullptr);
}

void GuestAbiProbe() {
  LOG("=== the guest ABI itself, inside the app domain ===");
  // TLS, the way a 32-bit bionic reads it. `docs/30` §Step 5g measured this from an adb-shell 32-bit
  // process; the app domain can refuse the coprocessor read even where the shell allows it, and `tp` is
  // the one MachineState field no tier can currently supply (no Coprocessor hook, §Step 9) — so this line
  // says whether that gap is worth closing before any bionic-dependent guest is loaded.
  if (setjmp(fault_jmp) == 0) {
    faulted = 0;
    unsigned tls = 0;
    asm volatile("mrc p15, 0, %0, c13, c0, 3\n" : "=r"(tls));
    LOG("  tpidrurw via mrc = 0x%08x %s", tls, tls ? "" : "(zero)");
  } else {
    LOG("  tpidrurw via mrc TRAPPED signal=%d — the emulator would have to supply it",
        static_cast<int>(faulted));
  }
  // ARM EABI numbers, from tests/corpus/syscall-abi-arm.txt. getpid is the control: if *it* traps, the
  // filter is not the app policy and the rest of the block is uninterpretable.
  LOG("  --- seccomp action per syscall (control first)");
  AskGuest(20, 0, 0, "getpid (control)");
  AskGuest(6, 0x7fffffffu, 0, "close(bad fd)");
  AskGuest(322, 0x7fffffffu, 0, "openat(bad dirfd)");
  AskGuest(337, 0, 0, "unshare(0)");
  AskGuest(375, -1u, 0, "setns(-1)");
  AskGuest(386, 0, 0, "bpf");
  AskGuest(388, 0, 0, "userfaultfd");
  AskGuest(377, 0, 0, "process_vm_writev");
  AskGuest(11, 0, 0, "execve(NULL)");
  AskGuest(136, 0xffffffffu, 0, "personality(invalid)");
  AskGuest(26, 0x1000u, 0, "ptrace(invalid request, last)");

  // ---- the one denial question that has a *policy* answer, asked with a path that exists.
  //
  // `execve(NULL)` above returns -EFAULT, which is an argument-checking answer and tells RetroBug
  // nothing: `docs/20` §C07 wants "the same errno a real 32-bit Android would give", and what a real one
  // gives for a *real* file the app owns is the SELinux answer — `app_data_file:file execute_no_trans`
  // is neverallowed for a targetSdk >= 29 app, so the expected value is EACCES(13). Saying "expected" is
  // not measuring it, and the measurement needs a path the app can write, which is why the package name
  // is read out of `/proc/self/cmdline` rather than hardcoded: it is this process's own directory, and a
  // copy of a shared object is not an executable file anyway, so the worst case is `ENOEXEC` — which
  // would itself be the finding, because it would mean the execute check had already been passed.
  char pkg[128] = {};
  if (FILE* c = std::fopen("/proc/self/cmdline", "re")) {
    if (std::fread(pkg, 1, sizeof pkg - 1, c) <= 0) pkg[0] = '\0';
    std::fclose(c);
  }
  pkg[sizeof pkg - 1] = '\0';
  char path[256] = {};
  if (pkg[0] != '\0' && std::strchr(pkg, '/') == nullptr) {
    std::snprintf(path, sizeof path, "/data/user/0/%s/cache/rb-exec-probe.so", pkg);
    bool wrote = false;
    if (FILE* w = std::fopen(path, "we")) {
      std::fputs("not an executable, and not meant to be one\n", w);
      wrote = std::fclose(w) == 0;
    }
    if (!wrote) {
      // The cache directory may not exist yet on a first run; make it and try once more, then say so
      // rather than reporting a denial that never happened.
      char dir[256] = {};
      std::snprintf(dir, sizeof dir, "/data/user/0/%s/cache", pkg);
      if (mkdir(dir, 0700) == 0 || errno == EEXIST) {
        if (FILE* w = std::fopen(path, "we")) {
          std::fputs("not an executable, and not meant to be one\n", w);
          wrote = std::fclose(w) == 0;
        }
      }
    }
    if (wrote) {
      const char* empty[1] = {nullptr};
      AskGuest(11, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(path)),
               static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(empty)),
               "execve(own file) <- the policy answer",
               static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(empty)));
      LOG("    (an ok/ENOEXEC here would mean the execute permission check was passed: report it)");
      remove(path);
    } else {
      LOG("  execve(own file): SKIPPED, could not write %s (errno=%d %s) — the policy answer is "
          "unmeasured, not assumed",
          path, errno, std::strerror(errno));
    }
  } else {
    LOG("  execve(own file): SKIPPED, no usable package name in /proc/self/cmdline");
  }
}
#endif  // __arm__

extern "C" JNIEXPORT void JNICALL Java_com_retrobug_probe_ProbeActivity_runNative(JNIEnv*, jobject) {
  LOG("=== app-domain probe: what a JIT may do inside an installed app ===");

  // ---- whose domain is this? The line that makes every answer below attributable.
  char ctx[256] = {};
  if (FILE* f = std::fopen("/proc/self/attr/current", "re")) {
    if (std::fgets(ctx, sizeof ctx, f) == nullptr) ctx[0] = '\0';
    std::fclose(f);
  }
  while (ctx[0] != '\0' && (ctx[std::strlen(ctx) - 1] == '\n' || ctx[std::strlen(ctx) - 1] == '\0')) {
    ctx[std::strlen(ctx) - 1] = '\0';
  }
  utsname u{};
  uname(&u);
  LOG("selinux=%s", ctx[0] ? ctx : "(unreadable)");
  // Enforcing or not, said explicitly. Every "ok" below would also be produced by a permissive policy,
  // so without this line the whole file is uninterpretable — and a shipping device that happens to be
  // permissive would make a design look safe on the bench and fail in the field.
  char enf[8] = {};
  if (FILE* e = std::fopen("/sys/fs/selinux/enforce", "re")) {
    if (std::fgets(enf, sizeof enf, e) == nullptr) enf[0] = '\0';
    std::fclose(e);
  }
  LOG("selinux_enforce=%s (1=Enforcing, 0=Permissive, empty=app cannot read it)",
      enf[0] ? enf : "(unreadable)");
  LOG("uid=%d gid=%d pid=%d comm=%s", static_cast<int>(getuid()), static_cast<int>(getgid()),
      static_cast<int>(getpid()), u.nodename);
  LOG("machine=%s release=%s pagesize=%ld at_pagesz=%lu", u.machine, u.release,
      sysconf(_SC_PAGESIZE), getauxval(AT_PAGESZ));
  // Which ABI this binary was built as, said out loud. The same source answers U-13 for the host ABI and
  // for the guest ABI, and a results file that does not say which one produced it cannot be used for
  // either — `uname -m` says aarch64 in a 32-bit process running under an arm64 kernel.
  LOG("build_abi=%s",
#if defined(__arm__)
      "armeabi-v7a (thumb-2, the guest ISA)"
#elif defined(__aarch64__)
      "arm64-v8a (the host ISA)"
#else
      "other"
#endif
  );

  // ---- the executable-memory battery
  void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Report("mmap anon PROT_READ|WRITE|EXEC", p);
  if (p != MAP_FAILED) {
    std::memcpy(p, kReturn42, sizeof kReturn42);
    __builtin___clear_cache(static_cast<char*>(p), static_cast<char*>(p) + 8);
    TryExecute("  anon RWX: call the stub", p);
    munmap(p, 4096);
  }

  // The two-step transition, which is what a real code cache does: write, then make executable.
  p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Report("mmap anon PROT_READ|WRITE (for later mprotect RX)", p);
  if (p != MAP_FAILED) {
    std::memcpy(p, kReturn42, sizeof kReturn42);
    const int rc = mprotect(p, 4096, PROT_READ | PROT_EXEC);
    if (rc != 0) {
      ++total_denied;
      LOG("%-52s DENIED  errno=%d (%s)", "mprotect RW -> RX", errno, std::strerror(errno));
    } else {
      ++total_ok;
      LOG("%-52s ok", "mprotect RW -> RX");
      __builtin___clear_cache(static_cast<char*>(p), static_cast<char*>(p) + 8);
      TryExecute("  after RW->RX: call the stub", p);
      const int back = mprotect(p, 4096, PROT_READ | PROT_WRITE);
      LOG("%-52s %s errno=%d", "mprotect RX -> RW again (patching an existing block)",
          back == 0 ? "ok" : "DENIED", back == 0 ? 0 : errno);
    }
    munmap(p, 4096);
  }

  // ---- memfd: the path a code cache uses when it wants a named, shareable executable region
  int fd = MemfdCreate("rbprobe-code", MFD_CLOEXEC);
  if (fd < 0) {
    ++total_denied;
    LOG("%-52s DENIED  errno=%d (%s)", "memfd_create(MFD_CLOEXEC)", errno, std::strerror(errno));
  } else {
    ++total_ok;
    LOG("memfd_create(MFD_CLOEXEC)                        ok      fd=%d", fd);
    if (ftruncate(fd, 4096) == 0) {
      void* m = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      Report("mmap memfd PROT_READ|WRITE (MAP_SHARED)", m);
      if (m != MAP_FAILED) {
        std::memcpy(m, kReturn42, sizeof kReturn42);
        const int rx = mprotect(m, 4096, PROT_READ | PROT_EXEC);
        if (rx == 0) {
          ++total_ok;
          LOG("%-52s ok", "mprotect memfd RW -> RX");
          __builtin___clear_cache(static_cast<char*>(m), static_cast<char*>(m) + 8);
          TryExecute("  memfd RX: call the stub", m);
        } else {
          ++total_denied;
          LOG("%-52s DENIED  errno=%d (%s)", "mprotect memfd RW -> RX", errno,
              std::strerror(errno));
        }
        munmap(m, 4096);
      }
    }
    // The one-shot version: map it executable from the start.
    void* mx = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED, fd, 0);
    Report("mmap memfd PROT_READ|WRITE|EXEC (MAP_SHARED)", mx);
    if (mx != MAP_FAILED) {
      std::memcpy(mx, kReturn42, sizeof kReturn42);
      __builtin___clear_cache(static_cast<char*>(mx), static_cast<char*>(mx) + 8);
      TryExecute("  memfd RWX: call the stub", mx);
      munmap(mx, 4096);
    }
    close(fd);
  }

  // ---- U-2: the 4 GiB low reservation the flat address model is built on
  //
  // A hint alone proves nothing. `mmap` with a non-zero address and no `MAP_FIXED*` treats the argument
  // as a suggestion, and the first run of this probe came back with `addr=0x7b00000000` for a request
  // that said `0x10000` — a 4 GiB reservation *somewhere in the 47-bit space*, which is not the thing
  // AD-6 needs. So the tests that matter insist, with `MAP_FIXED_NOREPLACE` (Linux 4.17+; this kernel
  // reports 5.15): either the window lands exactly there, or it is a denial.
#if defined(__arm__)
  // The whole low-4 GiB battery is skipped in the 32-bit build, and it is worth saying why in the output
  // rather than leaving an absence to be interpreted. U-2 asks whether a *64-bit* process can get a
  // contiguous 4 GiB window out of the low end of its address space; a 32-bit process has no address
  // space above 4 GiB to ask about, and `4 * kGiB` in a `size_t` on that build is not a 4 GiB request but
  // an arithmetic overflow to zero. The answers that *are* about this build — may a 32-bit app process
  // execute memory it mapped, and what does the app's seccomp filter do to an ARM EABI `svc` — are below.
  LOG("--- low 4 GiB reservation: skipped, see the arm64 report (U-2 is a 64-bit-host question)");
#else
  LOG("--- the low 4 GiB, described before it is tested");
  DescribeLowAddressSpace();

  // A single page, insisted on. If this succeeds where 512 MiB failed, the low range is *partly* free
  // and the obstacle is contiguity; if it fails too, the range is reserved as a matter of policy and no
  // arrangement of sizes helps. The two cases have different fallbacks, so they must be told apart.
  for (const std::uintptr_t at : {std::uintptr_t{0x1000}, std::uintptr_t{0x10000},
                                  std::uintptr_t{0x80000000}}) {
    char label[96];
    std::snprintf(label, sizeof label, "mmap 4 KiB PROT_NONE at 0x%x, MAP_FIXED_NOREPLACE",
                  static_cast<unsigned>(at));
    Report(label, mmap(reinterpret_cast<void*>(at), 4096, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
  }

  struct Sized {
    const char* label;
    std::uintptr_t hint;
    size_t len;
    bool insist;
  } sizes[] = {
      {"mmap 4 GiB PROT_NONE at 0x10000, MAP_FIXED_NOREPLACE", 0x10000u, 4u * kGiB, true},
      {"mmap 4 GiB PROT_NONE at 0x10000000, MAP_FIXED_NOREPLACE", 0x10000000u, 4u * kGiB, true},
      {"mmap 4 GiB PROT_NONE at 0x80000000, MAP_FIXED_NOREPLACE", 0x80000000u, 4u * kGiB, true},
      {"mmap 4 GiB PROT_NONE at 0x10000, hint only", 0x10000u, 4u * kGiB, false},
      {"mmap 4 GiB PROT_NONE anywhere", 0u, 4u * kGiB, false},
      {"mmap 2 GiB PROT_NONE at 0x10000, MAP_FIXED_NOREPLACE", 0x10000u, 2u * kGiB, true},
      {"mmap 1 GiB PROT_NONE at 0x10000, MAP_FIXED_NOREPLACE", 0x10000u, 1u * kGiB, true},
      {"mmap 512 MiB PROT_NONE at 0x10000, MAP_FIXED_NOREPLACE", 0x10000u, 512u * 1024u * 1024u, true},
  };
  for (const auto& s : sizes) {
    void* r = mmap(reinterpret_cast<void*>(s.hint), s.len, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (s.insist ? MAP_FIXED_NOREPLACE : 0),
                   -1, 0);
    Report(s.label, r);
    if (r != MAP_FAILED) {
      // The hint-only variants are only worth anything if they say where they actually landed: an
      // answer of "ok" at 0x7b00000000 is a *negative* result for identity addressing, and reporting
      // it as ok without the comparison is how this test passed the first time it ran.
      if (!s.insist && reinterpret_cast<std::uintptr_t>(r) != s.hint) {
        LOG("%-52s HINT NOT HONORED: got %p, wanted 0x%x", "  ^ where it actually went", r,
            static_cast<unsigned>(s.hint));
      }
      const bool below_4g =
          reinterpret_cast<std::uintptr_t>(r) + s.len <= (1ull << 32);
      LOG("  %-50s %s", "entirely below 4 GiB (identity-addressable)", below_4g ? "yes" : "no");
      // The operation the design actually needs is not the reservation but the *commit inside it*: a
      // guest page mapped executable at a low host address is what removes the bias addition from
      // every memory access (AD-6). Reserving and then failing to commit would be worse than useless.
      void* commit = mmap(r, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
      Report("  commit 4 KiB RWX inside the reservation", commit);
      if (commit != MAP_FAILED) {
        std::memcpy(commit, kReturn42, sizeof kReturn42);
        __builtin___clear_cache(static_cast<char*>(commit), static_cast<char*>(commit) + 8);
        TryExecute("    stub inside the reservation", commit);
      }
      munmap(r, s.len);
    }
  }

#endif  // the low-4 GiB battery, arm64-only

#if defined(__arm__)
  GuestAbiProbe();
#endif

  LOG("=== totals: ok=%llu denied=%llu faulted_or_wrong=%llu ===", static_cast<unsigned long long>(total_ok),
      static_cast<unsigned long long>(total_denied), static_cast<unsigned long long>(total_faulted));
  LOG("note: 'DENIED' is an answer, not a crash. The surprising outcome is an ok that cannot execute.");
}
