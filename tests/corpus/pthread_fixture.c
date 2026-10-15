/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * M2 Task 4: guest pthreads. `libpthread_fixture.so` is the device's own bionic pthread layer driven from
 * guest code: every function here creates real guest threads (pthread_create -> clone 0x003d0f00), and
 * synchronises them through bionic's own mutex / condvar / once / key / barrier code (futex WAIT/WAKE,
 * the kuser helpers, ldrex/strex). The same .so is run natively on the 32-bit-capable tablet by
 * tests/guest/pthread_oracle.c (tools/run_pthread_oracle.sh -> tests/corpus/oracle-pthread.txt), and
 * core/tests/guest_thread_test.cpp asserts both tiers give the oracle's numbers.
 *
 * Every result is a count the program determines, never a timing: a row that is right on hardware is right
 * whatever the interleaving was. A thread that fails a pthread call makes its row's result negative (the
 * failing call's number), so a broken primitive cannot pass as a smaller count.
 *
 * Built -nostdlib against the NDK's stub libc (its DT_NEEDED is libc.so, which the guest process's scope
 * answers with the device's own libc).
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS 16

/* ---- pt_create_join: n threads, each returns (i + 1) * 10; the sum of what pthread_join collected. */
static void* ret_index(void* arg) { return (void*)(((intptr_t)arg + 1) * 10); }

__attribute__((visibility("default"))) int pt_create_join(int n) {
  if (n < 1 || n > MAX_THREADS) return -1;
  pthread_t t[MAX_THREADS];
  for (int i = 0; i < n; ++i) {
    if (pthread_create(&t[i], 0, ret_index, (void*)(intptr_t)i) != 0) return -2;
  }
  int sum = 0;
  for (int i = 0; i < n; ++i) {
    void* r = 0;
    if (pthread_join(t[i], &r) != 0) return -3;
    sum += (int)(intptr_t)r;
  }
  return sum;
}

/* ---- pt_mutex_contend: `threads` threads each lock / ++counter / unlock `iters` times. The counter is a
 * plain int read and written in two steps, so only the mutex keeps it exact. */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_counter;
static int g_iters;

static void* contend(void* arg) {
  (void)arg;
  for (int i = 0; i < g_iters; ++i) {
    if (pthread_mutex_lock(&g_mu) != 0) return (void*)-4;
    const int v = g_counter;
    g_counter = v + 1;
    if (pthread_mutex_unlock(&g_mu) != 0) return (void*)-5;
  }
  return 0;
}

__attribute__((visibility("default"))) int pt_mutex_contend(int threads, int iters) {
  if (threads < 1 || threads > MAX_THREADS) return -1;
  g_counter = 0;
  g_iters = iters;
  pthread_t t[MAX_THREADS];
  for (int i = 0; i < threads; ++i) {
    if (pthread_create(&t[i], 0, contend, 0) != 0) return -2;
  }
  int bad = 0;
  for (int i = 0; i < threads; ++i) {
    void* r = 0;
    if (pthread_join(t[i], &r) != 0) return -3;
    if (r != 0) bad = (int)(intptr_t)r;
  }
  return bad != 0 ? bad : g_counter;
}

/* ---- pt_condvar_pingpong: two threads hand a token back and forth through one condvar, `rounds` times
 * each; the result is the number of handoffs (2 * rounds when every wake arrived). */
static pthread_mutex_t g_pp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pp_cv = PTHREAD_COND_INITIALIZER;
static int g_turn;
static int g_handoffs;
static int g_rounds;

static void* pingpong(void* arg) {
  const int me = (int)(intptr_t)arg;
  for (int i = 0; i < g_rounds; ++i) {
    if (pthread_mutex_lock(&g_pp_mu) != 0) return (void*)-4;
    while (g_turn != me) {
      if (pthread_cond_wait(&g_pp_cv, &g_pp_mu) != 0) {
        pthread_mutex_unlock(&g_pp_mu);
        return (void*)-6;
      }
    }
    g_turn = 1 - me;
    ++g_handoffs;
    pthread_cond_broadcast(&g_pp_cv);
    pthread_mutex_unlock(&g_pp_mu);
  }
  return 0;
}

__attribute__((visibility("default"))) int pt_condvar_pingpong(int rounds) {
  g_turn = 0;
  g_handoffs = 0;
  g_rounds = rounds;
  pthread_t a, b;
  if (pthread_create(&a, 0, pingpong, (void*)0) != 0) return -2;
  if (pthread_create(&b, 0, pingpong, (void*)1) != 0) return -2;
  void* ra = 0;
  void* rb = 0;
  if (pthread_join(a, &ra) != 0 || pthread_join(b, &rb) != 0) return -3;
  if (ra != 0) return (int)(intptr_t)ra;
  if (rb != 0) return (int)(intptr_t)rb;
  return g_handoffs;
}

/* ---- pt_keys_and_destructors: 5 threads; 4 set a non-null value for the key (and read it back), one
 * leaves it null. bionic runs the destructor for each non-null value at thread exit: the result is the
 * destructor-call count after every thread has been joined (4), plus 100 per thread whose getspecific
 * read back a value that was not its own (0 on a correct run). */
static pthread_key_t g_key;
static int g_destructed;
static int g_mismatch;
static pthread_mutex_t g_key_mu = PTHREAD_MUTEX_INITIALIZER;

static void key_dtor(void* v) {
  (void)v;
  pthread_mutex_lock(&g_key_mu);
  ++g_destructed;
  pthread_mutex_unlock(&g_key_mu);
}

static void* key_user(void* arg) {
  const intptr_t i = (intptr_t)arg;
  if (i == 4) return 0;  // leaves the key null: no destructor call for this one
  static int vals[MAX_THREADS];
  if (pthread_setspecific(g_key, &vals[i]) != 0) return (void*)-7;
  sched_yield();
  if (pthread_getspecific(g_key) != &vals[i]) {
    pthread_mutex_lock(&g_key_mu);
    ++g_mismatch;
    pthread_mutex_unlock(&g_key_mu);
  }
  return 0;
}

__attribute__((visibility("default"))) int pt_keys_and_destructors(void) {
  g_destructed = 0;
  g_mismatch = 0;
  if (pthread_key_create(&g_key, key_dtor) != 0) return -8;
  pthread_t t[5];
  for (int i = 0; i < 5; ++i) {
    if (pthread_create(&t[i], 0, key_user, (void*)(intptr_t)i) != 0) return -2;
  }
  for (int i = 0; i < 5; ++i) {
    void* r = 0;
    if (pthread_join(t[i], &r) != 0) return -3;
    if (r != 0) return (int)(intptr_t)r;
  }
  pthread_key_delete(g_key);
  return g_destructed + 100 * g_mismatch;
}

/* ---- pt_detached: n detached threads each add 1 under a mutex and signal; the creator waits for all n.
 * Detached threads tear down their own stacks (munmap) and exit; nothing joins them. */
static pthread_mutex_t g_det_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_det_cv = PTHREAD_COND_INITIALIZER;
static int g_det_done;

static void* detached_body(void* arg) {
  (void)arg;
  pthread_mutex_lock(&g_det_mu);
  ++g_det_done;
  pthread_cond_signal(&g_det_cv);
  pthread_mutex_unlock(&g_det_mu);
  return 0;
}

__attribute__((visibility("default"))) int pt_detached(int n) {
  if (n < 1 || n > MAX_THREADS) return -1;
  g_det_done = 0;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  for (int i = 0; i < n; ++i) {
    pthread_t t;
    if (pthread_create(&t, &attr, detached_body, 0) != 0) return -2;
  }
  pthread_attr_destroy(&attr);
  pthread_mutex_lock(&g_det_mu);
  while (g_det_done < n) pthread_cond_wait(&g_det_cv, &g_det_mu);
  const int done = g_det_done;
  pthread_mutex_unlock(&g_det_mu);
  return done;
}

/* ---- pt_once: 6 threads race pthread_once (released together by a barrier); the init routine runs once.
 * Result: 1000 * init-calls + the number of threads that saw the init's effect after pthread_once
 * returned (1006 on a correct run). */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_once_calls;
static volatile int g_once_value;
static pthread_barrier_t g_once_barrier;

static void once_init(void) {
  ++g_once_calls;
  sched_yield();  // widen the window in which another thread could wrongly run it too
  g_once_value = 42;
}

static void* once_user(void* arg) {
  (void)arg;
  pthread_barrier_wait(&g_once_barrier);
  if (pthread_once(&g_once, once_init) != 0) return (void*)-9;
  return (void*)(intptr_t)(g_once_value == 42 ? 1 : 0);
}

__attribute__((visibility("default"))) int pt_once(void) {
  g_once_calls = 0;
  g_once_value = 0;
  g_once = (pthread_once_t)PTHREAD_ONCE_INIT;
  if (pthread_barrier_init(&g_once_barrier, 0, 6) != 0) return -10;
  pthread_t t[6];
  for (int i = 0; i < 6; ++i) {
    if (pthread_create(&t[i], 0, once_user, 0) != 0) return -2;
  }
  int saw = 0;
  for (int i = 0; i < 6; ++i) {
    void* r = 0;
    if (pthread_join(t[i], &r) != 0) return -3;
    if ((intptr_t)r < 0) return (int)(intptr_t)r;
    saw += (int)(intptr_t)r;
  }
  pthread_barrier_destroy(&g_once_barrier);
  return 1000 * g_once_calls + saw;
}

/* ---- pt_errno_per_thread: 4 threads each set errno to their own value, meet at a barrier (so every one
 * has set its value before any reads back), then check errno is still theirs. The main thread's errno is
 * set before and checked after. Result: threads whose errno survived + 10 if main's did (14). */
static pthread_barrier_t g_errno_barrier;

static void* errno_user(void* arg) {
  const int mine = 1000 + (int)(intptr_t)arg;
  errno = mine;
  pthread_barrier_wait(&g_errno_barrier);
  sched_yield();
  return (void*)(intptr_t)(errno == mine ? 1 : 0);
}

__attribute__((visibility("default"))) int pt_errno_per_thread(void) {
  if (pthread_barrier_init(&g_errno_barrier, 0, 4) != 0) return -10;
  errno = 77;
  pthread_t t[4];
  for (int i = 0; i < 4; ++i) {
    if (pthread_create(&t[i], 0, errno_user, (void*)(intptr_t)i) != 0) return -2;
  }
  int ok = 0;
  for (int i = 0; i < 4; ++i) {
    void* r = 0;
    if (pthread_join(t[i], &r) != 0) return -3;
    ok += (int)(intptr_t)r;
  }
  const int main_ok = errno == 77 ? 10 : 0;
  pthread_barrier_destroy(&g_errno_barrier);
  return ok + main_ok;
}

/* ---- pt_timeouts: the timed waits time out. A condvar nobody signals, waited on for 50 ms (absolute
 * CLOCK_REALTIME deadline: futex WAIT_BITSET | CLOCK_REALTIME), and a mutex another thread holds, timed-locked
 * for 50 ms. Result: 1000 * cond_timedwait's return + mutex_timedlock's (110110: ETIMEDOUT twice). */
static pthread_mutex_t g_to_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_to_held = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_to_cv = PTHREAD_COND_INITIALIZER;

static struct timespec in_ms(clockid_t clock, int ms) {
  struct timespec ts;
  clock_gettime(clock, &ts);
  ts.tv_nsec += (long)ms * 1000000L;
  while (ts.tv_nsec >= 1000000000L) {
    ts.tv_nsec -= 1000000000L;
    ts.tv_sec += 1;
  }
  return ts;
}

static long long ms_between(const struct timespec* a, const struct timespec* b) {
  return (long long)(b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000;
}

static void* timedlocker(void* arg) {
  (void)arg;
  const struct timespec ts = in_ms(CLOCK_REALTIME, 50);
  const int rc = pthread_mutex_timedlock(&g_to_held, &ts);
  if (rc == 0) pthread_mutex_unlock(&g_to_held);
  return (void*)(intptr_t)rc;
}

__attribute__((visibility("default"))) int pt_timeouts(void) {
  struct timespec m0, m1, m2;
  clock_gettime(CLOCK_MONOTONIC, &m0);
  pthread_mutex_lock(&g_to_mu);
  const struct timespec ts = in_ms(CLOCK_REALTIME, 50);
  const int rc1 = pthread_cond_timedwait(&g_to_cv, &g_to_mu, &ts);
  pthread_mutex_unlock(&g_to_mu);
  clock_gettime(CLOCK_MONOTONIC, &m1);
  pthread_mutex_lock(&g_to_held);
  pthread_t t;
  if (pthread_create(&t, 0, timedlocker, 0) != 0) return -2;
  void* r = 0;
  if (pthread_join(t, &r) != 0) return -3;
  pthread_mutex_unlock(&g_to_held);
  clock_gettime(CLOCK_MONOTONIC, &m2);
  // Each wait really waited (most of) its 50 ms: a timeout answered early would pass the return codes alone.
  if (ms_between(&m0, &m1) < 45 || ms_between(&m1, &m2) < 45) return -11;
  return rc1 * 1000 + (int)(intptr_t)r;
}

/* ---- pt_sleeps: nanosleep, usleep, relative and absolute clock_nanosleep and sched_yield all return 0 (one
 * each: 5), and the monotonic clock advanced by at least the 25 ms asked for (+10). Result 15. */
__attribute__((visibility("default"))) int pt_sleeps(void) {
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int ok = 0;
  const struct timespec ten = {0, 10 * 1000000L};
  ok += nanosleep(&ten, 0) == 0;
  ok += usleep(5000) == 0;
  const struct timespec five = {0, 5 * 1000000L};
  ok += clock_nanosleep(CLOCK_MONOTONIC, 0, &five, 0) == 0;
  const struct timespec until = in_ms(CLOCK_MONOTONIC, 5);
  ok += clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, 0) == 0;
  ok += sched_yield() == 0;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  const long long ms = (long long)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
  return ok + (ms >= 25 ? 10 : 0);
}

/* ---- M2 Task 5: adopted host (ART) threads. These run on whatever thread calls them — the engine lane calls
 * them from host threads that never went through pthread_create (adopted on their first guest call), the
 * native oracle from threads pthread_create made. pt_key_* share one key, created once, whose destructor counts
 * the non-null values it was run for. */
static pthread_key_t g_adopt_key;
static pthread_once_t g_adopt_once = PTHREAD_ONCE_INIT;
static int g_adopt_dtors;
static pthread_mutex_t g_adopt_mu = PTHREAD_MUTEX_INITIALIZER;

static void adopt_dtor(void* v) {
  (void)v;
  pthread_mutex_lock(&g_adopt_mu);
  ++g_adopt_dtors;
  pthread_mutex_unlock(&g_adopt_mu);
}

static void adopt_key_init(void) { pthread_key_create(&g_adopt_key, adopt_dtor); }

__attribute__((visibility("default"))) int pt_self_tid(void) { return gettid(); }

__attribute__((visibility("default"))) unsigned pt_self(void) { return (unsigned)(uintptr_t)pthread_self(); }

/* The address errno lives at for the calling thread, after a write/read round trip of `v` through it. */
__attribute__((visibility("default"))) unsigned pt_errno_slot(int v) {
  errno = v;
  sched_yield();
  return errno == v ? (unsigned)(uintptr_t)&errno : 0u;
}

__attribute__((visibility("default"))) int pt_key_set(int v) {
  if (pthread_once(&g_adopt_once, adopt_key_init) != 0) return -9;
  return pthread_setspecific(g_adopt_key, (void*)(intptr_t)v) == 0 ? 0 : -7;
}

__attribute__((visibility("default"))) int pt_key_get(void) {
  if (pthread_once(&g_adopt_once, adopt_key_init) != 0) return -9;
  return (int)(intptr_t)pthread_getspecific(g_adopt_key);
}

__attribute__((visibility("default"))) int pt_key_destructor_count(void) {
  pthread_mutex_lock(&g_adopt_mu);
  const int n = g_adopt_dtors;
  pthread_mutex_unlock(&g_adopt_mu);
  return n;
}

/* Exit-time failure modes of an adopted thread (M2 Task 5 fix round 1; engine lane only — on a device the
 * faulting one is a SIGSEGV of the process). pt_slow_key_set(n): a value whose destructor spins n iterations
 * (far past one guest call's budget for n = 1000000) and then counts itself; pt_fault_key_set(): a value whose
 * destructor stores to address 0. */
static pthread_key_t g_slow_key, g_fault_key;
static pthread_once_t g_slow_once = PTHREAD_ONCE_INIT, g_fault_once = PTHREAD_ONCE_INIT;
static volatile int g_slow_done;

static void slow_dtor(void* v) {
  for (volatile unsigned i = 0; i < (unsigned)(uintptr_t)v; ++i) {
  }
  pthread_mutex_lock(&g_adopt_mu);
  ++g_slow_done;
  pthread_mutex_unlock(&g_adopt_mu);
}

static void fault_dtor(void* v) {
  (void)v;
  *(volatile int*)(uintptr_t)v = 1;  // v is 0x10: an unmapped page, a store that faults
}

static void slow_key_init(void) { pthread_key_create(&g_slow_key, slow_dtor); }
static void fault_key_init(void) { pthread_key_create(&g_fault_key, fault_dtor); }

__attribute__((visibility("default"))) int pt_slow_key_set(int iters) {
  if (pthread_once(&g_slow_once, slow_key_init) != 0) return -9;
  return pthread_setspecific(g_slow_key, (void*)(intptr_t)iters) == 0 ? 0 : -7;
}

__attribute__((visibility("default"))) int pt_slow_dtor_count(void) {
  pthread_mutex_lock(&g_adopt_mu);
  const int n = g_slow_done;
  pthread_mutex_unlock(&g_adopt_mu);
  return n;
}

__attribute__((visibility("default"))) int pt_fault_key_set(void) {
  if (pthread_once(&g_fault_once, fault_key_init) != 0) return -9;
  return pthread_setspecific(g_fault_key, (void*)(intptr_t)0x10) == 0 ? 0 : -7;
}

/* ---- pt_exit_group_from_thread: one thread parks forever on a condvar, one spins forever with no syscall,
 * and a third calls _exit(code) (exit_group) after 20 ms, while the caller is parked in pthread_join on the
 * first. Never returns: the process ends with `code`, and every thread with it. Not part of the numeric
 * oracle run (it ends the process); the oracle records its exit status separately. */
static pthread_mutex_t g_eg_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_eg_cv = PTHREAD_COND_INITIALIZER;
static volatile unsigned g_spins;

static void* parked_forever(void* arg) {
  (void)arg;
  pthread_mutex_lock(&g_eg_mu);
  for (;;) pthread_cond_wait(&g_eg_cv, &g_eg_mu);
  return 0;
}

static void* spin_forever(void* arg) {
  (void)arg;
  for (;;) ++g_spins;
  return 0;
}

static void* exit_soon(void* arg) {
  usleep(20000);
  _exit((int)(intptr_t)arg);
  return 0;
}

static void* exit_soon_ms(void* arg) {
  usleep(300000);  // long enough for the emulator's cloner to be well into its loop (JIT warm-up included)
  _exit((int)(intptr_t)arg);
  return 0;
}

__attribute__((visibility("default"))) int pt_exit_group_from_thread(int code) {
  pthread_t a, b, c;
  if (pthread_create(&a, 0, parked_forever, 0) != 0) return -2;
  if (pthread_create(&b, 0, spin_forever, 0) != 0) return -2;
  if (pthread_create(&c, 0, exit_soon, (void*)(intptr_t)code) != 0) return -2;
  pthread_join(a, 0);
  return -1;
}

/* ---- pt_clone_storm_exit: one thread creates detached threads in a tight loop, counting each successful
 * pthread_create in `pt_storm_clones` (exported, so the engine lane reads it from guest memory after the
 * process ended); another calls _exit(code) after 300 ms; the caller is parked in pthread_join on the cloner.
 * Never returns. The lane checks that the count stops changing the moment the process exits (no guest work and
 * no successful clone after exit_group) and that no host thread outlives the process. */
__attribute__((visibility("default"))) volatile int pt_storm_clones;

static void* storm_child(void* arg) {
  (void)arg;
  return 0;
}

static void* storm_cloner(void* arg) {
  (void)arg;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 64 * 1024);
  for (;;) {
    pthread_t t;
    if (pthread_create(&t, &attr, storm_child, 0) == 0) ++pt_storm_clones;
    sched_yield();  // lets the detached children run and tear down, so clones keep succeeding up to the exit
  }
  return 0;
}

__attribute__((visibility("default"))) int pt_clone_storm_exit(int code) {
  pt_storm_clones = 0;
  pthread_t cloner, exiter;
  if (pthread_create(&cloner, 0, storm_cloner, 0) != 0) return -2;
  if (pthread_create(&exiter, 0, exit_soon_ms, (void*)(intptr_t)code) != 0) return -2;
  pthread_join(cloner, 0);
  return -1;
}
