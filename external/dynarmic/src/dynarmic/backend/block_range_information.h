/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <unordered_map>
#include <vector>

#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>
#include <tsl/robin_set.h>

#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend {

/// RetroBug change: a per-4 KiB-page index instead of an interval_map of sets (RetroBug's compat/boost/icl one, whose
/// add() is linear in the map). The interval_map merged and copied the
/// descriptor sets of every interval a new block overlapped, which grew without bound on code with many entry points
/// into the same straight-line region (an interpreter's handlers): on a 2015 game it took 97% of the CPU. A block is now
/// recorded under each page it touches, and invalidation answers every block on the pages a range touches — a superset
/// of the old answer, which only costs recompiling a few extra blocks.
template<typename ProgramCounterType>
class BlockRangeInformation {
public:
    void AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location);
    void ClearCache();
    tsl::robin_set<IR::LocationDescriptor> InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges);

private:
    static constexpr unsigned kPageBits = 12;
    std::unordered_map<ProgramCounterType, std::vector<IR::LocationDescriptor>> by_page;
};

}  // namespace Dynarmic::Backend
