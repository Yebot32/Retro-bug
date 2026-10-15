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

int pt_create_join(int n);
int pt_mutex_contend(int threads, int iters);
int pt_condvar_pingpong(int rounds);
int pt_keys_and_destructors(void);
int pt_detached(int n);
int pt_once(void);
int pt_errno_per_thread(void);

int main(void) {
  printf("pt_create_join(8)=%d\n", pt_create_join(8));
  printf("pt_mutex_contend(2,100000)=%d\n", pt_mutex_contend(2, 100000));
  printf("pt_mutex_contend(4,2000)=%d\n", pt_mutex_contend(4, 2000));
  printf("pt_condvar_pingpong(500)=%d\n", pt_condvar_pingpong(500));
  printf("pt_keys_and_destructors()=%d\n", pt_keys_and_destructors());
  printf("pt_detached(8)=%d\n", pt_detached(8));
  printf("pt_once()=%d\n", pt_once());
  printf("pt_errno_per_thread()=%d\n", pt_errno_per_thread());
  return 0;
}
