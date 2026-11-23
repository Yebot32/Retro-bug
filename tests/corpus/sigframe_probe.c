/*
 * RetroBug — measure the AArch32 signal frame a handler is actually handed. SPDX-License-Identifier: Apache-2.0
 *
 * Why this exists: C26's delivery has to synthesise the frame a kernel would have made, and the project's own
 * research (docs/research/02 §5.4) says the layout is the "ugly part" and must be *verified* against the
 * kernel's headers rather than recalled. This measures it instead: it installs a handler, interrupts a known
 * instruction, and hunts the interrupted PC and SP inside the `ucontext` the kernel built. The offsets it
 * prints are the ones delivery must write, and they come from the device that will run the guest.
 *
 * Build (armv7, static, the same flags the engine lanes use):
 *   clang --target=armv7-none-linux-androideabi26 --sysroot=$SYSROOT \
 *     -mthumb -mfloat-abi=softfp -mfpu=neon -Os -static -o sigframe_probe tests/corpus/sigframe_probe.c
 * Run: adb push sigframe_probe /data/local/tmp && adb shell /data/local/tmp/sigframe_probe
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The interrupted instruction: its address is what `arm_pc` in the sigcontext must hold. `volatile` and a
 * separate noinline function so nothing folds it away or moves the value the handler looks for. */
static volatile int g_hits = 0;
static void* g_pc_at_interrupt = 0;
static void* g_sp_at_interrupt = 0;

__attribute__((noinline)) static void Interrupted(void) {
  /* Read pc and sp at a point we can name, then spin until the signal lands. `pc` here is the address of the
   * instruction after the one that reads it, which is what the kernel will have saved. */
  void* pc;
  void* sp;
  __asm__ volatile("mov %0, pc" : "=r"(pc));
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  g_pc_at_interrupt = pc;
  g_sp_at_interrupt = sp;
  while (g_hits == 0) {
    /* The signal is delivered here, somewhere in this loop. */
  }
}

static void Handler(int sig, siginfo_t* info, void* ucontext) {
  (void)sig;
  (void)info;
  g_hits = 1;

  printf("=== AArch32 signal frame as a handler sees it ===\n");
  printf("handler called: sig=%d info=%p ucontext=%p\n", sig, (void*)info, ucontext);

  /* The ucontext the kernel handed us. Everything below is read out of it, so these offsets are measured. */
  unsigned long* uc = (unsigned long*)ucontext;
  const unsigned long want_pc = (unsigned long)g_pc_at_interrupt;
  const unsigned long want_sp = (unsigned long)g_sp_at_interrupt;
  printf("looking for pc=0x%08lx sp=0x%08lx\n", want_pc, want_sp);

  /* 0..248 words is well past sizeof(ucontext_t) (744 bytes = 186 words) plus the siginfo in front. Scan far
   * enough to catch a frame that starts at a negative offset from the pointer we were given. */
  for (int i = -32; i < 256; i++) {
    if (uc[i] == want_pc) printf("  ucontext%+d words (%+d bytes) = interrupted PC\n", i, i * 4);
    if (uc[i] == want_sp) printf("  ucontext%+d words (%+d bytes) = interrupted SP\n", i, i * 4);
  }

  /* The sigcontext starts at a fixed place in bionic's ucontext_t; print the words around where the struct
   * header says, so the two can be compared rather than trusted. */
  printf("first 40 words of ucontext:\n");
  for (int i = 0; i < 40; i += 4) {
    printf("  +%3d: %08lx %08lx %08lx %08lx\n", i * 4, uc[i], uc[i + 1], uc[i + 2], uc[i + 3]);
  }
  printf("sizeof(siginfo_t)=%zu sizeof(ucontext_t)=%zu sizeof(stack_t)=%zu\n", sizeof(siginfo_t),
         sizeof(ucontext_t), sizeof(stack_t));
  printf("=== end ===\n");
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = Handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGUSR1, &sa, NULL) != 0) {
    perror("sigaction");
    return 1;
  }

  /* A child thread does the waiting; the main thread signal it, so the frame is built for a real thread the
   * way Mono's suspend will be. */
  pid_t self = (pid_t)syscall(__NR_gettid);

  /* Interrupt *this* thread from a second thread created with clone, which is what an emulator's guest thread
   * pair looks like. Simplest portable form here: fork a helper that signals us by tid. */
  pid_t helper = fork();
  if (helper == 0) {
    usleep(200000);
    syscall(__NR_tkill, self, SIGUSR1);
    _exit(0);
  }
  Interrupted();
  printf("handler ran: %d hit(s)\n", g_hits);
  return 0;
}
