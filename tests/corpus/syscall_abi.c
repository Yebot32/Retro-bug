/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * What a syscall looks like from each side of the boundary, measured rather than transcribed.
 *
 * C07's invariant says guest-visible `errno` values are ARM-EABI numbers and not host ones, and that the
 * map is *data*. Both halves need numbers nobody has typed from memory:
 *
 *   - the guest side is the ARM EABI table, which the AArch64 *generic* table disagrees with in more
 *     places than anyone wants to hold in their head (`ENAMETOOLONG`, `EOVERFLOW`, and the usual
 *     suspects);
 *   - the host side is whatever the arm64 build's own headers say, which is the same file set a
 *     `uname -r`-pinned NDK ships — so it moves under the project if it is guessed.
 *
 * So this prints both tables from the same source (the NDK's own headers, via the preprocessor, per ABI)
 * and `tools/syscall_abi_probe.sh` diffs the two outputs to produce the map as data. A divergence found
 * by comparison is a row in the table; a divergence that only exists in someone's recollection is a bug
 * report waiting for the first app to hit it.
 *
 * The second thing it measures is the return convention, and only on the guest side, because only the
 * guest side can be executed here: a monitor has to put back whatever a real kernel puts back, and the
 * ARM EABI story usually told is "negative errno in r0 with CPSR.C set". That is precisely the kind of
 * sentence that should be measured before it is coded, because the classic glue is `subs r2, r0, #0`,
 * which sets C for *any* non-zero result — so a successful `getpid` may well come back with C set too,
 * and then C is not the error flag at all and the discriminator is the negative range. The probe prints
 * r0 *and* C for a success that returns non-zero, a success that returns zero, and two failures, so the
 * question is settled by four lines instead of by a recollection.
 *
 * Built and run twice by tools/syscall_abi_probe.sh: once as armeabi-v7a (the guest ABI, executed on the
 * SM-X710, which has 32-bit runtime) and once as arm64 (the host ABI, executed on both devices).
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <asm/unistd.h>

static void emit(const char* name, long value) {
  printf("%-18s %ld 0x%lx\n", name, value, (unsigned long)value);
}

static void errnos() {
  /* EPERM..ELOOP in table order, then the ones the two ABIs are known to disagree about, then the
   * wide-range ARM-EABI values that a generic-numbered host would otherwise reuse wrongly. Listing the
   * aliases too (EAGAIN/EWOULDBLOCK, ENOTSUP/EOPNOTSUPP) is deliberate: if they are equal on one side
   * and not on the other, the map has to know that. */
  emit("EPERM", EPERM);
  emit("ENOENT", ENOENT);
  emit("ESRCH", ESRCH);
  emit("EINTR", EINTR);
  emit("EIO", EIO);
  emit("ENXIO", ENXIO);
  emit("E2BIG", E2BIG);
  emit("ENOEXEC", ENOEXEC);
  emit("EBADF", EBADF);
  emit("ECHILD", ECHILD);
  emit("EAGAIN", EAGAIN);
  emit("EWOULDBLOCK", EWOULDBLOCK);
  emit("ENOMEM", ENOMEM);
  emit("EACCES", EACCES);
  emit("EFAULT", EFAULT);
  emit("ENOTBLK", ENOTBLK);
  emit("EBUSY", EBUSY);
  emit("EEXIST", EEXIST);
  emit("EXDEV", EXDEV);
  emit("ENODEV", ENODEV);
  emit("ENOTDIR", ENOTDIR);
  emit("EISDIR", EISDIR);
  emit("EINVAL", EINVAL);
  emit("ENFILE", ENFILE);
  emit("EMFILE", EMFILE);
  emit("ENOTTY", ENOTTY);
  emit("ETXTBSY", ETXTBSY);
  emit("EFBIG", EFBIG);
  emit("ENOSPC", ENOSPC);
  emit("ESPIPE", ESPIPE);
  emit("EROFS", EROFS);
  emit("EMLINK", EMLINK);
  emit("EPIPE", EPIPE);
  emit("EDOM", EDOM);
  emit("ERANGE", ERANGE);
  emit("ENOSYS", ENOSYS);
  emit("ENOTEMPTY", ENOTEMPTY);
  emit("ENAMETOOLONG", ENAMETOOLONG);
  emit("ENOTSUP", ENOTSUP);
  emit("EOPNOTSUPP", EOPNOTSUPP);
  emit("EOVERFLOW", EOVERFLOW);
  emit("ECANCELED", ECANCELED);
  emit("EILSEQ", EILSEQ);
  emit("EBADFD", EBADFD);
  emit("EPROTO", EPROTO);
  emit("EKEYEXPIRED", EKEYEXPIRED);
  /* ERESTARTSYS and friends are kernel-internal and not in the userspace headers, which is itself the
   * point of printing this list from a compiler rather than from a document: what is not here cannot be
   * a guest-visible value, and a map row for it would be invented. */
}

static void numbers() {
  /* The subset C07's first slice has an opinion about. A number that is absent from one ABI's header
   * simply does not print, which is itself information: a guest call with no host counterpart has to be
   * emulated or refused, never forwarded. */
#ifdef __NR_read
  emit("__NR_read", __NR_read);
#endif
#ifdef __NR_write
  emit("__NR_write", __NR_write);
#endif
#ifdef __NR_close
  emit("__NR_close", __NR_close);
#endif
#ifdef __NR_mmap
  emit("__NR_mmap", __NR_mmap);
#endif
#ifdef __NR_mmap2
  emit("__NR_mmap2", __NR_mmap2);
#endif
#ifdef __NR_munmap
  emit("__NR_munmap", __NR_munmap);
#endif
#ifdef __NR_mprotect
  emit("__NR_mprotect", __NR_mprotect);
#endif
#ifdef __NR_brk
  emit("__NR_brk", __NR_brk);
#endif
#ifdef __NR_openat
  emit("__NR_openat", __NR_openat);
#endif
#ifdef __NR_gettid
  emit("__NR_gettid", __NR_gettid);
#endif
#ifdef __NR_getpid
  emit("__NR_getpid", __NR_getpid);
#endif
#ifdef __NR_clock_gettime
  emit("__NR_clock_gettime", __NR_clock_gettime);
#endif
#ifdef __NR_exit_group
  emit("__NR_exit_group", __NR_exit_group);
#endif
#ifdef __NR_prctl
  emit("__NR_prctl", __NR_prctl);
#endif
#ifdef __NR_futex
  emit("__NR_futex", __NR_futex);
#endif
#ifdef __NR_sigaction
  emit("__NR_sigaction", __NR_sigaction);
#endif
#ifdef __NR_rt_sigaction
  emit("__NR_rt_sigaction", __NR_rt_sigaction);
#endif
#ifdef __NR_arm_cacheflush
  emit("__NR_arm_cacheflush", __NR_arm_cacheflush);
#endif
#ifdef __ARM_NR_cacheflush
  emit("__ARM_NR_cacheflush", __ARM_NR_cacheflush);
#endif
#ifdef __ARM_NR_get_tls
  emit("__ARM_NR_get_tls", __ARM_NR_get_tls);
#endif
#ifdef __ARM_NR_set_tls
  emit("__ARM_NR_set_tls", __ARM_NR_set_tls);
#endif
#ifdef __ARM_NR_base
  emit("__ARM_NR_base", __ARM_NR_base);
#endif
#ifdef __ARM_NR_BREAKPOINT_IA
  emit("__ARM_NR_BREAKPOINT_IA", __ARM_NR_BREAKPOINT_IA);
#endif
}

#if defined(__arm__)
/*
 * One raw syscall, printing what the kernel actually left behind.
 *
 * `svc` and `mrs` in a single asm statement, deliberately: the C bit is the thing being observed, and an
 * ordinary C expression between the two can set flags and destroy it. The compiler also has to be kept
 * from choosing r7/r1/r2 for its own reasons, hence the fixed register variables.
 */
static void raw(unsigned nr, unsigned a0, unsigned a1, unsigned a2, const char* label) {
  register unsigned r0 asm("r0") = a0;
  register unsigned r1 asm("r1") = a1;
  register unsigned r2 asm("r2") = a2;
  register unsigned r3 asm("r3") = 0;
  unsigned cpsr = 0;
  /* `r7` is the syscall-number register in ARM EABI and clang's Thumb frame pointer, so it cannot be an
   * asm operand at all ("write to reserved register"). Parked and restored by hand inside the block,
   * which is also the honest shape: a real guest's `svc` clobbers nothing else, and the ARM Linux return
   * path leaves the answer in r0 with the carry bit set on error. */
  asm volatile("mov  r3, r7\n"
               "mov  r7, %[nr]\n"
               "svc  #0\n"
               "mrs  %[c], cpsr\n"
               "mov  r7, r3\n"
               : "+r"(r0), "+r"(r3), [c] "=r"(cpsr)
               : [nr] "r"(nr), "r"(r1), "r"(r2)
               : "memory", "cc");
  const int carry = (cpsr >> 29) & 1;
  printf("%-22s r7=0x%05x r0=0x%08x (%ld) C=%d\n", label, nr, r0,
         (long)(int)r0, carry);
}

static void guest_convention(void) {
  static const char missing[] = "retrobug-probe-does-not-exist";
  static char longname[300];
  unsigned i;
  for (i = 0; i + 1 < sizeof longname; ++i) longname[i] = 'a';
  longname[sizeof longname - 1] = 0;

  printf("--- raw svc, executed on ARMv7 hardware\n");
  /* Four shapes plus a fifth, chosen so that whatever turns out to be true, the lines say it rather than
   * confirming what the probe's author expected:
   *
   *   getpid        succeeds with a *non-zero* result — if C is set here, C cannot be the error flag;
   *   sched_yield   succeeds with a *zero* result     — and separates "C means error" from "C means
   *                 result != 0", which is what the classic ARM EABI glue (`subs r2, r0, #0`) would
   *                 produce;
   *   openat        fails on an unusable dirfd, with a *valid* pointer so the answer is EBADF and not
   *                 the EFAULT a null pathname gets first (the first version of this probe passed NULL
   *                 and measured the wrong thing convincingly);
   *   openat/long   fails with ENAMETOOLONG, which is the case that decides whether the kernel speaks
   *                 the ARM EABI table (62) or the table bionic's headers publish (36). Those two
   *                 numbers being different is the entire content of C07's "guest errno, not host
   *                 errno" invariant, so it is settled here rather than assumed;
   *   r7 0x0fff0    is above the table, so the kernel has no entry: ENOSYS.
   */
  raw(__NR_getpid, 0, 0, 0, "getpid (ok, non-zero)");
  raw(__NR_sched_yield, 0, 0, 0, "sched_yield (ok, zero)");
  raw(__NR_openat, 0x7fffffffu, (unsigned)(uintptr_t)missing, 0u, "openat(bad dirfd)");
  raw(__NR_openat, -100u /* AT_FDCWD */, (unsigned)(uintptr_t)longname, 0u, "openat(name too long)");
  raw(0x0fff0u, 0, 0, 0, "r7 above the table");
  printf("guest table for comparison: EBADF=%d ENAMETOOLONG=%d ENOSYS=%d EFAULT=%d\n", EBADF,
         ENAMETOOLONG, ENOSYS, EFAULT);
}
#endif

int main(void) {
  printf("# abi=");
#if defined(__aarch64__)
  printf("aarch64");
#elif defined(__arm__)
  printf("arm-eabi");
#else
  printf("other");
#endif
  printf(" sizeof(void*)=%zu\n", sizeof(void*));
  printf("--- errnos\n");
  errnos();
  printf("--- syscall numbers\n");
  numbers();
#if defined(__arm__)
  guest_convention();
#endif
  return 0;
}
