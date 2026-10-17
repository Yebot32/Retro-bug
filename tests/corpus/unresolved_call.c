/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Two strong imports nothing defines, each called from its own export. A process with an unresolved-import
 * trap binds both to it and loads the library; a call must then fault naming the import it reached — the
 * second one by its own name, not the first's (Angry Birds Go, 2026-10-17: a GL-thread native faulted at
 * the trap with no name among 78 candidates). Built `-nostdlib` with no `--no-undefined`, so the two stay
 * undefined in `.dynsym` and reach the loader's resolver.
 */

extern int rb_never_defined_alpha(int);
extern int rb_never_defined_beta(int);

int uc_call_alpha(int x) { return rb_never_defined_alpha(x) + 1; }

int uc_call_beta(int x) { return rb_never_defined_beta(x) + 2; }
