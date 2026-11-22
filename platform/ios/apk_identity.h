// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace rb::platform::ios {

struct ApkIdentity {
  std::string package;
  std::u16string version_name;
  std::uint32_t version_code = 0;
};

ApkIdentity ReadApkIdentity(std::span<const std::uint8_t> manifest);

}  // namespace rb::platform::ios
