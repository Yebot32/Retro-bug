// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// An APK's entries as a byte stream: what the iOS `AAsset*` thunks read through. `ApkArchive` is the
// archive (one per APK); `ApkFile` is one open entry with its own cursor (many may be open at once, as a
// game's asset layer expects).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rb/loader/zip.h"

namespace rb::loader {

class ApkFile {
 public:
  [[nodiscard]] std::int64_t Length() const { return static_cast<std::int64_t>(bytes_.size()); }
  [[nodiscard]] std::int64_t Position() const { return pos_; }
  /// Up to `n` bytes from the cursor; 0 at the end. The cursor advances by what was copied.
  std::int64_t Read(void* dst, std::int64_t n);
  /// SEEK_SET (0), SEEK_CUR (1), SEEK_END (2); returns the new position, or -1 for a bad `whence` or a
  /// negative resulting offset. Seeking past the end is legal; the next read is 0.
  std::int64_t Seek(std::int64_t off, int whence);

 private:
  friend class ApkArchive;
  explicit ApkFile(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
  std::vector<std::uint8_t> bytes_;
  std::int64_t pos_ = 0;
};

class ApkArchive {
 public:
  /// The APK at `path`. nullopt with `why` when it is not a zip this reader reads.
  static std::unique_ptr<ApkArchive> Open(const std::string& path, std::string* why);
  /// An archive already in memory (a test fixture, or an APK the shell imported).
  static std::unique_ptr<ApkArchive> OpenMemory(std::vector<std::uint8_t> bytes, std::string* why);

  bool Has(const std::string& name) const { return zip_->Has(name); }
  const std::vector<std::string>& Names() const { return zip_->Names(); }
  /// A read-only view that does not keep a cursor of its own.
  std::optional<std::vector<std::uint8_t>> Read(const std::string& name, std::string* why) {
    return zip_->Read(name, why);
  }
  /// One entry's bytes and a cursor of its own. nullptr with `why` when the entry is absent or damaged.
  std::unique_ptr<ApkFile> OpenFile(const std::string& name, std::string* why);

 private:
  ApkArchive() = default;
  std::unique_ptr<ZipReader> zip_;
};

}  // namespace rb::loader
