/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * A library whose one function is an IFUNC that the library itself never calls — the shape of bionic's
 * `strcpy`, `stpcpy` and `strcat`. Nothing in this image relocates against `ifunc_add`, so its loader
 * records no IFUNC slot for it, and `FindExport("ifunc_add")` can only answer with the *resolver*. The
 * importer next door (`link_ifunc_user.c`) is what has to be bound to the implementation instead.
 *
 * `-nostdlib`, like the C06 chain: the resolver takes no `hwcap` and needs no libc, so the binding is the
 * only thing under test.
 */

/// How many times the resolver has run, in guest memory — so a row can tell "the linker cached its
/// answer" from "the hook happened to be called once".
volatile unsigned g_ifunc_resolver_runs = 0;

static int ifunc_add_impl(int a, int b) { return a + b; }

/// Returns the implementation. Called as `ifunc_add(a, b)` instead — which is what an importer bound to
/// the resolver does — it ignores both arguments and returns this code pointer as the "sum".
static void* resolve_ifunc_add(void) {
  ++g_ifunc_resolver_runs;
  return (void*)ifunc_add_impl;
}

int ifunc_add(int, int) __attribute__((ifunc("resolve_ifunc_add")));
