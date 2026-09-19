/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Minimal stand-in for the two Boost.ICL containers dynarmic uses, so the vendored engine needs
 * neither Boost nor source edits (AD-14 keeps upstream byte-identical). This implements a
 * documented API; it is not derived from Boost source.
 *
 * Only what dynarmic calls is provided — see ../README.md for the call sites and for the
 * condition under which this layer can be deleted.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace rb::compat {

/**
 * A closed interval [lower, upper] of a discrete type.
 *
 * Empty is represented explicitly rather than as lower > upper, because callers build intervals
 * as (start, start + length - 1) and a zero-length range at address 0 must not wrap into a
 * full-domain interval.
 */
template <typename T>
class Interval {
 public:
  Interval() = default;

  static Interval closed(T low, T high) {
    Interval i;
    if (low <= high) {
      i.low_ = low;
      i.high_ = high;
      i.non_empty_ = true;
    }
    return i;
  }

  T lower() const { return low_; }
  T upper() const { return high_; }
  bool empty() const { return !non_empty_; }

  bool intersects(const Interval& o) const {
    if (!non_empty_ || !o.non_empty_) {
      return false;
    }
    return low_ <= o.high_ && o.low_ <= high_;
  }

  bool contains(T p) const { return non_empty_ && low_ <= p && p <= high_; }

 private:
  T low_{};
  T high_{};
  bool non_empty_ = false;
};

namespace detail {

/// True if segment `s` lies entirely before `bound` with at least one unit of gap, i.e. it
/// cannot be merged with an interval starting at `bound`. Overflow-guarded: an interval ending
/// at the type's maximum never has a successor.
template <typename T>
bool ends_strictly_before(const Interval<T>& s, T bound) {
  return s.upper() != std::numeric_limits<T>::max() && s.upper() + 1 < bound;
}

}  // namespace detail

/**
 * Sorted, disjoint, non-adjacent closed intervals: the subset of boost::icl::interval_set that
 * dynarmic's self-modifying-code invalidation path uses (add / clear / empty / iterate).
 *
 * Adjacent intervals coalesce, as they do for a discrete domain in ICL: adding [8,12] to [4,7]
 * gives [4,12]. Skipping that merge would leave a translated block un-invalidated at a block
 * boundary, which is a silent correctness bug rather than a visible failure.
 */
template <typename T>
class IntervalSet {
 public:
  using value_type = Interval<T>;
  using const_iterator = typename std::vector<value_type>::const_iterator;
  using iterator = const_iterator;

  void add(const value_type& iv) {
    if (iv.empty()) {
      return;
    }
    T low = iv.lower();
    T high = iv.upper();

    std::size_t i = 0;
    while (i < segs_.size() && detail::ends_strictly_before(segs_[i], low)) {
      ++i;
    }
    std::size_t j = i;
    while (j < segs_.size() && segs_[j].lower() <= high) {
      low = std::min(low, segs_[j].lower());
      high = std::max(high, segs_[j].upper());
      ++j;
    }
    segs_.erase(segs_.begin() + static_cast<std::ptrdiff_t>(i),
                segs_.begin() + static_cast<std::ptrdiff_t>(j));
    segs_.insert(segs_.begin() + static_cast<std::ptrdiff_t>(i), Interval<T>::closed(low, high));
  }

  void clear() { segs_.clear(); }
  bool empty() const { return segs_.empty(); }
  std::size_t size() const { return segs_.size(); }
  const_iterator begin() const { return segs_.begin(); }
  const_iterator end() const { return segs_.end(); }

 private:
  std::vector<value_type> segs_;
};

}  // namespace rb::compat
