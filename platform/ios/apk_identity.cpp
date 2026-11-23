// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "apk_identity.h"

#include <filesystem>
#include <optional>

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
  ApkIdentity identity;
  bool root = true;
  for (std::size_t pos = 8; end - pos >= 8;) {
    const auto type = U16(manifest, pos);
    const auto header = U16(manifest, pos + 2);
    const auto size = U32(manifest, pos + 4);
    if (header < 8 || size < header || size > end - pos) return {};
    const Bytes chunk = manifest.subspan(pos, size);
    pos += size;
    if (type == 0x0001) strings = chunk;
    if (type != 0x0102) continue;
    if (header != 16 || chunk.size() < 36) return {};
    const auto element = PoolString(strings, U32(chunk, 20));
    if (root && element != u"manifest") return {};
    if (!root && element != u"application") continue;
    const std::size_t stride = U16(chunk, 26), count = U16(chunk, 28), first = 16 + U16(chunk, 24);
    if (stride < 20 || first < 36 || first > chunk.size() || count > (chunk.size() - first) / stride) return {};
    for (std::size_t i = 0; i < count; ++i) {
      const auto attr = chunk.subspan(first + i * stride, 20);
      const auto ns = U32(attr, 0);
      const auto name = PoolString(strings, U32(attr, 4));
      const auto kind = attr[15];
      const auto data = U32(attr, 16);
      const auto string = kind == 3 ? data : U32(attr, 8);
      if (root && ns == 0xffffffffu && name == u"package") {
        const auto package = PoolString(strings, string);
        for (const char16_t c : package) {
          if (!(c == '.' || c == '_' || (c >= '0' && c <= '9') ||
                (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return {};
        }
        identity.package.assign(package.begin(), package.end());
      } else if (PoolString(strings, ns) == u"http://schemas.android.com/apk/res/android") {
        if (root && name == u"versionName") identity.version_name = PoolString(strings, string);
        if (root && name == u"versionCode" && (kind == 0x10 || kind == 0x11)) identity.version_code = data;
        if (!root && kind == 1 && name == u"icon") identity.icon = data;
        if (!root && kind == 1 && name == u"roundIcon") identity.round_icon = data;
      }
    }
    if (!root) break;
    root = false;
  }
  return identity;
}

namespace {

// Android resources.arsc groups values by package, resource type, and device configuration.
Bytes TakeChunk(Bytes& remaining) {
  if (remaining.size() < 8) return {};
  const auto header = U16(remaining, 2);
  const auto size = U32(remaining, 4);
  if (header < 8 || size < header || size > remaining.size()) return {};
  const auto chunk = remaining.first(size);
  remaining = remaining.subspan(size);
  return chunk;
}

Bytes ResourceEntry(Bytes type, unsigned index) {
  const std::size_t header = U16(type, 2), start = U32(type, 16), count = U32(type, 12);
  if (start < header || start > type.size()) return {};
  const auto flags = type[9];
  const std::size_t stride = (flags & 2u) && !(flags & 1u) ? 2 : 4;
  if (count > (start - header) / stride) return {};
  std::uint32_t offset = 0xffffffffu;
  if (flags & 1u) {
    std::size_t lo = 0, hi = count;
    while (lo < hi) {
      const auto mid = lo + (hi - lo) / 2;
      if (U16(type, header + mid * 4) < index) lo = mid + 1;
      else hi = mid;
    }
    if (lo < count && U16(type, header + lo * 4) == index) offset = U16(type, header + lo * 4 + 2) * 4;
  } else if (index < count) {
    if (stride == 4) offset = U32(type, header + index * 4);
    else if (const auto packed = U16(type, header + index * 2); packed != 0xffffu) offset = packed * 4;
  }
  if (offset > type.size() - start || type.size() - start - offset < 8) return {};
  return type.subspan(start + offset);
}

void FindDrawables(Bytes packages, Bytes strings, std::uint32_t id, unsigned depth,
                   std::vector<ApkDrawable>& result) {
  if (depth == 0) return;
  for (auto package = TakeChunk(packages); !package.empty(); package = TakeChunk(packages)) {
    const auto header = U16(package, 2);
    if (U16(package, 0) != 0x0200 || header < 284 || U32(package, 8) != id >> 24) continue;
    const auto type_offset = header >= 288 ? U32(package, 284) : 0;
    auto types = package.subspan(header);
    for (auto type = TakeChunk(types); !type.empty(); type = TakeChunk(types)) {
      if (U16(type, 0) != 0x0201 || U16(type, 2) < 36 ||
          std::uint64_t{type[8]} + type_offset != ((id >> 16) & 0xff)) continue;
      const auto entry = ResourceEntry(type, id & 0xffff);
      if (entry.empty()) continue;
      const auto flags = U16(entry, 2);
      if (flags & 1u) continue;
      const auto value_offset = U16(entry, 0);
      const bool compact = (flags & 8u) != 0;
      if (!compact && (value_offset < 8 || value_offset > entry.size() - 8 || U16(entry, value_offset) < 8)) continue;
      const auto kind = compact ? flags >> 8 : entry[value_offset + 3];
      const auto value = U32(entry, compact ? 4 : value_offset + 4);
      if (kind == 1 && value != id) FindDrawables(package, strings, value, depth - 1, result);
      if (kind != 3) continue;
      try {
        const auto path = std::filesystem::path(PoolString(strings, value)).generic_u8string();
        if (!path.empty()) result.push_back({std::string(path.begin(), path.end()), U16(type, 34)});
      } catch (const std::filesystem::filesystem_error&) {
        // Malformed path in the resource table.
      }
    }
  }
}

}  // namespace

std::vector<ApkDrawable> ReadApkDrawables(Bytes resources, std::uint32_t id) {
  std::vector<ApkDrawable> result;
  const auto table = TakeChunk(resources);
  if (id == 0 || table.empty() || U16(table, 0) != 2 || U16(table, 2) < 12) return result;
  auto children = table.subspan(U16(table, 2));
  const auto strings = TakeChunk(children);
  if (!strings.empty() && U16(strings, 0) == 1) FindDrawables(children, strings, id, 4, result);
  return result;
}

}  // namespace rb::platform::ios
