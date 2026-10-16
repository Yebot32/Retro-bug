/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * A library whose constructors fault part-way: DT_INIT_ARRAY[0] counts itself in `g_ctor_runs`, then
 * DT_INIT_ARRAY[1] branches to an address no guest window maps. `GuestProcess::RunConstructors` must name
 * the fault, and a second `RunConstructors` must refuse rather than run entry 0 again (the counter stays 1).
 */

volatile int g_ctor_runs = 0;

/// Far above a 64 MiB window and the 0x0bad0000 trap page: a call there faults by name.
static void (*volatile g_nowhere)(void) = (void (*)(void))0x0badf000;

__attribute__((constructor(101))) static void counts(void) { g_ctor_runs = g_ctor_runs + 1; }

__attribute__((constructor(102))) static void faults(void) { g_nowhere(); }
