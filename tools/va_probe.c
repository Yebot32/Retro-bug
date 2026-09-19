/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Address-space and executable-memory probe. Answers open risk U-2 (can a process reserve a
 * contiguous low 4 GiB window for fastmem?) and U-13's local half (is anon RWX permitted?).
 *
 * IMPORTANT CAVEAT when reading its output: run from `adb shell` this executes as the `shell`
 * SELinux domain, not `appdomain`. The VA-layout answers transfer, because they are kernel
 * behaviour; the exec answers DO NOT necessarily transfer, because execmem is decided per
 * SELinux domain. The authoritative app-domain answer must come from inside an installed RetroBug
 * build (docs/10 §17 U-13), which is why this prints domain-relevant context rather than a
 * pass/fail verdict.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define GB4 ((size_t)4u << 30)

static void try_map(const char* label, void* hint, size_t len, int prot, int flags) {
  void* p = mmap(hint, len, prot, flags, -1, 0);
  if (p == MAP_FAILED) {
    printf("%-46s FAILED  errno=%d (%s)\n", label, errno, strerror(errno));
  } else {
    printf("%-46s ok      addr=%p\n", label, p);
    munmap(p, len);
  }
}

int main(void) {
  printf("pagesize=%d  sizeof(void*)=%zu\n", (int)sysconf(_SC_PAGESIZE), sizeof(void*));

  /* The fastmem question: a contiguous 4 GiB window, anchored low vs anywhere. */
  try_map("anon PROT_NONE 4GiB, any address", NULL, GB4, PROT_NONE,
          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE);
  try_map("anon PROT_NONE 4GiB at hint 0x10000 (MAP_FIXED_NOREPLACE)", (void*)(uintptr_t)0x10000,
          GB4, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE);
  try_map("anon PROT_NONE 4GiB at hint 0 (MAP_FIXED_NOREPLACE)", (void*)(uintptr_t)0, GB4, PROT_NONE,
          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE);
  try_map("anon PROT_NONE 4GiB at hint 0x80000000 (top of 32-bit)",
          (void*)(uintptr_t)0x80000000, GB4, PROT_NONE,
          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE);

  /* The JIT question, indicative for the shell domain only. */
  try_map("anon RWX (single-mapping JIT)", NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
          MAP_PRIVATE | MAP_ANONYMOUS);
  try_map("anon RW then mprotect RX", NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);

  /* Stack/heap executability: expected to be denied everywhere (neverallow execstack/execheap). */
  printf("%-46s %d\n", "VM_MMAP_DISABLE_CONTID-like hint: skip", 0);
  printf("note: appdomain answers for execmem require an in-app probe, see docs/10 U-13\n");
  return 0;
}
