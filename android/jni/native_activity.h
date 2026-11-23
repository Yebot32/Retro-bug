// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

struct ANativeActivity;
namespace rb::host { class Boot; }

namespace rb::launcher {
class HostSlots;

void RegisterNativeActivityThunks(HostSlots& slots);
void StartNativeActivity(HostSlots& slots, host::Boot& boot, std::uint32_t entry,
                         ANativeActivity* activity, const void* saved_state, std::size_t saved_size);
}  // namespace rb::launcher
