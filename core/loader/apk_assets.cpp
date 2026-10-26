// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See rb/loader/apk_assets.h.

#include "rb/loader/apk_assets.h"

#include <algorithm>
#include <cstring>

namespace rb::loader {

std::unique_ptr<ApkArchive> ApkArchive::Open(const std::string& path, std::string* why) {
  auto zip = ZipReader::OpenFile(path, why);
  if (!zip) return nullptr;
  std::unique_ptr<ApkArchive> archive(new ApkArchive);
  archive->zip_ = std::move(zip);
  return archive;
}

std::unique_ptr<ApkArchive> ApkArchive::OpenMemory(std::vector<std::uint8_t> bytes, std::string* why) {
  auto zip = ZipReader::OpenMemory(std::move(bytes), why);
  if (!zip) return nullptr;
  std::unique_ptr<ApkArchive> archive(new ApkArchive);
  archive->zip_ = std::move(zip);
  return archive;
}

std::unique_ptr<ApkFile> ApkArchive::OpenFile(const std::string& name, std::string* why) {
  auto bytes = zip_->Read(name, why);
  if (!bytes) return nullptr;
  return std::unique_ptr<ApkFile>(new ApkFile(std::move(*bytes)));
}

std::int64_t ApkFile::Read(void* dst, std::int64_t n) {
  if (n <= 0 || dst == nullptr || pos_ >= static_cast<std::int64_t>(bytes_.size())) return 0;
  const std::int64_t left = static_cast<std::int64_t>(bytes_.size()) - pos_;
  const std::int64_t take = std::min(n, left);
  std::memcpy(dst, bytes_.data() + pos_, static_cast<std::size_t>(take));
  pos_ += take;
  return take;
}

std::int64_t ApkFile::Seek(std::int64_t off, int whence) {
  std::int64_t base = 0;
  switch (whence) {
    case 0: base = 0; break;                                       // SEEK_SET
    case 1: base = pos_; break;                                    // SEEK_CUR
    case 2: base = static_cast<std::int64_t>(bytes_.size()); break; // SEEK_END
    default: return -1;
  }
  const std::int64_t next = base + off;
  if (next < 0) return -1;
  pos_ = next;
  return next;
}

}  // namespace rb::loader
