/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Ground truth for the bionic thread/static-TLS layout facts `rb::runtime::GuestProcess` relies on but
 * cannot read off `libc.so`'s code, because the *linker* computes them (StaticTlsLayout lives in
 * ld-android, not in libc): sizeof(bionic_tcb), the order and alignment of the static-TLS layout, and
 * sizeof(bionic_tls). It asks a real 32-bit process on the device about its own main thread, through
 * libc's own `__libc_get_static_tls_bounds`, `dl_iterate_phdr` (libc's TLS module offset) and TPIDRURW,
 * and checks every value against the constants in `core/runtime/guest_process.cpp`.
 * Exit 0 = every constant confirmed; 1 = a mismatch, named.
 *
 * Run (Git Bash, from the repo root; needs a device with a 32-bit ABI — R52Y106BECZ):
 *
 *   TC="$LOCALAPPDATA/Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64"
 *   "$TC/bin/clang.exe" --target=armv7-none-linux-androideabi26 --sysroot="$TC/sysroot" -O1 \
 *       -o build/tls_layout_probe tests/guest/tls_layout_probe.c
 *   MSYS_NO_PATHCONV=1 "$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe" -s R52Y106BECZ push \
 *       "$(cygpath -w build/tls_layout_probe)" /data/local/tmp/rb_tls_layout_probe
 *   MSYS_NO_PATHCONV=1 "$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe" -s R52Y106BECZ shell \
 *       "chmod 755 /data/local/tmp/rb_tls_layout_probe; /data/local/tmp/rb_tls_layout_probe; echo exit=\$?"
 *
 * The probe executable itself has no PT_TLS segment, so the linker reserves the tcb first (offset 0),
 * bionic_tls next, then libc.so's own TLS module — exactly the layout GuestProcess builds minus that module.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uintptr_t g_start;
static long g_libc_module_off = -1;
static int bad = 0;

static void expect(const char* what, unsigned long got, unsigned long want, const char* consequence) {
  printf("%-46s 0x%-8lx %s\n", what, got, got == want ? "ok" : "MISMATCH");
  if (got != want) {
    printf("    want 0x%lx: %s\n", want, consequence);
    bad = 1;
  }
}

static int on_module(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;
  (void)data;
  if (info->dlpi_tls_modid != 0 && info->dlpi_tls_data != NULL && info->dlpi_name != NULL &&
      strstr(info->dlpi_name, "/libc.so") != NULL) {
    g_libc_module_off = (long)((uintptr_t)info->dlpi_tls_data - g_start);
  }
  return 0;
}

int main(void) {
  void** tp;
  __asm__("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
  void (*bounds)(void**, void**) = (void (*)(void**, void**))dlsym(RTLD_DEFAULT, "__libc_get_static_tls_bounds");
  if (bounds == NULL) {
    printf("__libc_get_static_tls_bounds is not exported by this libc\n");
    return 1;
  }
  void *start = NULL, *end = NULL;
  bounds(&start, &end);
  g_start = (uintptr_t)start;
  const uintptr_t tcb = (uintptr_t)tp - 0xc;  /* TP = tcb + 0xc: asserted from libc's code by GuestProcess */
  const uintptr_t bionic_tls = (uintptr_t)tp[-1];
  const uintptr_t th = (uintptr_t)tp[1];
  dl_iterate_phdr(on_module, NULL);

  expect("offset_bionic_tcb (tcb - static TLS start)", tcb - g_start, 0x0, "the tcb is not reserved first");
  /* = align(sizeof(bionic_tcb), alignof(bionic_tls)): 0x2c means an 11-slot tcb and a 4-aligned bionic_tls */
  expect("offset_bionic_tls", bionic_tls - g_start, 0x2c, "sizeof(bionic_tcb) or alignof(bionic_tls) differs");
  /* libc's PT_TLS (align 8) follows bionic_tls: align8(0x2c + sizeof(bionic_tls)) == 0xb78 bounds the size to
   * (0xb44, 0xb4c]; libc's own code accesses bionic_tls+0xb49, and bionic_tls ends in char padding[2] -> 0xb4c */
  expect("libc.so TLS module offset", (unsigned long)g_libc_module_off, 0xb78, "sizeof(bionic_tls) differs from 0xb4c");
  expect("static TLS size (with libc's 0x30-byte module)", (uintptr_t)end - g_start, 0xba8, "layout size differs");
  expect("pthread_internal_t +0x298 == bionic_tls", *(uintptr_t*)(th + 0x298), bionic_tls, "field moved");
  expect("pthread_internal_t +0x2a0 == tcb", *(uintptr_t*)(th + 0x2a0), tcb, "field moved");
  expect("main thread mmap_size (+0x64)", *(uint32_t*)(th + 0x64), 0x5000, "thread mapping shape differs");
  expect("main thread static TLS start - mmap_base", g_start - *(uintptr_t*)(th + 0x60), 0x5000 - 0x3000 - 0xba8,
         "static TLS is not 0x3000 below the mapping's end");
  expect("main thread unguarded base - mmap_base (+0x68)", *(uintptr_t*)(th + 0x68) - *(uintptr_t*)(th + 0x60), 0x1000,
         "guard size differs");
  expect("main thread unguarded size (+0x6c)", *(uint32_t*)(th + 0x6c), 0x3000, "mapping shape differs");
  expect("bionic_tls+0x414 == static TLS end", *(uintptr_t*)(bionic_tls + 0x414), (uintptr_t)end,
         "scratch buffers are not at the static TLS end");
  expect("main thread attr.flags (+0x10)", *(uint32_t*)(th + 0x10), 0x4, "attr flags differ");
  expect("main thread cached_pid (+0xc) == getpid()", *(uint32_t*)(th + 0xc) & 0x7fffffffu, (unsigned long)getpid(),
         "cached_pid moved");
  expect("main thread tid (+0x8) == gettid()", *(uint32_t*)(th + 0x8), (unsigned long)gettid(), "tid moved");
  expect("main thread start_routine (+0x30)", *(uintptr_t*)(th + 0x30), 0, "is_main() differs");
  expect("main thread byte +0x2c4", *(unsigned char*)(th + 0x2c4), 0, "+0x2c4 is set on the main thread");
  expect("main thread next/prev (sole entry)", *(uintptr_t*)th | *(uintptr_t*)(th + 4), 0, "g_thread_list not single");
  printf("%s\n", bad ? "MISMATCH" : "all constants confirmed");
  return bad;
}
