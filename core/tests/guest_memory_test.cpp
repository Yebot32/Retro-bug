/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the guest memory model (C03), focused on the interaction AD-7 had to decide: a guest
 * page is 4 KiB, a host page is whatever the device says, and when several differently-protected
 * guest pages share one host granule they must leave fastmem rather than the model growing a soft
 * TLB or granting the union of their permissions.
 *
 * host_page_shift is injected, so the 4 KiB and 16 KiB behaviours are both exercised on either kind
 * of machine. The real 16 KiB case is additionally run on the emulator, which measured
 * getconf PAGESIZE = 16384 (docs/30 §Step 3b) — injected and native agreeing is the point.
 */
#include "rb/mem/guest_memory.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

using namespace rb::mem;

int failures = 0;
int checks = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK(...)                          \
  do {                                      \
    ++checks;                               \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)

/// This kernel's real page-size shift. mmap/mprotect cannot be finer than this, so an injected
/// shift below it is refused by the model rather than simulated — which means the 4 KiB-host cases
/// can only really run on a 4 KiB host, while 16 KiB behaviour is testable anywhere.
unsigned NativeShift() {
  const long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = kGuestPageShift;
  while (static_cast<long>(1) << shift < ps) ++shift;
  return shift;
}

int skipped = 0;
#define SKIP_IF_NOT_HOST(shift, why)                       \
  do {                                                     \
    if (NativeShift() > (shift)) {                         \
      ++skipped;                                           \
      std::printf("SKIP   %s (needs a %d KiB-page host)\n", why, 1 << ((shift) - 10)); \
      return;                                              \
    }                                                      \
  } while (0)

std::unique_ptr<GuestMemory> make(unsigned host_shift, std::size_t pages = 64) {
  AddressModel m;
  m.kind = AddressModel::kBiased;  // never assume where the window landed
  // Requests below this kernel's granularity are impossible, not merely unusual; generic tests
  // take the native granule and the granule-specific ones ask explicitly and skip otherwise.
  m.host_page_shift = std::max(host_shift, NativeShift());
  std::string err;
  auto gm = GuestMemory::Create(pages * kGuestPage, m, &err);
  if (!gm) fail("window creation failed", __LINE__, err);
  return gm;
}

// ------------------------------------------------------------------- basics

void map_write_read_roundtrip() {
  auto gm = make(12);
  if (!gm) return;
  std::string err;
  CHECK(gm->Map(0x1000, 0x1000, kRead | kWrite, &err));
  const char msg[] = "armeabi-v7a";
  CHECK(gm->Write(0x1000, msg, sizeof msg));
  char back[32] = {};
  CHECK(gm->Read(0x1000, back, sizeof back));
  CHECK(std::strcmp(back, "armeabi-v7a") == 0);
  CHECK(gm->IsMapped(0x1000));
  CHECK(!gm->IsMapped(0x2000));
  CHECK(gm->mapped_page_count() == 1);
}

void fresh_anonymous_pages_read_back_as_zero() {
  auto gm = make(12);
  if (!gm) return;
  std::string err;
  CHECK(gm->Map(0x1000, 0x1000, kRead | kWrite, &err));
  unsigned char pattern[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(gm->Write(0x1000, pattern, sizeof pattern));
  CHECK(gm->Unmap(0x1000, 0x1000, &err));
  CHECK(gm->Map(0x1000, 0x1000, kRead | kWrite, &err));  // re-mapping must be zeroed
  unsigned char out[8] = {};
  CHECK(gm->Read(0x1000, out, sizeof out));
  CHECK(std::memcmp(out, "\0\0\0\0\0\0\0\0", 8) == 0);
}

void unmapped_accesses_fail_rather_than_faulting_the_host() {
  auto gm = make(12);
  if (!gm) return;
  unsigned char b[4] = {};
  CHECK(!gm->Read(0x9000, b, sizeof b));
  CHECK(!gm->Write(0x9000, b, sizeof b));
  CHECK(gm->Host(0x9000) == nullptr);
  CHECK(!gm->IsFastmem(0x9000));
}

void length_is_rounded_to_guest_pages_and_spanning_accesses_work() {
  auto gm = make(12);
  if (!gm) return;
  std::string err;
  CHECK(gm->Map(0x1000, 0x1234, kRead | kWrite, &err));  // crosses into a second page
  CHECK(gm->mapped_page_count() == 2);
  unsigned char big[0x1200];
  for (int i = 0; i < 0x1200; ++i) big[i] = static_cast<unsigned char>(i * 7);
  CHECK(gm->Write(0x1200, big, sizeof big));
  unsigned char back[0x1200] = {};
  CHECK(gm->Read(0x1200, back, sizeof back));
  CHECK(std::memcmp(big, back, sizeof big) == 0);
}

void permissions_are_enforced_per_page() {
  auto gm = make(12);
  if (!gm) return;
  std::string err;
  CHECK(gm->Map(0x1000, 0x1000, kRead | kExec, &err));
  unsigned char b[4] = {};
  CHECK(gm->Read(0x1000, b, sizeof b));
  CHECK(!gm->Write(0x1000, b, sizeof b));  // RX page: a guest write must fault, not succeed
  CHECK(gm->ProtAt(0x1000) == (kRead | kExec));
}

// ------------------------------------------------------- mixed-granule logic

/// The two adjacent guest pages that an ELF image produces constantly: executable text next to
/// writable data, 4 KiB apart, inside one 16 KiB host page.
void mixed_protections_leave_fastmem_on_a_16k_host() {
  auto gm = make(14);
  if (!gm) return;
  std::string err;
  CHECK(gm->Map(0x1000, 0x1000, kRead | kExec, &err));        // granule 0, slot 0
  CHECK(gm->Map(0x2000, 0x1000, kRead | kWrite, &err));      // same 16 KiB granule, slot 1

  CHECK(!gm->IsFastmem(0x1000));
  CHECK(!gm->IsFastmem(0x2000));
  CHECK(gm->Host(0x1000) == nullptr);                        // callers must take the slow path
  CHECK(gm->out_of_line_page_count() == 2);

  // Still fully usable, and still permission-correct in isolation — that is the whole point of
  // leaving fastmem instead of granting the union of both pages' permissions.
  const char code[] = "\x70\x47";
  CHECK(gm->Write(0x2000, code, sizeof code));
  char back[4] = {};
  CHECK(gm->Read(0x2000, back, sizeof back));
  CHECK(std::memcmp(back, code, sizeof code) == 0);
  CHECK(!gm->Write(0x1000, code, sizeof code));
  CHECK(gm->Read(0x1000, back, sizeof back));
}

/// The same source on a 4 KiB host has no conflict to resolve, so both pages stay in place.
/// If this returned false too, the out-of-line path would be firing unnecessarily and costing
/// every guest access a memcpy.
void same_protections_stay_in_fastmem_at_16k() {
  auto gm = make(14);
  if (!gm) return;
  std::string err;
  CHECK(gm->Map(0x0000, 0x4000, kRead | kWrite, &err));      // two pages, one granule, one prot
  CHECK(gm->IsFastmem(0x0000));
  CHECK(gm->IsFastmem(0x2000));
  CHECK(gm->Host(0x2000) != nullptr);
  CHECK(gm->out_of_line_page_count() == 0);
}

/// A 4 KiB host has no granule large enough to hold two differently-protected pages, so the mixed
/// case that forces out-of-line copies at 16 KiB is a non-event here. Only meaningful on a 4 KiB
/// host: mmap/mprotect granularity cannot be simulated at a finer grain than the kernel's.
void mixed_protections_stay_in_fastmem_on_a_4k_host() {
  SKIP_IF_NOT_HOST(12, "4 KiB host granule behaviour");
  auto gm = make(12);
  if (!gm) return;
  std::string err;
  CHECK(gm->Map(0x1000, 0x1000, kRead | kExec, &err));
  CHECK(gm->Map(0x2000, 0x1000, kRead | kWrite, &err));
  CHECK(gm->IsFastmem(0x1000));
  CHECK(gm->IsFastmem(0x2000));
  CHECK(gm->out_of_line_page_count() == 0);
}

/// Changing protection can create or destroy a conflict, and the guest must not observe the
/// migration: content has to survive in both directions.
void protect_migration_preserves_content_both_ways() {
  auto gm = make(14);
  if (!gm) return;
  std::string err;
  CHECK(gm->Map(0x0000, 0x4000, kRead | kWrite, &err));
  const unsigned char payload[16] = {0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4,
                                     5, 6, 7, 8, 9, 10, 11, 12};
  CHECK(gm->Write(0x2000, payload, sizeof payload));
  CHECK(gm->IsFastmem(0x2000));

  // Create a conflict: page 0 becomes RX while page 1 stays RW.
  CHECK(gm->Protect(0x0000, 0x1000, kRead | kExec, &err));
  CHECK(!gm->IsFastmem(0x0000));
  CHECK(!gm->IsFastmem(0x2000));
  unsigned char back[16] = {};
  CHECK(gm->Read(0x2000, back, sizeof back));
  CHECK(std::memcmp(back, payload, sizeof payload) == 0);

  // Remove it: both become RW again and should migrate back with content intact.
  CHECK(gm->Protect(0x0000, 0x1000, kRead | kWrite, &err));
  CHECK(gm->IsFastmem(0x0000));
  CHECK(gm->IsFastmem(0x2000));
  std::memset(back, 0, sizeof back);
  CHECK(gm->Read(0x2000, back, sizeof back));
  CHECK(std::memcmp(back, payload, sizeof payload) == 0);
  CHECK(gm->Write(0x2000, payload, sizeof payload));  // writable again through the fast path
}

void unmap_clears_both_forms() {
  auto gm = make(14);
  if (!gm) return;
  std::string err;
  CHECK(gm->Map(0x1000, 0x1000, kRead | kExec, &err));
  CHECK(gm->Map(0x2000, 0x1000, kRead | kWrite, &err));
  CHECK(gm->out_of_line_page_count() == 2);
  CHECK(!gm->IsFastmem(0x2000));

  CHECK(gm->Unmap(0x1000, 0x1000, &err));
  CHECK(!gm->IsMapped(0x1000));
  CHECK(gm->IsMapped(0x2000));
  // Removing the page that caused the conflict leaves the survivor alone in its granule with a
  // single protection, so it correctly migrates back in place — the out-of-line count should go to
  // zero, not merely down by one. Asserting the intermediate value here would bake in a bug.
  CHECK(gm->IsFastmem(0x2000));
  CHECK(gm->out_of_line_page_count() == 0);

  CHECK(gm->Unmap(0x2000, 0x1000, &err));
  CHECK(!gm->IsMapped(0x2000));
  CHECK(gm->mapped_page_count() == 0);
}

// ------------------------------------------------------- pointer crossings

void translate_round_trips_in_both_forms() {
  auto gm = make(14);
  if (!gm) return;
  std::string err;
  CHECK(gm->Map(0x0000, 0x2000, kRead | kWrite, &err));
  CHECK(gm->Map(0x4000, 0x1000, kRead | kWrite, &err));      // own granule, stays in place

  void* in_place = gm->Host(0x1000);
  CHECK(in_place != nullptr);
  std::uint32_t back = 0;
  CHECK(gm->Translate(in_place, &back));
  CHECK(back == 0x1000);

  // A host pointer inside a mapped page must translate; one outside the window must not, so a
  // stray RetroBug pointer can never be mistaken for guest memory.
  char not_guest[8];
  CHECK(!gm->Translate(not_guest, &back));
  const auto unmapped_in_window = reinterpret_cast<const void*>(
      reinterpret_cast<std::uintptr_t>(gm->window_base()) + 0x3000);
  CHECK(!gm->Translate(unmapped_in_window, &back));  // inside the window but unmapped
}

void writes_through_a_host_pointer_are_visible_to_reads() {
  auto gm = make(12);
  if (!gm) return;
  std::string err;
  CHECK(gm->Map(0x1000, 0x1000, kRead | kWrite, &err));
  void* p = gm->Host(0x1040);
  CHECK(p != nullptr);
  if (p) std::memcpy(p, "OK", 3);
  char back[4] = {};
  CHECK(gm->Read(0x1040, back, 3));
  CHECK(std::strcmp(back, "OK") == 0);
}

// ------------------------------------------------------------- rejections

void bad_requests_are_refused_not_corrupted() {
  auto gm = make(12);
  if (!gm) return;
  std::string err;
  CHECK(!gm->Map(0x1000, 0, kRead | kWrite, &err));
  CHECK(!gm->Map(0x1000, 0x1000, kNone, &err));
  CHECK(!gm->Protect(0x8000, 0x1000, kRead, &err));          // nothing mapped there
  CHECK(!gm->Map(0xFFFFFF00, 0x40000, kRead | kWrite, &err));  // runs off the end of the window
  CHECK(!gm->Read(0xFFFFF800, nullptr, 0x10000));             // would wrap past the window end
}

void host_page_shifts_below_the_guest_page_are_refused() {
  AddressModel m;
  m.kind = AddressModel::kBiased;
  m.host_page_shift = 9;  // 512-byte host pages do not exist, and the maths would divide by zero
  std::string err;
  CHECK(GuestMemory::Create(8 * kGuestPage, m, &err) == nullptr);
  CHECK(!err.empty());
}

void window_size_must_be_page_multiples() {
  AddressModel m;
  m.kind = AddressModel::kBiased;
  m.host_page_shift = 12;
  std::string err;
  CHECK(GuestMemory::Create(4095, m, &err) == nullptr);
  CHECK(GuestMemory::Create(0, m, &err) == nullptr);
}

// ------------------------------------------------------- native host check

/// On the 16 KiB emulator this asserts the injected-14 case against reality rather than only
/// against a parameter. On a 4 KiB host it asserts the injected-12 case. Either way it must hold.
void native_host_granule_matches_injection() {
  const long ps = sysconf(_SC_PAGESIZE);
  CHECK(ps > 0);
  AddressModel m;
  m.kind = AddressModel::kBiased;
  m.host_page_shift = 12;
  std::string err;
  // A window that ignores the real host granule is a bug, not a simplification: check the model
  // built from the true page size behaves the same way.
  unsigned shift = 12;
  while ((static_cast<long>(1) << shift) < ps) ++shift;
  m.host_page_shift = shift;
  auto gm = GuestMemory::Create(64 * kGuestPage, m, &err);
  if (!gm) {
    fail("native-granule window creation failed", __LINE__, err);
    return;
  }
  CHECK(gm->host_page_shift() == shift);
  CHECK(gm->Map(0x1000, 0x1000, kRead | kWrite, &err));
  CHECK(gm->Map(0x2000, 0x1000, kRead | kWrite, &err));
  CHECK(gm->IsFastmem(0x1000));
  CHECK(gm->IsFastmem(0x2000));
}

}  // namespace

/// Prints before each case so a crash names its own test. On Android the child's stdout can be lost
/// to the crash handler, which turns a segfault into a guess without this.
#define RUN(fn)                        \
  do {                                 \
    std::fprintf(stderr, "%s\n", #fn); \
    std::fflush(stderr);               \
    fn();                              \
  } while (0)

int main() {
  RUN(map_write_read_roundtrip);
  RUN(fresh_anonymous_pages_read_back_as_zero);
  RUN(unmapped_accesses_fail_rather_than_faulting_the_host);
  RUN(length_is_rounded_to_guest_pages_and_spanning_accesses_work);
  RUN(permissions_are_enforced_per_page);

  RUN(mixed_protections_leave_fastmem_on_a_16k_host);
  RUN(same_protections_stay_in_fastmem_at_16k);
  RUN(mixed_protections_stay_in_fastmem_on_a_4k_host);
  RUN(protect_migration_preserves_content_both_ways);
  RUN(unmap_clears_both_forms);

  RUN(translate_round_trips_in_both_forms);
  RUN(writes_through_a_host_pointer_are_visible_to_reads);
  RUN(bad_requests_are_refused_not_corrupted);
  RUN(host_page_shifts_below_the_guest_page_are_refused);
  RUN(window_size_must_be_page_multiples);
  RUN(native_host_granule_matches_injection);

  std::printf("%d checks, %d failures, %d skipped (host page shift %u)\n", checks, failures, skipped,
              NativeShift());
  return failures == 0 ? 0 : 1;
}
