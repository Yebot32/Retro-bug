# compat/ — Boost-API shims

**Purpose.** `external/dynarmic` requires Boost (`CMakeLists.txt:151`, `find_package(Boost 1.57 REQUIRED)`),
but uses only eight names from it. Rather than carrying a Boost tree as a build dependency, this
directory provides those eight names as header-only shims, and nothing else.

**Mechanism.** This directory is put on the include path ahead of anything else, so dynarmic's
`#include <boost/variant.hpp>` and `#include <boost/icl/interval_set.hpp>` resolve here. **No file under
`external/` is edited** — which is the point: AD-14 requires the vendored tree to stay byte-identical to
its pinned upstream commit, and a patch against a dependency this size would be a permanent rebase tax.
The shims are purely additive and can be removed by deleting this directory and supplying real Boost.

**What is covered** (the complete set of `boost::` tokens appearing anywhere in dynarmic's `src/`,
enumerated rather than guessed):

| Name | Provided by | Notes |
|---|---|---|
| `boost::variant`, `apply_visitor`, `static_visitor`, `get`, `recursive_wrapper` | `boost/variant.hpp` | `std::variant` equivalents. `recursive_wrapper<T>` collapses to `T`, since `std::variant` supports self-referential alternatives directly and the alias is only instantiated where the alternatives are complete. |
| `boost::icl::interval_set`, `discrete_interval` | `boost/icl/interval_set.hpp` over `detail/interval_core.hpp` | |
| `boost::icl::interval_map` | `boost/icl/interval_map.hpp` | |

**Why these two containers are not incidental.** They are on the self-modifying-code path:
`backend/block_range_information.cpp` records every translated block's PC range in an `interval_map` and
resolves invalidations with `equal_range`, and the AArch32 backends accumulate invalidated ranges in an
`interval_set` at `InvalidateCacheRange`. That write-tracking-plus-invalid pair is exactly the SMC mechanism
chosen in `docs/10` AD-7 over `userfaultfd`, because it is the one that also works on iOS. So the failure
mode here is not a crash: a dropped descriptor leaves stale translated code executing silently. The
asymmetry that makes this container safe to implement is that **over**-reporting an invalidation costs a
recompile, while **under**-reporting is a correctness bug — hence segments are kept sorted, disjoint and
non-adjacent with splitting on partial overlap, and adjacency coalesces so a block boundary cannot fall in a
gap.

**Tested by.** `core/tests/interval_compat_test.cpp` — covering adjacency merge, partial-overlap split with
value union, containment at both bounds, the `upper() == max` overflow case, and the specific
register-then-invalidate-intersection sequence `InvalidateRanges` performs. Compiling the shims is not the
bar; this is the part of the tree where a subtle bug is invisible until it produces wrong guest behaviour.

**Deletion condition.** Remove this directory and link real Boost when either upstream drops the
dependency or the `std::variant` migration in dynarmic's own history makes these names unnecessary.
Nothing in RetroBug's own code includes `<boost/...>`; only `external/dynarmic` does, which is what keeps
this layer finite.

**Licensing.** These files are RetroBug code under Apache-2.0 (SPDX headers in each file). They implement
interfaces defined by Boost, which is BSL-1.0; they are not derived from Boost source. `NOTICE.md` records
that no Boost is distributed with RetroBug.
