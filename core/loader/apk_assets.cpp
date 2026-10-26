// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See rb/loader/apk_assets.h. The cursor arithmetic is the whole file: bytes are already in memory, so a
// seek is a clamp and a read is a copy.

#include "rb/loader/apk_assets.h"

#include <algorithm>
#include <cstring>

namespace rb::loader {

std::unique_ptr<ApkAssets> ApkAssets::Open(const std::string& path, std::string* why) {
  auto zip = ZipReader::OpenFile(path, why);
  if (!zip) return nullptr;
  std::unique_ptr<ApkAssets> assets(new ApkAssets);
  assets->zip_ = std::move(zip);
  return assets;
}

std::unique_ptr<ApkAssets> ApkAssets::OpenMemory(std::vector<std::uint8_t> bytes, std::string* why) {
  auto zip = ZipReader::OpenMemory(std::move(bytes), why);
  if (!zip) return nullptr;
  std::unique_ptr<ApkAssets> assets(new ApkAssets);
  assets->zip_ = std::move(zip);
  return assets;
}

bool ApkAssets::OpenEntry(const std::string& name, std::string* why) {
  auto bytes = zip_->Read(name, why);
  if (!bytes) return false;
  bytes_ = std::move(*bytes);
  pos_ = 0;
  return true;
}

std::int64_t ApkAssets::Read(void* dst, std::int64_t n) {
  if (n <= 0 || dst == nullptr || pos_ >= static_cast<std::int64_t>(bytes_.size())) return 0;
  const std::int64_t left = static_cast<std::int64_t>(bytes_.size()) - pos_;
  const std::int64_t take = std::min(n, left);
  std::memcpy(dst, bytes_.data() + pos_, static_cast<std::size_t>(take));
  pos_ += take;
  return take;
}

std::int64_t ApkAssets::Seek(std::int64_t off, int whence) {
  std::int64_t base = 0;
  switch (whence) {
    case 0: base = 0; break;                                                // SEEK_SET
    case 1: base = pos_; break;                                             // SEEK_CUR
    case 2: base = static_cast<std::int64_t>(bytes_.size()); break;          // SEEK_END
    default: return -1;
  }
  const std::int64_t next = base + off;
  if (next < 0) return -1;
  // Seeking past the end is legal for a stream: the next read is just 0, and Position reports the request.
  pos_ = next;
  return next;
}

}  // namespace rb::loader
