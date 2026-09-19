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

/// The 42-returning arm64 stub, and the guard that lets a faulting execute be *reported* rather than
/// being the last thing the probe ever says.
constexpr std::uint32_t kReturn42[2] = {0x5280'0540u, 0xD65F'03C0u}; /* mov w0, #42 ; ret */

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
    const auto fn = reinterpret_cast<int (*)()>(reinterpret_cast<std::uintptr_t>(at));
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

constexpr std::uintptr_t kGiB = 1024u * 1024u * 1024u;

/**
 * What occupies the low 4 GiB of this process, and how large the biggest free gap there is.
 *
 * `MAP_FIXED_NOREPLACE` answered `EEXIST` at 0x10000, 0x10000000 and 0x80000000 — for sizes down to 512
 * MiB — in the app domain, while the same binary from `adb shell` reserved at 0x10000 without trouble.
 * `EEXIST` says "something is there"; it does not say what, how much, or whether it can be given back.
 * Those are precisely the questions the identity-vs-biased address model turns on, so this reads
 * `/proc/self/maps` and answers them directly instead of leaving the inference to a reader.
 */
void DescribeLowAddressSpace() {
  FILE* f = std::fopen("/proc/self/maps", "re");
  if (f == nullptr) {
    LOG("%-52s errno=%d (%s)", "open /proc/self/maps", errno, std::strerror(errno));
    return;
  }
  char line[512];
  std::uintptr_t lowest = ~0ull, highest_low_end = 0, count_low = 0, bytes_low = 0;
  std::uintptr_t prev_end = 0;
  std::uintptr_t biggest_gap = 0, biggest_gap_at = 0;
  bool first = true;
  int printed = 0;
  /** Considers one candidate gap; kept as a lambda so the space *before* the first mapping and
   * *after* the last is measured the same way as the spaces between them. Leaving the tail out is how
   * a 324 MiB window below 4 GiB would go uncounted, and the tail is a real candidate. */
  auto consider = [&](std::uintptr_t from, std::uintptr_t to) {
    if (to <= from || to - from <= biggest_gap) return;
    biggest_gap = to - from;
    biggest_gap_at = from;
  };
  while (std::fgets(line, sizeof line, f) != nullptr) {
    std::uintptr_t start = 0, end = 0;
    if (std::sscanf(line, "%lx-%lx", &start, &end) != 2) continue;
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
  const std::uintptr_t gap = biggest_gap_at;
  const std::uintptr_t gap_size = biggest_gap;
  if (gap != 0 && gap_size >= (16u << 20)) {
    // Bounded by the gap the search found, not by 4 GiB: bisecting past it would just rediscover that
    // the neighbouring mapping is there, and the answer would be the same but slower.
    std::uintptr_t lo = 0, hi = gap_size > (4u * kGiB) ? (4u * kGiB) : gap_size;
    // Logged every step, because the first version of this loop never terminated: `mid` is rounded down
    // to a 16 MiB granularity, so once `hi - lo` got below 32 MiB the rounded midpoint equalled `lo`,
    // the map succeeded, `lo` did not move, and the `while` condition stayed true forever. The process
    // produced no signal and no tombstone — it simply stopped answering, which is the worst kind of
    // hang to debug on a device nobody is watching. A search must break when a step stops making
    // progress, not merely be capped.
    for (int step = 0; lo + (16u << 20) < hi; ++step) {
      const std::uintptr_t mid = (lo + hi) / 2u & ~((16u << 20) - 1u);
      if (mid == lo || mid == hi) break;
      void* t = mmap(reinterpret_cast<void*>(gap), mid, PROT_NONE,
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
    LOG("  %-50s %llu MiB at 0x%lx", "largest contiguous low window obtainable",
        static_cast<unsigned long long>(lo / (1024u * 1024u)), static_cast<unsigned long>(gap));
    LOG("  %-50s %s", "guest 32-bit space reachable without a bias",
        lo >= (4u * kGiB) ? "all of it" : "PARTIAL — biased guest base is required on Android");
  }
}

int MemfdCreate(const char* name, unsigned flags) {
  return static_cast<int>(syscall(SYS_memfd_create, name, flags));
}

}  // namespace

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

  LOG("=== totals: ok=%llu denied=%llu faulted_or_wrong=%llu ===", static_cast<unsigned long long>(total_ok),
      static_cast<unsigned long long>(total_denied), static_cast<unsigned long long>(total_faulted));
  LOG("note: 'DENIED' is an answer, not a crash. The surprising outcome is an ok that cannot execute.");
}
