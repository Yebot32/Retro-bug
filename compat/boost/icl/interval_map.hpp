/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Boost.ICL-compatible segregating interval map, over rb::compat's interval primitives.
 * API belongs to Boost (BSL-1.0); this implementation is RetroBug's.
 *
 * Supports exactly what backend/block_range_information.cpp uses:
 *   .add(std::make_pair(discrete_interval<K>, V))   — overlapping keys split and union their values
 *   .clear()
 *   .equal_range(interval) -> std::pair<iterator, iterator>, elements are (interval, value)
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "detail/interval_core.hpp"
// discrete_interval lives here; including it keeps this header valid regardless of the order a
// translation unit reaches them in (dynarmic includes interval_map.hpp first).
#include "interval_set.hpp"

namespace boost::icl {

/// Value union for the associative types dynarmic uses as mapped values (std::set).
/// A distinct type without insert-range would be a compile error here rather than a silent
/// no-op, which is the failure mode worth avoiding on an invalidation path.
template <typename V>
void icl_combine(V& dst, const V& src) {
  dst.insert(src.begin(), src.end());
}

/**
 * Segregating map of closed intervals to values.
 *
 * Segments stay sorted, disjoint and non-adjacent, which is what makes equal_range a
 * contiguous iterator pair. On add(), existing segments are split at the new interval's
 * boundaries and the covered ones have the new value unioned in — so a block registered at
 * [10,20) and an invalidation of [15,25) must both survive as distinct, correct entries.
 * Losing a descriptor here would leave translated code stale, which is the one error this
 * container is not allowed to make; reporting an extra one merely costs a recompile.
 */
template <typename Type, typename V>
class interval_map {
 public:
  using key_type = Type;
  using mapped_type = V;
  using value_type = std::pair<rb::compat::Interval<Type>, V>;
  using container_type = std::vector<value_type>;
  using iterator = typename container_type::iterator;
  using const_iterator = typename container_type::const_iterator;

  /// Accepts any pair whose first converts to an Interval — dynarmic passes a
  /// `std::pair<discrete_interval<PC>, std::set<LocationDescriptor>>`, and a non-template
  /// signature here would fail deduction rather than converting.
  template <typename Pair>
  void add(const Pair& entry) {
    insert(rb::compat::Interval<Type>(entry.first), entry.second);
  }

  template <typename I>
  void add(const I& iv, const V& value) {
    insert(rb::compat::Interval<Type>(iv), value);
  }

  void insert(const rb::compat::Interval<Type>& iv, const V& value) {
    if (iv.empty()) {
      return;
    }
    split_at(iv.lower());
    if (iv.upper() != std::numeric_limits<Type>::max()) {
      split_at(static_cast<Type>(iv.upper() + 1));
    }

    auto first = std::lower_bound(segs_.begin(), segs_.end(), iv.lower(),
                                  [](const value_type& a, Type b) { return a.first.lower() < b; });
    bool covered = false;
    for (auto it = first; it != segs_.end() && it->first.lower() <= iv.upper(); ++it) {
      icl_combine(it->second, value);
      covered = true;
    }
    if (!covered) {
      segs_.insert(first, value_type{iv, value});
    }
  }

  void clear() { segs_.clear(); }
  bool empty() const { return segs_.empty(); }
  std::size_t size() const { return segs_.size(); }

  std::pair<iterator, iterator> equal_range(const rb::compat::Interval<Type>& iv) {
    auto first = std::lower_bound(segs_.begin(), segs_.end(), iv,
                                  [](const value_type& a, const rb::compat::Interval<Type>& b) {
                                    return a.first.upper() < b.lower();
                                  });
    auto last = std::lower_bound(first, segs_.end(), iv,
                                 [](const value_type& a, const rb::compat::Interval<Type>& b) {
                                   return a.first.lower() <= b.upper();
                                 });
    return {first, last};
  }

  std::pair<const_iterator, const_iterator> equal_range(
      const rb::compat::Interval<Type>& iv) const {
    auto first = std::lower_bound(segs_.begin(), segs_.end(), iv,
                                  [](const value_type& a, const rb::compat::Interval<Type>& b) {
                                    return a.first.upper() < b.lower();
                                  });
    auto last = std::lower_bound(first, segs_.end(), iv,
                                 [](const value_type& a, const rb::compat::Interval<Type>& b) {
                                   return a.first.lower() <= b.upper();
                                 });
    return {first, last};
  }

  iterator begin() { return segs_.begin(); }
  iterator end() { return segs_.end(); }
  const_iterator begin() const { return segs_.begin(); }
  const_iterator end() const { return segs_.end(); }

 private:
  /// Ensure no segment straddles `bound`: split [lo,hi] into [lo,bound-1] and [bound,hi].
  void split_at(Type bound) {
    container_type out;
    out.reserve(segs_.size() + 1);
    for (auto& seg : segs_) {
      if (seg.first.lower() < bound && bound <= seg.first.upper()) {
        out.emplace_back(rb::compat::Interval<Type>::closed(seg.first.lower(),
                                                            static_cast<Type>(bound - 1)),
                         seg.second);
        out.emplace_back(rb::compat::Interval<Type>::closed(bound, seg.first.upper()), seg.second);
      } else {
        out.push_back(seg);
      }
    }
    segs_.swap(out);
  }

  container_type segs_;
};

}  // namespace boost::icl
