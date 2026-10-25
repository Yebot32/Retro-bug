// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The zip reader's implementation: see rb/loader/zip.h.

#include "rb/loader/zip.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace rb::loader {

namespace {

std::string Hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%x", v);
  return buf;
}

std::uint32_t U16(const std::vector<std::uint8_t>& b, std::size_t at) { return b[at] | (b[at + 1] << 8); }

std::uint32_t U32(const std::vector<std::uint8_t>& b, std::size_t at) {
  return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

}  // namespace

ZipReader::~ZipReader() {
  if (fd_ >= 0) ::close(fd_);
}

std::unique_ptr<ZipReader> ZipReader::OpenFile(const std::string& path, std::string* why) {
  std::unique_ptr<ZipReader> z(new ZipReader);
  z->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (z->fd_ < 0) {
    z->Fail(why, "open(" + path + "): " + std::strerror(errno));
    return nullptr;
  }
  struct stat st {};
  if (::fstat(z->fd_, &st) != 0) {
    z->Fail(why, "fstat(" + path + "): " + std::strerror(errno));
    return nullptr;
  }
  z->size_ = static_cast<std::uint64_t>(st.st_size);
  if (!z->Parse(why)) return nullptr;
  return z;
}

std::unique_ptr<ZipReader> ZipReader::OpenMemory(std::vector<std::uint8_t> bytes, std::string* why) {
  std::unique_ptr<ZipReader> z(new ZipReader);
  z->in_memory_ = true;
  z->size_ = bytes.size();
  z->bytes_ = std::move(bytes);
  if (!z->Parse(why)) return nullptr;
  return z;
}

bool ZipReader::Parse(std::string* why) {
  if (size_ < 22) {
    return Fail(why, "the archive is shorter than an end-of-central-directory record");
  }
  const std::uint64_t tail_len = std::min<std::uint64_t>(size_, 22 + 0xffff);
  std::vector<std::uint8_t> tail(tail_len);
  if (!ReadAt(size_ - tail_len, tail.data(), tail.size())) return Fail(why, "reading the zip's tail: " + err_);
  std::size_t eocd = tail.size() - 22;
  while (U32(tail, eocd) != 0x06054b50u) {
    if (eocd == 0) return Fail(why, "the archive has no end-of-central-directory record");
    --eocd;
  }
  const std::uint32_t entries = U16(tail, eocd + 10), cd_size = U32(tail, eocd + 12), cd_at = U32(tail, eocd + 16);
  if (entries == 0xffffu || cd_at == 0xffff'ffffu || cd_size == 0xffff'ffffu) {
    return Fail(why, "the archive is a ZIP64 archive, which this reader does not read");
  }
  std::vector<std::uint8_t> cd(cd_size);
  if (!ReadAt(cd_at, cd.data(), cd.size())) return Fail(why, "reading the central directory: " + err_);
  std::size_t at = 0;
  for (std::uint32_t i = 0; i < entries; ++i) {
    if (at + 46 > cd.size() || U32(cd, at) != 0x02014b50u) {
      return Fail(why, "central directory entry " + std::to_string(i) + " is damaged");
    }
    const std::uint32_t nlen = U16(cd, at + 28), xlen = U16(cd, at + 30), clen = U16(cd, at + 32);
    if (at + 46 + nlen > cd.size()) return Fail(why, "a central directory name runs off the directory");
    std::string name(reinterpret_cast<const char*>(cd.data() + at + 46), nlen);
    entries_[name] = Entry{U16(cd, at + 10), U32(cd, at + 16), U32(cd, at + 20), U32(cd, at + 24), U32(cd, at + 42)};
    order_.push_back(std::move(name));
    at += 46 + nlen + xlen + clen;
  }
  return true;
}

std::optional<std::vector<std::uint8_t>> ZipReader::Read(const std::string& name, std::string* why) {
  const auto none = [&](const std::string& w) -> std::optional<std::vector<std::uint8_t>> {
    if (why != nullptr) *why = w;
    return std::nullopt;
  };
  const auto it = entries_.find(name);
  if (it == entries_.end()) return none("no entry named " + name);
  const Entry& e = it->second;
  std::vector<std::uint8_t> lh(30);
  if (!ReadAt(e.local, lh.data(), lh.size()) || U32(lh, 0) != 0x04034b50u) {
    return none(name + ": local header is damaged");
  }
  const std::uint64_t data = std::uint64_t{e.local} + 30 + U16(lh, 26) + U16(lh, 28);
  std::vector<std::uint8_t> out(e.usize);
  if (e.method == 0) {
    if (e.csize != e.usize) return none(name + ": stored, but the sizes disagree");
    if (!ReadAt(data, out.data(), out.size())) return none(name + ": " + err_);
  } else if (e.method == 8) {
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return none(name + ": inflateInit2 failed");
    std::vector<std::uint8_t> chunk(1 << 16);
    std::uint64_t left = e.csize, pos = data;
    int rc = Z_OK;
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    while (rc == Z_OK && left > 0) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, chunk.size()));
      if (!ReadAt(pos, chunk.data(), n)) {
        inflateEnd(&z);
        return none(name + ": " + err_);
      }
      pos += n;
      left -= n;
      z.next_in = chunk.data();
      z.avail_in = static_cast<uInt>(n);
      rc = inflate(&z, left == 0 ? Z_FINISH : Z_NO_FLUSH);
      if (rc == Z_BUF_ERROR && z.avail_out == 0) break;  // more input than the directory's size says
    }
    const std::uint64_t produced = z.total_out;
    inflateEnd(&z);
    if (rc != Z_STREAM_END || produced != e.usize) {
      return none(name + ": inflate ended with " + std::to_string(rc) + " after " + std::to_string(produced) + " of " +
                  std::to_string(e.usize) + " bytes");
    }
  } else {
    return none(name + ": compression method " + std::to_string(e.method) + " is not stored or deflate");
  }
  const std::uint32_t crc = static_cast<std::uint32_t>(crc32(0L, out.data(), static_cast<uInt>(out.size())));
  if (crc != e.crc) return none(name + ": CRC " + Hex(crc) + " is not the directory's " + Hex(e.crc));
  return out;
}

bool ZipReader::Fail(std::string* why, const std::string& w) {
  if (why != nullptr) *why = w;
  return false;
}

bool ZipReader::ReadAt(std::uint64_t off, std::uint8_t* into, std::size_t n) {
  if (in_memory_) {
    if (off > size_ || n > size_ - off) {
      err_ = "unexpected end of archive at offset " + std::to_string(off);
      return false;
    }
    std::memcpy(into, bytes_.data() + off, n);
    return true;
  }
  while (n > 0) {
    const ssize_t got = ::pread(fd_, into, n, static_cast<off_t>(off));
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) {
      err_ = got == 0 ? "unexpected end of file at offset " + std::to_string(off) : std::strerror(errno);
      return false;
    }
    into += got;
    off += static_cast<std::uint64_t>(got);
    n -= static_cast<std::size_t>(got);
  }
  return true;
}

}  // namespace rb::loader
