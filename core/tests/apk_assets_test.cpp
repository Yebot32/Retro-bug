// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// `rb::loader::ApkArchive`/`ApkFile` (S1): the byte-stream view of an APK's entries the iOS AAsset thunks
// read through. No device and no guest — the archive is written by the test.

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

using rb::loader::ApkArchive;
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
  auto archive = ApkArchive::OpenMemory(w.Finish(), &why);
  CHECK_CTX(archive != nullptr, why);
  if (!archive) return;

  CHECK(archive->Has("assets/hello.txt"));
  CHECK(!archive->Has("assets/none.txt"));
  CHECK_CTX(archive->Names().size() == 1, std::to_string(archive->Names().size()));
  auto file = archive->OpenFile("assets/hello.txt", &why);
  CHECK_CTX(file != nullptr, why);
  if (!file) return;
  CHECK_CTX(file->Length() == 10, std::to_string(file->Length()));

  char buf[16] = {};
  CHECK_CTX(file->Read(buf, 5) == 5, "first read");
  CHECK_CTX(Str(buf, 5) == "hello", Str(buf, 5));
  CHECK_CTX(file->Position() == 5, std::to_string(file->Position()));

  CHECK_CTX(file->Seek(0, 0) == 0, "SEEK_SET");
  CHECK_CTX(file->Read(buf, 10) == 10, "whole read");
  CHECK_CTX(Str(buf, 10) == "hello, apk", Str(buf, 10));
  CHECK_CTX(file->Read(buf, 4) == 0, "read past the end");

  CHECK_CTX(file->Seek(-3, 2) == 7, "SEEK_END");
  CHECK_CTX(file->Read(buf, 8) == 3, "tail read");
  CHECK_CTX(Str(buf, 3) == "apk", Str(buf, 3));

  CHECK_CTX(file->Seek(100, 0) == 100, "seek past the end");
  CHECK(file->Read(buf, 4) == 0);
  CHECK(file->Seek(-100, 0) == -1);
  CHECK(file->Seek(0, 9) == -1);
}

void two_entries_have_independent_cursors() {
  ZipWriter w;
  w.AddStored("assets/a.txt", Bytes("aaaa"));
  w.AddStored("assets/b.txt", Bytes("bbbbbb"));
  std::string why;
  auto archive = ApkArchive::OpenMemory(w.Finish(), &why);
  CHECK(archive != nullptr);
  if (!archive) return;
  auto a = archive->OpenFile("assets/a.txt", &why);
  auto b = archive->OpenFile("assets/b.txt", &why);
  CHECK(a != nullptr && b != nullptr);
  if (!a || !b) return;
  char buf[8] = {};
  CHECK(a->Read(buf, 2) == 2 && Str(buf, 2) == "aa");
  CHECK(b->Read(buf, 3) == 3 && Str(buf, 3) == "bbb");
  CHECK(a->Read(buf, 2) == 2 && Str(buf, 2) == "aa");
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
  auto archive = ApkArchive::OpenMemory(w.Finish(), &why);
  CHECK_CTX(archive != nullptr, why);
  if (!archive) return;
  auto file = archive->OpenFile("assets/levels/one.dat", &why);
  CHECK_CTX(file != nullptr, why);
  if (!file) return;
  CHECK_CTX(file->Length() == static_cast<std::int64_t>(big.size()), std::to_string(file->Length()));
  std::vector<std::uint8_t> round(big.size());
  CHECK_CTX(file->Read(round.data(), static_cast<std::int64_t>(round.size())) ==
                static_cast<std::int64_t>(big.size()),
            "read");
  CHECK(round == big);
}

void refuses_a_missing_entry() {
  ZipWriter w;
  w.AddStored("assets/hello.txt", Bytes("hi"));
  std::string why;
  auto archive = ApkArchive::OpenMemory(w.Finish(), &why);
  CHECK(archive != nullptr);
  if (!archive) return;
  CHECK(archive->OpenFile("assets/missing.txt", &why) == nullptr);
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
  auto archive = ApkArchive::Open(path.data(), &why);
  CHECK_CTX(archive != nullptr, why);
  if (archive) {
    auto file = archive->OpenFile("assets/hello.txt", &why);
    CHECK_CTX(file != nullptr, why);
    if (file) {
      char buf[16] = {};
      const auto got = file->Read(buf, sizeof buf);
      CHECK_CTX(Str(buf, got) == "file-backed", Str(buf, got));
    }
  }
  std::remove(path.data());
}

}  // namespace

int main() {
  reads_a_stored_entry();
  two_entries_have_independent_cursors();
  reads_a_deflated_entry();
  refuses_a_missing_entry();
  opens_from_a_file();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
