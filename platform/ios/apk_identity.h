// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>
#include <variant>

namespace rb::platform::ios {

struct ApkIdentity {
  std::string package;
  std::u16string version_name;
  std::uint32_t version_code = 0;
  std::uint32_t icon = 0;
  std::uint32_t round_icon = 0;
  std::uint32_t target_sdk = 0;
  std::map<std::u16string, std::variant<bool, std::int32_t, std::u16string>> metadata;
};

ApkIdentity ReadApkIdentity(std::span<const std::uint8_t> manifest);

struct ApkDrawable {
  std::string path;
  unsigned density;
};

std::vector<ApkDrawable> ReadApkDrawables(std::span<const std::uint8_t> resources, std::uint32_t id);

}  // namespace rb::platform::ios
