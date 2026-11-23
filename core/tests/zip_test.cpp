// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// `rb::loader::ZipReader` (S1): the APK reader the boot uses, tested without a device, a guest or an APK.
// A zip is written by hand here (stored and deflated entries, then damaged in each defined way), so the
// reader's refusals are exercised rather than assumed from a real archive that happens to be valid.

#include <unistd.h>
#include <zlib.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rb/loader/zip.h"
#include "zip_builder.h"

namespace {

using rb::test::Bytes;
using rb::test::Put16;
using rb::test::Put32;
using rb::test::ZipWriter;

int checks = 0;
int failures = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::fprintf(stderr, "FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK(...)                                   \
  do {                                               \
    ++checks;                                        \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)

#define CHECK_CTX(cond, detail)                   \
  do {                                            \
    ++checks;                                     \
    if (!(cond)) fail(#cond, __LINE__, (detail)); \
  } while (0)

void round_trips_stored_and_deflated() {
  ZipWriter w;
  const std::vector<std::uint8_t> lib = Bytes("the 32-bit library's bytes");
  w.AddStored("assets/rb32/libx.so", lib);
  // Bigger than the reader's 64 KiB inflate chunk, so the chunked path is what runs.
  std::vector<std::uint8_t> big;
  for (int i = 0; i < 4000; ++i) {
    const char* line = "assets/rb32sys/libc.so: a repeated line, so deflate has something to do\n";
    big.insert(big.end(), line, line + std::strlen(line));
  }
  w.AddDeflated("assets/rb32sys/libc.so", big);
  w.AddStored("AndroidManifest.xml", Bytes("not really XML"));

  std::string why;
  auto z = rb::loader::ZipReader::OpenMemory(w.Finish(), &why);
  CHECK_CTX(z != nullptr, why);
  if (!z) return;

  CHECK(z->Has("assets/rb32/libx.so"));
  CHECK(z->Has("assets/rb32sys/libc.so"));
  CHECK(!z->Has("assets/rb32/liby.so"));
  CHECK(z->Names().size() == 3);

  auto a = z->Read("assets/rb32/libx.so", &why);
  CHECK_CTX(a.has_value(), why);
  if (a) CHECK(*a == lib);

  auto b = z->Read("assets/rb32sys/libc.so", &why);
  CHECK_CTX(b.has_value(), why);
  if (b) CHECK(*b == big);

  auto c = z->Read("AndroidManifest.xml", &why);
  CHECK_CTX(c.has_value(), why);
  if (c) CHECK_CTX(c->size() == 14 && std::memcmp(c->data(), "not really XML", 14) == 0, "manifest bytes");
}

void refuses_by_name() {
  ZipWriter w;
  w.AddStored("assets/rb32/libx.so", Bytes("bytes"));
  std::string why;

  {
    auto z = rb::loader::ZipReader::OpenMemory(w.Finish(), &why);
    CHECK(z != nullptr);
    if (z) {
      auto missing = z->Read("assets/rb32/libnope.so", &why);
      CHECK(!missing.has_value());
      CHECK_CTX(why.find("no entry named assets/rb32/libnope.so") != std::string::npos, why);
    }
  }
  {
    // A stored entry whose central directory CRC is wrong: the reader must not hand the bytes out.
    ZipWriter bad;
    bad.AddStored("assets/rb32/libx.so", Bytes("bytes"), 0x1234'5678u);
    auto z = rb::loader::ZipReader::OpenMemory(bad.Finish(), &why);
    CHECK(z != nullptr);
    if (z) {
      auto out = z->Read("assets/rb32/libx.so", &why);
      CHECK(!out.has_value());
      CHECK_CTX(why.find("CRC") != std::string::npos, why);
    }
  }
  {
    // Not a zip at all: no end-of-central-directory record.
    auto z = rb::loader::ZipReader::OpenMemory(Bytes(std::string(64, 'x')), &why);
    CHECK(z == nullptr);
    CHECK_CTX(why.find("end-of-central-directory") != std::string::npos, why);
  }
  {
    // A ZIP64 sentinel where the central directory offset belongs: refused by name, not misread as 0.
    std::vector<std::uint8_t> z64;
    Put32(z64, 0x06054b50u);
    Put16(z64, 0);
    Put16(z64, 0);
    Put16(z64, 0xffffu);
    Put16(z64, 0xffffu);
    Put32(z64, 0xffff'ffffu);
    Put32(z64, 0xffff'ffffu);
    Put16(z64, 0);
    auto z = rb::loader::ZipReader::OpenMemory(z64, &why);
    CHECK(z == nullptr);
    CHECK_CTX(why.find("ZIP64") != std::string::npos, why);
  }
}

void reads_from_a_file() {
  ZipWriter w;
  const std::vector<std::uint8_t> lib = Bytes("file-backed bytes");
  w.AddStored("assets/rb32/libx.so", lib);
  const std::vector<std::uint8_t> image = w.Finish();

  const char* dir = std::getenv("TMPDIR");
  std::string tmpl = std::string(dir != nullptr && *dir != '\0' ? dir : "/data/local/tmp") + "/rb_zip_XXXXXX";
  std::vector<char> path(tmpl.begin(), tmpl.end());
  path.push_back('\0');
  const int fd = mkstemp(path.data());
  if (fd < 0) {
    std::fprintf(stderr, "# no temp file (%s): file-backed read not exercised\n", std::strerror(errno));
    return;
  }
  const ssize_t written = ::write(fd, image.data(), image.size());
  ::close(fd);
  CHECK_CTX(written == static_cast<ssize_t>(image.size()), "write");
  std::string why;
  auto z = rb::loader::ZipReader::OpenFile(path.data(), &why);
  CHECK_CTX(z != nullptr, why);
  if (z) {
    auto got = z->Read("assets/rb32/libx.so", &why);
    CHECK_CTX(got.has_value(), why);
    if (got) CHECK(*got == lib);
  }
  std::remove(path.data());
}

}  // namespace

int main() {
  round_trips_stored_and_deflated();
  refuses_by_name();
  reads_from_a_file();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
