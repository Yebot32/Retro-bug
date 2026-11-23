// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A read-only zip archive. The APK boot uses it to reach `assets/rb32/` without ever holding the whole
// archive in memory: the central directory is read once (names and offsets), then one entry at a time — a
// stored one by a raw read, a deflated one inflated in 64 KiB chunks.
//
// The reader is platform-free (it takes a path or a byte vector and uses only zlib), so the iOS shell reads
// an imported APK through the same code Android boots from.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rb::loader {

class ZipReader {
 public:
  /// The archive at `path`. nullopt (with `why`) when it cannot be opened or is not a zip this reader reads.
  static std::unique_ptr<ZipReader> OpenFile(const std::string& path, std::string* why);
  /// An archive already in memory (the iOS shell's imported APK, a test fixture).
  static std::unique_ptr<ZipReader> OpenMemory(std::vector<std::uint8_t> bytes, std::string* why);
  ~ZipReader();
  ZipReader(const ZipReader&) = delete;
  ZipReader& operator=(const ZipReader&) = delete;

  bool Has(const std::string& name) const { return entries_.count(name) != 0; }
  /// The entry's uncompressed bytes, with its CRC checked. nullopt (with `why`) when it is absent, damaged,
  /// not stored/deflate, or ZIP64 (refused by name).
  std::optional<std::vector<std::uint8_t>> Read(const std::string& name, std::string* why);
  /// Every entry name, in central-directory order.
  const std::vector<std::string>& Names() const { return order_; }
  /// The entry's uncompressed size from the central directory, nullopt when absent.
  std::optional<std::uint32_t> UncompressedSize(const std::string& name) const {
    const auto it = entries_.find(name);
    return it == entries_.end() ? std::nullopt : std::optional<std::uint32_t>(it->second.usize);
  }

 private:
  ZipReader() = default;
  bool Parse(std::string* why);
  bool ReadAt(std::uint64_t off, std::uint8_t* into, std::size_t n);
  bool Fail(std::string* why, const std::string& w);

  struct Entry {
    std::uint32_t method = 0, crc = 0, csize = 0, usize = 0, local = 0;
  };

  int fd_ = -1;
  bool in_memory_ = false;
  std::vector<std::uint8_t> bytes_;
  std::uint64_t size_ = 0;
  std::string err_;
  std::map<std::string, Entry> entries_;
  std::vector<std::string> order_;
};

}  // namespace rb::loader
