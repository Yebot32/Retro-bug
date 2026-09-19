/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the Boost.ICL-compatible interval containers used by dynarmic's
 * self-modifying-code invalidation path.
 *
 * Why this file matters more than its size suggests: a lost descriptor here leaves stale
 * translated code executing silently in a guest, which surfaces as unexplainable misbehaviour
 * in an emulated app rather than as a failure. Over-reporting only costs a recompile, so every
 * test below that could go either way is written to catch *under*-reporting.
 *
 * The cases were derived from the actual call sites:
 *   backend/block_range_information.cpp   AddRange / InvalidateRanges
 *   backend/{arm64,x64}/a32_interface.cpp InvalidateCacheRange -> interval_set -> InvalidateCacheRanges
 */
#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void fail(const char* what, int line) {
  ++failures;
  std::printf("FAIL  %s   (line %d)\n", what, line);
}

// Variadic so braced initialisers on the same line as a comma (Desc{1, 2}) survive
// preprocessor argument splitting.
#define RB_CHECK(...)           \
  do {                          \
    ++checks;                   \
    if (!(__VA_ARGS__)) {       \
      fail(#__VA_ARGS__, __LINE__); \
    }                           \
  } while (0)

using u32 = std::uint32_t;
using u64 = std::uint64_t;
template <typename T>
using Iv = boost::icl::discrete_interval<T>;
template <typename T>
using Set = boost::icl::interval_set<T>;
template <typename T, typename V>
using Map = boost::icl::interval_map<T, V>;

/// Snapshot of a set as (lower, upper) pairs, for readable assertions.
template <typename T>
std::vector<std::pair<T, T>> dump(const Set<T>& s) {
  std::vector<std::pair<T, T>> out;
  for (const auto& iv : s) {
    out.emplace_back(iv.lower(), iv.upper());
  }
  return out;
}

// ---------------------------------------------------------------- interval_set

void set_empty_interval_is_ignored() {
  Set<u32> s;
  s.add(Iv<u32>::closed(10, 5));  // inverted bounds => empty
  s.add(Iv<u32>::closed(4, 7));
  RB_CHECK(s.size() == 1);
  RB_CHECK((dump(s) == std::vector<std::pair<u32, u32>>{{4, 7}}));
}

void set_coalesces_adjacent_not_just_overlapping() {
  // [4,7] and [8,12] do not overlap but are adjacent. Not merging them would leave a gap at a
  // block boundary, i.e. an invalidation that misses the first word of the next block.
  Set<u32> s;
  s.add(Iv<u32>::closed(4, 7));
  s.add(Iv<u32>::closed(8, 12));
  RB_CHECK((dump(s) == std::vector<std::pair<u32, u32>>{{4, 12}}));
}

void set_keeps_genuine_gaps() {
  Set<u32> s;
  s.add(Iv<u32>::closed(0, 3));
  s.add(Iv<u32>::closed(10, 12));
  RB_CHECK((dump(s) == std::vector<std::pair<u32, u32>>{{0, 3}, {10, 12}}));
}

void set_absorbs_contained_and_grows_left() {
  Set<u32> s;
  s.add(Iv<u32>::closed(0, 100));
  s.add(Iv<u32>::closed(10, 20));
  RB_CHECK(s.size() == 1);
  s.add(Iv<u32>::closed(200, 300));
  s.add(Iv<u32>::closed(50, 250));  // bridges both => one span
  RB_CHECK((dump(s) == std::vector<std::pair<u32, u32>>{{0, 300}}));
}

void set_handles_domain_max_without_overflow() {
  // upper() + 1 is the obvious adjacency test and it wraps at the maximum value; the
  // implementation guards it, which is exactly the kind of thing that silently mis-merges.
  constexpr u32 kMax = ~u32{0};
  Set<u32> s;
  s.add(Iv<u32>::closed(0, kMax));
  s.add(Iv<u32>::closed(5, 10));
  RB_CHECK((dump(s) == std::vector<std::pair<u32, u32>>{{0, kMax}}));

  Set<u32> t;
  t.add(Iv<u32>::closed(kMax, kMax));
  t.add(Iv<u32>::closed(0, 3));
  RB_CHECK((dump(t) == std::vector<std::pair<u32, u32>>{{0, 3}, {kMax, kMax}}));
}

void set_iterates_in_address_order() {
  Set<u64> s;
  s.add(Iv<u64>::closed(500, 599));
  s.add(Iv<u64>::closed(100, 199));
  s.add(Iv<u64>::closed(300, 399));
  const auto v = dump(s);
  RB_CHECK(v.size() == 3 && v[0].first == 100 && v[1].first == 300 && v[2].first == 500);
}

void set_clear_works() {
  Set<u32> s;
  s.add(Iv<u32>::closed(1, 2));
  s.clear();
  RB_CHECK(s.empty());
}

// ------------------------------------------------------------------ interval_map

using Desc = std::set<int>;

/// Mirrors BlockRangeInformation::InvalidateRanges: every descriptor whose registered range
/// intersects an invalidated interval must come back.
std::set<int> invalidate(const Map<u32, Desc>& ranges, const Set<u32>& invalid) {
  std::set<int> out;
  for (const auto& iv : invalid) {
    auto pair = const_cast<Map<u32, Desc>&>(ranges).equal_range(iv);
    for (auto it = pair.first; it != pair.second; ++it) {
      out.insert(it->second.begin(), it->second.end());
    }
  }
  return out;
}

void map_splits_on_partial_overlap_and_unions_values() {
  Map<u32, Desc> m;
  m.add(std::make_pair(Iv<u32>::closed(10, 20), Desc{1}));
  m.add(std::make_pair(Iv<u32>::closed(15, 25), Desc{2}));

  RB_CHECK(m.size() == 3);  // [10,14]{1} [15,20]{1,2} [21,25]{2}
  Set<u32> probe;
  probe.add(Iv<u32>::closed(12, 12));
  RB_CHECK(invalidate(m, probe) == Desc{1});
  probe.clear();
  probe.add(Iv<u32>::closed(17, 17));
  RB_CHECK(invalidate(m, probe) == Desc{1, 2});
  probe.clear();
  probe.add(Iv<u32>::closed(23, 23));
  RB_CHECK(invalidate(m, probe) == Desc{2});
}

void map_does_not_merge_adjacent_keys_with_different_values() {
  // Unlike the set, the map must NOT coalesce [10,20] and [21,30]: merging them would attach
  // descriptor 2 to an invalidation of [10,20] and, worse, hide which block is which.
  Map<u32, Desc> m;
  m.add(std::make_pair(Iv<u32>::closed(10, 20), Desc{1}));
  m.add(std::make_pair(Iv<u32>::closed(21, 30), Desc{2}));
  RB_CHECK(m.size() == 2);
  Set<u32> probe;
  probe.add(Iv<u32>::closed(10, 20));
  RB_CHECK(invalidate(m, probe) == Desc{1});  // must not leak descriptor 2
}

void map_intersection_boundaries_are_exact() {
  Map<u32, Desc> m;
  m.add(std::make_pair(Iv<u32>::closed(100, 199), Desc{7}));

  Set<u32> just_before;
  just_before.add(Iv<u32>::closed(50, 99));
  RB_CHECK(invalidate(m, just_before).empty());  // adjacent, not intersecting

  Set<u32> first_word;
  first_word.add(Iv<u32>::closed(99, 100));
  RB_CHECK(invalidate(m, first_word) == Desc{7});

  Set<u32> last_word;
  last_word.add(Iv<u32>::closed(199, 200));
  RB_CHECK(invalidate(m, last_word) == Desc{7});

  Set<u32> just_after;
  just_after.add(Iv<u32>::closed(200, 250));
  RB_CHECK(invalidate(m, just_after).empty());

  Set<u32> surround;
  surround.add(Iv<u32>::closed(0, 4000));
  RB_CHECK(invalidate(m, surround) == Desc{7});  // invalidation strictly larger than the key
}

void map_handles_max_address_and_wide_invalidation() {
  constexpr u32 kMax = ~u32{0};
  Map<u32, Desc> m;
  m.add(std::make_pair(Iv<u32>::closed(kMax - 3, kMax), Desc{9}));
  Set<u32> probe;
  probe.add(Iv<u32>::closed(kMax, kMax));
  RB_CHECK(invalidate(m, probe) == Desc{9});
  probe.clear();
  probe.add(Iv<u32>::closed(0, kMax));  // whole address space
  RB_CHECK(invalidate(m, probe) == Desc{9});
}

void map_registered_then_overlapping_invalidate_keeps_every_descriptor() {
  // The shape the SMC path actually produces: several blocks registered, one code patch whose
  // range spans all of them.
  Map<u32, Desc> m;
  m.add(std::make_pair(Iv<u32>::closed(0x1000, 0x10FF), Desc{11}));
  m.add(std::make_pair(Iv<u32>::closed(0x1100, 0x11FF), Desc{12}));
  m.add(std::make_pair(Iv<u32>::closed(0x1300, 0x13FF), Desc{13}));

  Set<u32> patch;
  patch.add(Iv<u32>::closed(0x1080, 0x1180));
  RB_CHECK(invalidate(m, patch) == Desc{11, 12});

  patch.clear();
  patch.add(Iv<u32>::closed(0x1000, 0x2000));
  RB_CHECK(invalidate(m, patch) == Desc{11, 12, 13});
}

void map_stays_queryable_when_blocks_are_registered_out_of_order() {
  // The realistic shape: blocks are registered in whatever order they get translated, not in
  // address order. equal_range is a binary search over a sorted segment list, so an insert that
  // appends out-of-order segments silently returns too few descriptors — stale translated code,
  // not a crash.
  Map<u32, Desc> m;
  m.add(std::make_pair(Iv<u32>::closed(3000, 3099), Desc{30}));
  m.add(std::make_pair(Iv<u32>::closed(1000, 1099), Desc{10}));
  m.add(std::make_pair(Iv<u32>::closed(5000, 5099), Desc{50}));
  m.add(std::make_pair(Iv<u32>::closed(2000, 2099), Desc{20}));
  m.add(std::make_pair(Iv<u32>::closed(4000, 4099), Desc{40}));

  for (u32 base = 1000; base <= 5000; base += 1000) {
    Set<u32> probe;
    probe.add(Iv<u32>::closed(base, static_cast<u32>(base + 99)));
    const int expected = static_cast<int>(base / 100);
    RB_CHECK(invalidate(m, probe) == Desc{expected});
  }

  Set<u32> all;
  all.add(Iv<u32>::closed(0, 6000));
  RB_CHECK(invalidate(m, all) == Desc{10, 20, 30, 40, 50});

  // A single invalidation spanning two registered blocks must report both.
  Set<u32> straddle;
  straddle.add(Iv<u32>::closed(3050, 4050));
  RB_CHECK(invalidate(m, straddle) == Desc{30, 40});
}

void map_iteration_yields_non_overlapping_sorted_segments() {
  Map<u32, Desc> m;
  m.add(std::make_pair(Iv<u32>::closed(300, 400), Desc{3}));
  m.add(std::make_pair(Iv<u32>::closed(100, 200), Desc{1}));
  m.add(std::make_pair(Iv<u32>::closed(350, 450), Desc{4}));

  u32 prev_upper = 0;
  bool first = true;
  bool ordered = true;
  for (const auto& seg : m) {
    if (!first && seg.first.lower() <= prev_upper) {
      ordered = false;  // overlapping or adjacent-merged segments would break equal_range's
    }                 // contiguous iterator pair
    prev_upper = seg.first.upper();
    first = false;
  }
  RB_CHECK(ordered);
}

}  // namespace

int main() {
  set_empty_interval_is_ignored();
  set_coalesces_adjacent_not_just_overlapping();
  set_keeps_genuine_gaps();
  set_absorbs_contained_and_grows_left();
  set_handles_domain_max_without_overflow();
  set_iterates_in_address_order();
  set_clear_works();

  map_splits_on_partial_overlap_and_unions_values();
  map_does_not_merge_adjacent_keys_with_different_values();
  map_intersection_boundaries_are_exact();
  map_handles_max_address_and_wide_invalidation();
  map_registered_then_overlapping_invalidate_keeps_every_descriptor();
  map_stays_queryable_when_blocks_are_registered_out_of_order();
  map_iteration_yields_non_overlapping_sorted_segments();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
