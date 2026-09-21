/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The leaf of a two-library guest dependency chain, built for C06's tests.
 *
 * Deliberately compiled `-nostdlib`: a chain that drags in `libc.so` cannot tell "the linker resolved
 * the graph" from "the loader happened to get lucky with a big library", and its constructor would need
 * `__cxa_atexit`. In C, `__attribute__((constructor))` still lands in `.init_array`, which is the whole
 * mechanism under test.
 */

volatile unsigned g_leaf_ran = 0;

void __attribute__((constructor)) leaf_init(void) { g_leaf_ran = 0xA11A11u; }

int leaf_add(int a, int b) { return a + b; }
