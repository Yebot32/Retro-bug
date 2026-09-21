/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The dependent half of C06's two-library chain. Everything interesting here is a *reference into the
 * other module*, which is what a per-image resolver could not answer and a linker must.
 *
 *  - `mid_call` reaches `leaf_add` through the PLT, so loading this image at all is evidence the symbol
 *    was found in a scope wider than the file itself; calling it is evidence the address was the right
 *    one and in the right ISA state.
 *  - `mid_init` records what it can see of `g_leaf_ran` **at the moment it runs**. If the linker ran
 *    initializers in the wrong order, that word reads 0 and no amount of re-reading the module list
 *    hides it — the ordering claim becomes a value in guest memory rather than a property of a vector.
 */

extern int leaf_add(int, int);
extern volatile unsigned g_leaf_ran;

volatile unsigned g_mid_ran = 0;
volatile unsigned g_leaf_seen_by_mid = 0;

void __attribute__((constructor)) mid_init(void) {
  g_leaf_seen_by_mid = g_leaf_ran;
  g_mid_ran = 0xB22B22u + (unsigned)leaf_add(2, 3);
}

int mid_call(int a) { return leaf_add(a, a) + 1; }
