// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A minimal zip writer for the tests: stored entries, plus raw-deflate entries for the inflate path.
// `crc` is what the central directory claims, so a test can make it disagree with the data on purpose.
#pragma once

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace rb::test {

inline void Put16(std::vector<std::uint8_t>& b, std::uint32_t v) {
  b.push_back(static_cast<std::uint8_t>(v & 0xff));
  b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
}

inline void Put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}

inline std::vector<std::uint8_t> Bytes(const std::string& s) { return std::vector<std::uint8_t>(s.begin(), s.end()); }

class ZipWriter {
 public:
  void AddStored(const std::string& name, const std::vector<std::uint8_t>& bytes,
                 std::uint32_t crc_override = 0xffff'ffffu) {
    Entry e;
    e.name = name;
    e.bytes = bytes;
    e.method = 0;
    e.payload = bytes;
    e.crc = crc_override != 0xffff'ffffu
                ? crc_override
                : static_cast<std::uint32_t>(crc32(0L, bytes.data(), static_cast<uInt>(bytes.size())));
    entries_.push_back(std::move(e));
  }

  void AddDeflated(const std::string& name, const std::vector<std::uint8_t>& bytes) {
    Entry e;
    e.name = name;
    e.bytes = bytes;
    e.method = 8;
    e.crc = static_cast<std::uint32_t>(crc32(0L, bytes.data(), static_cast<uInt>(bytes.size())));
    z_stream z{};
    if (deflateInit2(&z, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
      std::fprintf(stderr, "deflateInit2 failed\n");
      return;
    }
    e.payload.resize(deflateBound(&z, static_cast<uLong>(bytes.size())));
    z.next_in = const_cast<Bytef*>(bytes.data());
    z.avail_in = static_cast<uInt>(bytes.size());
    z.next_out = e.payload.data();
    z.avail_out = static_cast<uInt>(e.payload.size());
    const int rc = deflate(&z, Z_FINISH);
    if (rc != Z_STREAM_END) {
      std::fprintf(stderr, "deflate rc=%d\n", rc);
      deflateEnd(&z);
      return;
    }
    e.payload.resize(z.total_out);
    deflateEnd(&z);
    entries_.push_back(std::move(e));
  }

  std::vector<std::uint8_t> Finish() const {
    std::vector<std::uint8_t> out;
    struct Placed {
      std::uint32_t local = 0;
    };
    std::vector<Placed> placed(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      const Entry& e = entries_[i];
      placed[i].local = static_cast<std::uint32_t>(out.size());
      Put32(out, 0x04034b50u);  // local file header
      Put16(out, 20);           // version needed
      Put16(out, 0);            // flags
      Put16(out, e.method);
      Put16(out, 0);  // time
      Put16(out, 0);  // date
      Put32(out, e.crc);
      Put32(out, static_cast<std::uint32_t>(e.payload.size()));
      Put32(out, static_cast<std::uint32_t>(e.bytes.size()));
      Put16(out, static_cast<std::uint32_t>(e.name.size()));
      Put16(out, 0);  // extra length
      out.insert(out.end(), e.name.begin(), e.name.end());
      out.insert(out.end(), e.payload.begin(), e.payload.end());
    }
    const std::uint32_t cd_at = static_cast<std::uint32_t>(out.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      const Entry& e = entries_[i];
      Put32(out, 0x02014b50u);  // central directory header
      Put16(out, 20);           // version made by
      Put16(out, 20);           // version needed
      Put16(out, 0);            // flags
      Put16(out, e.method);
      Put16(out, 0);
      Put16(out, 0);
      Put32(out, e.crc);
      Put32(out, static_cast<std::uint32_t>(e.payload.size()));
      Put32(out, static_cast<std::uint32_t>(e.bytes.size()));
      Put16(out, static_cast<std::uint32_t>(e.name.size()));
      Put16(out, 0);  // extra
      Put16(out, 0);  // comment
      Put16(out, 0);  // disk
      Put16(out, 0);  // internal attrs
      Put32(out, 0);  // external attrs
      Put32(out, placed[i].local);
      out.insert(out.end(), e.name.begin(), e.name.end());
    }
    const std::uint32_t cd_size = static_cast<std::uint32_t>(out.size()) - cd_at;
    Put32(out, 0x06054b50u);  // end of central directory
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, static_cast<std::uint32_t>(entries_.size()));
    Put16(out, static_cast<std::uint32_t>(entries_.size()));
    Put32(out, cd_size);
    Put32(out, cd_at);
    Put16(out, 0);
    return out;
  }

 private:
  struct Entry {
    std::string name;
    std::vector<std::uint8_t> bytes, payload;
    std::uint32_t method = 0, crc = 0;
  };
  std::vector<Entry> entries_;
};

}  // namespace rb::test
