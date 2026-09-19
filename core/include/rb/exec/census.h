/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Block-graph walking and the IR-op census (part of C02).
 *
 * Why this exists before the interpreter: Dynarmic's IR has ~600 opcodes, and "implement Tier I" is
 * unbounded without knowing which of them real guest code reaches. The census is that knowledge, from
 * the same front end the executor will use — so the work list is measured rather than guessed, and the
 * gap between "implemented" and "demanded" is a number.
 *
 * The walk follows the terminal tree, not control-flow *values*: a `LinkBlock` names its successor in
 * the location descriptor, `If`/`CheckBit` carry both legs inline. Blocks are keyed by that full
 * descriptor — pc, mode, IT state, flags — which is the same granularity a compiling tier has to use,
 * so a block reached with two different IT states is counted twice because it *is* two blocks. Keying
 * on pc alone would have quietly merged them and under-reported exactly the predication paths
 * `docs/32-isa-coverage.md` measured as load-bearing.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "rb/exec/machine_state.h"
#include "rb/isa/front_end.h"
#include "rb/mem/guest_memory.h"

namespace rb::exec {

/// What the walk produced, so a caller can tell "the guest function is small" apart from "the walk
/// stopped early and never looked at the rest of it".
struct Census {
  /// Op name -> times it appeared, sorted by count descending then name, so the report is stable.
  std::vector<std::pair<std::string, unsigned>> ops;
  /// Terminal kinds seen, same ordering. Which terminals exist decides what the executor must
  /// implement to leave a block, so this is not decoration.
  std::vector<std::pair<std::string, unsigned>> terminals;
  unsigned blocks = 0;
  /// Non-zero means the walk stopped before exhausting the graph, so the numbers below are a
  /// lower bound. Reported as part of the result rather than through an error out-parameter: a
  /// caller that forgot to check still cannot mistake this for a complete census.
  unsigned blocks_left_unvisited = 0;
  unsigned translate_failures = 0;
  unsigned truncated_blocks = 0;  ///< blocks that ended on an unfetchable word, not a real terminal

  unsigned OpCount(const std::string& name) const;
  unsigned TotalOps() const;
};

struct WalkLimits {
  unsigned max_blocks = 20000;  ///< a guest loop must not turn a census into a hang
};

/**
 * Walk every block reachable from `start`.
 *
 * A block that cannot be translated is counted in `translate_failures` and does not abort the walk:
 * reaching an import stub or a data address through a tail call is ordinary, and a census that died
 * there would report a smaller graph rather than a broken one.
 */
Census CensusFrom(const mem::GuestMemory& memory, GuestAddr start, isa::Mode mode,
                  const WalkLimits& limits = {});

}  // namespace rb::exec
