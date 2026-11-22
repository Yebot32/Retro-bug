// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "apk_identity.h"

#include <filesystem>

namespace rb::platform::ios {
namespace {

using Bytes = std::span<const std::uint8_t>;

std::uint32_t U16(Bytes data, std::size_t pos) {
  return data[pos] | (std::uint32_t{data[pos + 1]} << 8);
}

std::uint32_t U32(Bytes data, std::size_t pos) {
  return U16(data, pos) | (U16(data, pos + 2) << 16);
}

std::u16string PoolString(Bytes pool, std::uint32_t index) {
  if (pool.size() < 28) return {};
  const std::size_t header = U16(pool, 2), start = U32(pool, 20);
  if (header < 28 || header > start || start > pool.size() ||
      index >= U32(pool, 8) || index >= (start - header) / 4) return {};
  const std::size_t offset = U32(pool, header + index * 4);
  if (offset >= pool.size() - start) return {};
  const Bytes entry = pool.subspan(start + offset);
  if ((U32(pool, 16) & 0x100u) != 0) {
    // UTF-8 entries have separate UTF-16-unit and UTF-8-byte length prefixes.
    const auto read_length = [&](std::size_t& pos) -> std::size_t {
      if (pos >= entry.size()) return entry.size();
      const auto first = entry[pos++];
      if ((first & 0x80u) == 0) return first;
      if (pos >= entry.size()) return entry.size();
      return ((first & 0x7fu) << 8) | entry[pos++];
    };
    std::size_t pos = 0;
    read_length(pos);
    const auto size = read_length(pos);
    if (size >= entry.size() - pos || entry[pos + size] != 0) return {};
    const auto* text = reinterpret_cast<const char8_t*>(entry.data() + pos);
    try {
      return std::filesystem::path(std::u8string(text, size)).u16string();
    } catch (const std::filesystem::filesystem_error&) {
      return {};
    }
  }
  if (entry.size() < 4) return {};
  std::size_t start16 = 2;
  std::size_t length = U16(entry, 0);
  if ((length & 0x8000u) != 0) {
    length = ((length & 0x7fffu) << 16) | U16(entry, 2);
    start16 = 4;
  }
  if (length >= (entry.size() - start16) / 2 || U16(entry, start16 + length * 2) != 0) return {};
  std::u16string value;
  value.reserve(length);
  for (std::size_t i = 0; i < length; ++i) value.push_back(static_cast<char16_t>(U16(entry, start16 + i * 2)));
  return value;
}

}  // namespace

ApkIdentity ReadApkIdentity(Bytes manifest) {
  if (manifest.size() < 8 || U16(manifest, 0) != 0x0003 || U16(manifest, 2) != 8) return {};
  const std::size_t end = U32(manifest, 4);
  if (end < 8 || end > manifest.size()) return {};
  Bytes strings;
  for (std::size_t pos = 8; end - pos >= 8;) {
    const auto type = U16(manifest, pos);
    const auto header = U16(manifest, pos + 2);
    const auto size = U32(manifest, pos + 4);
    if (header < 8 || size < header || size > end - pos) return {};
    const Bytes chunk = manifest.subspan(pos, size);
    pos += size;
    if (type == 0x0001) strings = chunk;
    if (type != 0x0102) continue;
    // The first element must be the root. Read its unqualified package and Android version attributes.
    if (header != 16 || chunk.size() < 36 || PoolString(strings, U32(chunk, 20)) != u"manifest") return {};
    const std::size_t stride = U16(chunk, 26), count = U16(chunk, 28), first = 16 + U16(chunk, 24);
    if (stride < 20 || first < 36 || first > chunk.size() || count > (chunk.size() - first) / stride) return {};
    ApkIdentity identity;
    for (std::size_t i = 0; i < count; ++i) {
      const auto attr = chunk.subspan(first + i * stride, 20);
      const auto ns = U32(attr, 0);
      const auto name = PoolString(strings, U32(attr, 4));
      const auto kind = attr[15];
      const auto data = U32(attr, 16);
      const auto string = kind == 3 ? data : U32(attr, 8);
      if (ns == 0xffffffffu && name == u"package") {
        const auto package = PoolString(strings, string);
        for (const char16_t c : package) {
          if (!(c == '.' || c == '_' || (c >= '0' && c <= '9') ||
                (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return {};
        }
        identity.package.assign(package.begin(), package.end());
      } else if (PoolString(strings, ns) == u"http://schemas.android.com/apk/res/android") {
        if (name == u"versionName") identity.version_name = PoolString(strings, string);
        if (name == u"versionCode" && (kind == 0x10 || kind == 0x11)) identity.version_code = data;
      }
    }
    return identity;
  }
  return {};
}

}  // namespace rb::platform::ios
