/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The native driver for M2 Task 4's pthread oracle: links `libpthread_fixture.so` (tests/corpus/
 * pthread_fixture.c, the very bytes the engine lane loads as a guest) and prints one `row=value` line per
 * fixture call, on a real 32-bit process on the tablet (tools/run_pthread_oracle.sh writes the output to
 * tests/corpus/oracle-pthread.txt). core/tests/guest_thread_test.cpp runs the same rows — same names, same
 * arguments — on both tiers and compares against this file.
 *
 * The row list is the contract between the two: a row name is the call exactly as written here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int pt_create_join(int n);
int pt_mutex_contend(int threads, int iters);
int pt_condvar_pingpong(int rounds);
int pt_keys_and_destructors(void);
int pt_detached(int n);
int pt_once(void);
int pt_errno_per_thread(void);
int pt_timeouts(void);
int pt_sleeps(void);
int pt_exit_group_from_thread(int code);
int pt_clone_storm_exit(int code);
int pt_syscall_storm(int n);
int pt_tls_dtor_on_thread(void);


/* M2 Task 5: the native counterpart of the lane's adopted-thread row. On a device every ART thread is a real
 * pthread, so the thread pthread_create makes here is what an adopted host thread must look like to the guest.
 * Thread A: own tid (non-zero, not main's), own pthread_self, pt_key_set(7) then pt_key_get() == 7; thread B
 * (while A is alive): pt_key_get() == 0; after A is joined, main sees pt_key_destructor_count() == 1. The digits
 * of the result are those five facts (11111 when all hold) — the lane composes the same number. */
#include <pthread.h>
#include <stdint.h>

int pt_self_tid(void);
unsigned pt_self(void);
int pt_key_set(int v);
int pt_key_get(void);
int pt_key_destructor_count(void);

static int g_a_tid, g_a_get, g_b_get;
static unsigned g_a_self;

static void* thread_b(void* arg) {
  (void)arg;
  g_b_get = pt_key_get();
  return 0;
}

static void* thread_a(void* arg) {
  (void)arg;
  g_a_tid = pt_self_tid();
  g_a_self = pt_self();
  pt_key_set(7);
  g_a_get = pt_key_get();
  pthread_t b;
  pthread_create(&b, 0, thread_b, 0);  // B runs and finishes while A is alive and holds its value
  pthread_join(b, 0);
  return 0;
}

static int adopted_thread_keys(void) {
  const int main_tid = pt_self_tid();
  const unsigned main_self = pt_self();
  pthread_t a;
  if (pthread_create(&a, 0, thread_a, 0) != 0) return -2;
  pthread_join(a, 0);
  return (g_a_tid != 0 && g_a_tid != main_tid ? 1 : 0) + (g_a_self != main_self ? 10 : 0) +
         (g_a_get == 7 ? 100 : 0) + (g_b_get == 0 ? 1000 : 0) + (pt_key_destructor_count() == 1 ? 10000 : 0);
}

int main(int argc, char** argv) {
  // `exit_group <code>`: the process must end with <code> from another thread (the script records rc).
  if (argc == 3 && strcmp(argv[1], "exit_group") == 0) return pt_exit_group_from_thread(atoi(argv[2]));
  if (argc == 3 && strcmp(argv[1], "clone_storm") == 0) return pt_clone_storm_exit(atoi(argv[2]));
  printf("pt_create_join(8)=%d\n", pt_create_join(8));
  printf("pt_mutex_contend(2,100000)=%d\n", pt_mutex_contend(2, 100000));
  printf("pt_mutex_contend(4,2000)=%d\n", pt_mutex_contend(4, 2000));
  printf("pt_condvar_pingpong(500)=%d\n", pt_condvar_pingpong(500));
  printf("pt_keys_and_destructors()=%d\n", pt_keys_and_destructors());
  printf("pt_detached(8)=%d\n", pt_detached(8));
  printf("pt_once()=%d\n", pt_once());
  printf("pt_errno_per_thread()=%d\n", pt_errno_per_thread());
  printf("pt_timeouts()=%d\n", pt_timeouts());
  printf("pt_sleeps()=%d\n", pt_sleeps());
  printf("pt_syscall_storm(150000)=%d\n", pt_syscall_storm(150000));
  printf("pt_tls_dtor_on_thread()=%d\n", pt_tls_dtor_on_thread());
  printf("adopted_thread_keys()=%d\n", adopted_thread_keys());
  return 0;
}
