/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/block_range_information.h"

#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>
#include <mcl/stdint.hpp>
#include <tsl/robin_set.h>

namespace Dynarmic::Backend {

template<typename ProgramCounterType>
void BlockRangeInformation<ProgramCounterType>::AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location) {
    if (range.empty()) {
        return;
    }
    const ProgramCounterType first = range.lower() >> kPageBits;
    const ProgramCounterType last = range.upper() >> kPageBits;
    for (ProgramCounterType page = first;; ++page) {
        by_page[page].push_back(location);
        if (page == last) {
            break;
        }
    }
}

template<typename ProgramCounterType>
void BlockRangeInformation<ProgramCounterType>::ClearCache() {
    by_page.clear();
}

template<typename ProgramCounterType>
tsl::robin_set<IR::LocationDescriptor> BlockRangeInformation<ProgramCounterType>::InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges) {
    tsl::robin_set<IR::LocationDescriptor> erase_locations;
    for (auto invalidate_interval : ranges) {
        if (invalidate_interval.empty()) {
            continue;
        }
        const ProgramCounterType first = invalidate_interval.lower() >> kPageBits;
        const ProgramCounterType last = invalidate_interval.upper() >> kPageBits;
        for (ProgramCounterType page = first;; ++page) {
            const auto it = by_page.find(page);
            if (it != by_page.end()) {
                for (const auto& descriptor : it->second) {
                    erase_locations.insert(descriptor);
                }
                by_page.erase(it);  // the blocks recorded here are being erased; a recompile records them again
            }
            if (page == last) {
                break;
            }
        }
    }
    return erase_locations;
}

template class BlockRangeInformation<u32>;
template class BlockRangeInformation<u64>;

}  // namespace Dynarmic::Backend
