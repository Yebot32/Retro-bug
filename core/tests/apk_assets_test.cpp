// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// `rb::loader::ApkAssets` (S1): the byte-stream view of an APK's entries the iOS AAsset thunks read through.
// No device and no guest — the archive is written by the test.

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rb/loader/apk_assets.h"
#include "zip_builder.h"

namespace {

using rb::loader::ApkAssets;
using rb::test::Bytes;
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

std::string Str(char* buf, std::int64_t n) { return std::string(buf, static_cast<std::size_t>(n)); }

void reads_a_stored_entry() {
  ZipWriter w;
  w.AddStored("assets/hello.txt", Bytes("hello, apk"));
  std::string why;
  auto assets = ApkAssets::OpenMemory(w.Finish(), &why);
  CHECK_CTX(assets != nullptr, why);
  if (!assets) return;

  CHECK(assets->Has("assets/hello.txt"));
  CHECK(!assets->Has("assets/none.txt"));
  CHECK_CTX(assets->Names().size() == 1, std::to_string(assets->Names().size()));
  CHECK_CTX(assets->OpenEntry("assets/hello.txt", &why), why);
  CHECK_CTX(assets->Length() == 10, std::to_string(assets->Length()));

  char buf[16] = {};
  CHECK_CTX(assets->Read(buf, 5) == 5, "first read");
  CHECK_CTX(Str(buf, 5) == "hello", Str(buf, 5));
  CHECK_CTX(assets->Position() == 5, std::to_string(assets->Position()));

  CHECK_CTX(assets->Seek(0, 0) == 0, "SEEK_SET");
  CHECK_CTX(assets->Read(buf, 10) == 10, "whole read");
  CHECK_CTX(Str(buf, 10) == "hello, apk", Str(buf, 10));
  CHECK_CTX(assets->Read(buf, 4) == 0, "read past the end");

  CHECK_CTX(assets->Seek(-3, 2) == 7, "SEEK_END");
  CHECK_CTX(assets->Read(buf, 8) == 3, "tail read");
  CHECK_CTX(Str(buf, 3) == "apk", Str(buf, 3));

  // A seek past the end is not an error, and the next read is empty; a seek before the start is.
  CHECK_CTX(assets->Seek(100, 0) == 100, "seek past the end");
  CHECK(assets->Read(buf, 4) == 0);
  CHECK(assets->Seek(-100, 0) == -1);
  CHECK(assets->Seek(0, 9) == -1);
}

void reads_a_deflated_entry() {
  std::vector<std::uint8_t> big;
  for (int i = 0; i < 5000; ++i) {
    const char* line = "a level file, repeated so deflate and the 64 KiB inflate chunk both run\n";
    big.insert(big.end(), line, line + std::strlen(line));
  }
  ZipWriter w;
  w.AddDeflated("assets/levels/one.dat", big);
  std::string why;
  auto assets = ApkAssets::OpenMemory(w.Finish(), &why);
  CHECK_CTX(assets != nullptr, why);
  if (!assets) return;
  CHECK_CTX(assets->OpenEntry("assets/levels/one.dat", &why), why);
  CHECK_CTX(assets->Length() == static_cast<std::int64_t>(big.size()), std::to_string(assets->Length()));
  std::vector<std::uint8_t> round(big.size());
  CHECK_CTX(assets->Read(round.data(), static_cast<std::int64_t>(round.size())) ==
                static_cast<std::int64_t>(big.size()),
            "read");
  CHECK(round == big);
}

void refuses_a_missing_entry() {
  ZipWriter w;
  w.AddStored("assets/hello.txt", Bytes("hi"));
  std::string why;
  auto assets = ApkAssets::OpenMemory(w.Finish(), &why);
  CHECK(assets != nullptr);
  if (!assets) return;
  CHECK(!assets->OpenEntry("assets/missing.txt", &why));
  CHECK_CTX(why.find("no entry named assets/missing.txt") != std::string::npos, why);
}

void opens_from_a_file() {
  ZipWriter w;
  w.AddStored("assets/hello.txt", Bytes("file-backed"));
  const std::vector<std::uint8_t> image = w.Finish();
  const char* dir = std::getenv("TMPDIR");
  std::string tmpl = std::string(dir != nullptr && *dir != '\0' ? dir : "/data/local/tmp") + "/rb_apk_XXXXXX";
  std::vector<char> path(tmpl.begin(), tmpl.end());
  path.push_back('\0');
  const int fd = mkstemp(path.data());
  if (fd < 0) {
    std::fprintf(stderr, "# no temp file (%s): file-backed open not exercised\n", std::strerror(errno));
    return;
  }
  const ssize_t n = ::write(fd, image.data(), image.size());
  ::close(fd);
  CHECK(n == static_cast<ssize_t>(image.size()));
  std::string why;
  auto assets = ApkAssets::Open(path.data(), &why);
  CHECK_CTX(assets != nullptr, why);
  if (assets) {
    CHECK_CTX(assets->OpenEntry("assets/hello.txt", &why), why);
    char buf[16] = {};
    const auto got = assets->Read(buf, sizeof buf);
    CHECK_CTX(Str(buf, got) == "file-backed", Str(buf, got));
  }
  std::remove(path.data());
}

}  // namespace

int main() {
  reads_a_stored_entry();
  reads_a_deflated_entry();
  refuses_a_missing_entry();
  opens_from_a_file();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
