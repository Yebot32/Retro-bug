// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// SHA-256 as the profile verifier uses it: the FIPS 180-4 vectors (empty, "abc", the two-block message
// and a million 'a's) plus a cross-check that the tool's copy and the app's copy agree. No guest, no
// device: this runs on every host lane, like the other fixture-free suites.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "rb/crypto/sha256.h"

namespace {

int checks = 0;
int failures = 0;

void Check(bool ok, const char* what, const std::string& detail = {}) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL  %s%s%s\n", what, detail.empty() ? "" : "  ", detail.c_str());
}

void CheckHex(const std::string& input, const char* expected, const char* what) {
  const auto digest = rb::crypto::Sha256(reinterpret_cast<const std::uint8_t*>(input.data()),
                                        input.size());
  const std::string hex = rb::crypto::Sha256Hex(digest);
  Check(hex == expected, what, hex + " != " + expected);
}

void run() {
  CheckHex("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "the empty string");
  CheckHex("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "\"abc\"");
  CheckHex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
           "the two-block message");
  std::string million(1000000, 'a');
  CheckHex(million, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
           "one million 'a's");
  // A cross-check value shared with the tool: `rb_aotgen` hashes each library it translates, and the app
  // re-hashes the imported APK's entries. Both copies must produce this digest for the same bytes.
  CheckHex("RetroBug AOT", "073d6f9284501b9e766dcc35b174bc5c20db6b370b57e8e3cf772ba0a7a551f1",
           "the cross-check message");
}

}  // namespace

int main() {
  run();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
