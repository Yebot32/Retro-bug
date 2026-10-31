// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// SHA-256 for the profile manifest (AOT task 5). Self-contained like the rest of the tool: the APK the
// runtime verifies against is hashed here at build time, and the same digest is compiled into the app,
// so the app never has to parse a manifest to know which bytes were translated.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rb::aotgen {

using Sha256Digest = std::array<std::uint8_t, 32>;

Sha256Digest Sha256(const std::uint8_t* data, std::size_t size);
inline Sha256Digest Sha256(const std::vector<std::uint8_t>& bytes) {
  return Sha256(bytes.data(), bytes.size());
}
/// Lower-case hex, 64 characters.
std::string Sha256Hex(const Sha256Digest& digest);

}  // namespace rb::aotgen
