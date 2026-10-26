// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// An APK's assets as a byte stream: what the iOS `AAsset*` thunks read through (a game asks its Java layer
// for the AssetManager, and the native side reads files out of the APK). Built on ZipReader, so the same
// entry-level view the boot uses is what a title's asset reads get.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rb/loader/zip.h"

namespace rb::loader {

class ApkAssets {
 public:
  /// The APK at `path`. nullopt with `why` when it is not a zip this reader reads.
  static std::unique_ptr<ApkAssets> Open(const std::string& path, std::string* why);
  /// An archive already in memory (a test fixture, or an APK the shell imported).
  static std::unique_ptr<ApkAssets> OpenMemory(std::vector<std::uint8_t> bytes, std::string* why);

  bool Has(const std::string& name) const { return zip_->Has(name); }
  const std::vector<std::string>& Names() const { return zip_->Names(); }
  /// Opens `name` (an archive path, e.g. "assets/levels/one.dat") and resets the cursor. The bytes are read
  /// now, so a later Seek/Read never goes back to the file.
  bool OpenEntry(const std::string& name, std::string* why);
  /// A read-only entry view that does not keep a cursor of its own.
  std::optional<std::vector<std::uint8_t>> Read(const std::string& name, std::string* why) {
    return zip_->Read(name, why);
  }

  [[nodiscard]] std::int64_t Length() const { return static_cast<std::int64_t>(bytes_.size()); }
  [[nodiscard]] std::int64_t Position() const { return pos_; }
  /// Up to `n` bytes from the cursor; 0 at the end. The cursor advances by what was copied.
  std::int64_t Read(void* dst, std::int64_t n);
  /// SEEK_SET (0), SEEK_CUR (1), SEEK_END (2); returns the new position, or -1 for a bad `whence` or a
  /// negative resulting offset.
  std::int64_t Seek(std::int64_t off, int whence);

 private:
  ApkAssets() = default;
  std::unique_ptr<ZipReader> zip_;
  std::vector<std::uint8_t> bytes_;
  std::int64_t pos_ = 0;
};

}  // namespace rb::loader
