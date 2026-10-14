/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The importer of `link_ifunc_leaf.c`'s IFUNC. Two references to it, through two relocation kinds, so the
 * linker is asked for the name twice:
 *
 *  - `ifunc_user_call` reaches `ifunc_add` through the PLT (`R_ARM_JUMP_SLOT`). Bound to the resolver, it
 *    returns a code pointer plus one instead of `2a + 1`.
 *  - `g_ifunc_add_ptr` is a data word holding the function's address (`R_ARM_ABS32`). bionic gives it the
 *    implementation too — a function pointer compared or stored by the program must be the same one the
 *    calls reach — and it is the second question that lets a row see the resolver ran only once.
 */

extern int ifunc_add(int, int);

int (*volatile g_ifunc_add_ptr)(int, int) = ifunc_add;

int ifunc_user_call(int a) { return ifunc_add(a, a) + 1; }
