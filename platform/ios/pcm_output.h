// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "rb/audio/silent_sles.h"

namespace rb::platform::ios {

std::unique_ptr<audio::PcmOutput> OpenPcmOutput(audio::PcmFormat format,
                                                const std::function<void(const std::string&)>& log);

}  // namespace rb::platform::ios
