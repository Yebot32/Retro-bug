/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 */
#include "rb/exec/census.h"

#include "dynarmic/common/variant_util.h"
#include "dynarmic/ir/terminal.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace rb::exec {
namespace {

namespace Term = Dynarmic::IR::Term;
using Dynarmic::A32::FPSCR;
using Dynarmic::A32::LocationDescriptor;
using Dynarmic::A32::PSR;

/// Every successor a terminal tree names.
///
/// `If`/`CheckBit`/`CheckHalt` hold their legs as nested terminals rather than as descriptors, so the
/// tree has to be descended: a conditional block's fallthrough is only reachable through the `else_`
/// leg, and stopping at the top level would census the taken path alone.
void CollectSuccessors(const Term::Terminal& terminal, std::vector<LocationDescriptor>& out) {
  Dynarmic::Common::VisitVariant<void>(
      terminal, [&](const auto& t) {
        using T = std::remove_cvref_t<decltype(t)>;
        if constexpr (std::is_same_v<T, Term::LinkBlock> || std::is_same_v<T, Term::LinkBlockFast> ||
                      std::is_same_v<T, Term::Interpret>) {
          out.emplace_back(t.next);
        } else if constexpr (std::is_same_v<T, Term::If> || std::is_same_v<T, Term::CheckBit>) {
          CollectSuccessors(t.then_, out);
          CollectSuccessors(t.else_, out);
        } else if constexpr (std::is_same_v<T, Term::CheckHalt>) {
          CollectSuccessors(t.else_, out);
        }
        // Invalid, ReturnToDispatch, PopRSBHint, FastDispatchHint and LinkRegister name no
        // successor reachable from here: they leave to the dispatcher, to a return address the
        // static graph cannot know, or to a hint.
      });
}

template <typename Map>
std::vector<std::pair<std::string, unsigned>> Sorted(Map&& counts) {
  std::vector<std::pair<std::string, unsigned>> out{counts.begin(), counts.end()};
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  return out;
}

}  // namespace

unsigned Census::OpCount(const std::string& name) const {
  const auto it = std::find_if(ops.begin(), ops.end(),
                               [&](const auto& e) { return e.first == name; });
  return it == ops.end() ? 0u : it->second;
}

unsigned Census::TotalOps() const {
  unsigned total = 0;
  for (const auto& [name, count] : ops) total += count;
  return total;
}

Census CensusFrom(const mem::GuestMemory& memory, GuestAddr start, isa::Mode mode,
                  const WalkLimits& limits) {
  Census out;
  std::map<std::string, unsigned> op_counts;
  std::map<std::string, unsigned> terminal_counts;
  std::set<std::uint64_t> seen;

  std::vector<LocationDescriptor> work{
      LocationDescriptor{start, PSR{}, FPSCR{}, false}.SetTFlag(mode == isa::Mode::Thumb)};

  while (!work.empty()) {
    const LocationDescriptor location = work.back();
    work.pop_back();
    // Keyed by the whole descriptor, so a loop is a repeated key rather than a repeated walk: this is
    // what makes the traversal terminate on a guest function that jumps back into itself.
    if (!seen.insert(location.UniqueHash()).second) continue;
    if (out.blocks >= limits.max_blocks) {
      // Inclusive of this block, which is therefore also not visited. What matters is that it is
      // non-zero: a lower bound reported as a total is how a census becomes a lie.
      out.blocks_left_unvisited = static_cast<unsigned>(work.size()) + 1u;
      break;
    }

    std::string err;
    auto block = isa::TranslateOne(memory, location, &err);
    if (!block) {
      ++out.translate_failures;
      continue;
    }
    ++out.blocks;
    if (block->truncated_by_unmapped_read) ++out.truncated_blocks;
    for (const std::string& op : block->ops) ++op_counts[op];
    ++terminal_counts[block->terminal];

    std::vector<LocationDescriptor> successors;
    CollectSuccessors(block->ir.GetTerminal(), successors);
    for (const auto& s : successors) work.push_back(s);
  }

  out.ops = Sorted(op_counts);
  out.terminals = Sorted(terminal_counts);
  return out;
}

}  // namespace rb::exec
