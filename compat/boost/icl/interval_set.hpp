/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 * Boost.ICL-compatible facade over rb::compat::IntervalSet. API belongs to Boost (BSL-1.0);
 * the implementation here is RetroBug's.
 */
#pragma once

// Not decoration: dynarmic's backend headers use std::map / std::set / std::tuple without
// including them, because boost/icl/* and boost/variant.hpp pull those in transitively. Omitting
// them here produces "too few template arguments for class template 'map'" at a distance, which is
// a genuinely confusing failure to debug, so the transitive surface is reproduced deliberately.
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "detail/interval_core.hpp"

namespace boost::icl {

using rb::compat::Interval;
using rb::compat::IntervalSet;

/// Name used by dynarmic: `boost::icl::discrete_interval<u32>::closed(lo, hi)`.
template <typename Type>
class discrete_interval : public rb::compat::Interval<Type> {
 public:
  using base = rb::compat::Interval<Type>;
  using base::closed;

  discrete_interval() = default;
  discrete_interval(const base& b) : base(b) {}
  operator base() const { return static_cast<const base&>(*this); }
};

/// `boost::icl::interval_set<T>` as dynarmic uses it.
template <typename Type>
class interval_set : public rb::compat::IntervalSet<Type> {
 public:
  using value_type = rb::compat::Interval<Type>;
  interval_set() = default;
  interval_set(const discrete_interval<Type>& iv) { this->add(iv); }
};

}  // namespace boost::icl
