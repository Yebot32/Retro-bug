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

#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

#define CHECK_CTX(cond, detail)              \
  do {                                       \
    ++checks;                                \
    if (!(cond)) fail(#cond, __LINE__, detail); \
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

/// A private file mapping has to behave like a kernel's: changing its protection changes permission
/// and nothing else.
///
/// This is the shape the ELF loader drives the model through on every single load — map a segment
/// writable, write its relocations into it, then drop the write bit for RELRO and text. If the
/// protection change re-establishes the mapping from the fd instead, every word the loader wrote
/// reverts to the file's copy, and the guest runs with the linker's placeholder GOT while the load
/// reports complete success. The anonymous migration test above cannot catch that: an anonymous
/// granule has no fd to fall back to.
void file_backed_writes_survive_a_protection_change() {
  // 12 and 14 KiB shifts cover the two rebalancing forms: at 4 KiB every granule is one page, so a
  // protection change stays in place; at 16 KiB four differently-protected guest pages share one
  // granule and have to migrate out of line. Both must preserve content.
  for (const unsigned shift : {12u, 14u}) {
    auto gm = make(shift);
    if (!gm) return;
    std::string err;
    const std::size_t n = 4 * kGuestPage;
    int fd = -1;
#if defined(SYS_memfd_create)
    fd = static_cast<int>(syscall(SYS_memfd_create, "rb-file-pages", 0u));
#endif
    if (fd < 0) {
      ++skipped;
      std::printf("SKIP   file-backed protection change (no memfd: %s)\n", std::strerror(errno));
      return;
    }
    std::vector<unsigned char> content(n);
    for (std::size_t i = 0; i < n; ++i) content[i] = static_cast<unsigned char>(i * 31 + 7);
    for (std::size_t done = 0; done < n;) {
      const auto w = ::pwrite(fd, content.data() + done, n - done, static_cast<off_t>(done));
      if (w <= 0) break;
      done += static_cast<std::size_t>(w);
    }
    CHECK(gm->MapFile(0x0, n, kRead | kWrite, fd, 0, /*shared=*/false, &err));
    // The caller closes its own descriptor straight away, exactly as an ELF loader will: a mapping
    // holds its own reference to the file, so nothing further may depend on this fd staying open.
    // Everything below still has to work, including the re-granulations, which remap from the file.
    // `MapFile` used to keep the number it was handed rather than duplicating it, and the first
    // symptom was a `Protect` failing with EBADF long after the load.
    ::close(fd);

    // Two words, in different granules at the 16 KiB shift, and a sentinel the file does not contain.
    const std::uint32_t marker = 0xA1B2C3D4u;
    CHECK(gm->Write(0x0008, &marker, sizeof marker));
    CHECK(gm->Write(0x2008, &marker, sizeof marker));

    // Uniform change: the granule stays in place.
    CHECK(gm->Protect(0x0, n, kRead | kExec, &err));
    std::uint32_t got = 0;
    CHECK(gm->Read(0x0008, &got, sizeof got));
    CHECK_CTX(got == marker, "shift " + std::to_string(shift) + " page 0 after mprotect: 0x" +
                                 std::to_string(got));
    got = 0;
    CHECK(gm->Read(0x2008, &got, sizeof got));
    CHECK_CTX(got == marker, "shift " + std::to_string(shift) + " page 2 after mprotect");
    CHECK(gm->Write(0x0008, &marker, sizeof marker) == false);  // the new permission is real
    unsigned char unchanged[4] = {};
    CHECK(gm->Read(0x1000, unchanged, sizeof unchanged));
    CHECK_CTX(std::memcmp(unchanged, content.data() + 0x1000, 4) == 0,
              "untouched file bytes must still read as the file's");

    // Conflicting protection in one granule: migrates out of line, and must still hold the writes.
    CHECK(gm->Protect(0x1000, 0x1000, kRead | kWrite, &err));
    got = 0;
    CHECK(gm->Read(0x0008, &got, sizeof got));
    CHECK_CTX(got == marker, "shift " + std::to_string(shift) + " page 0 after leaving fastmem");
    got = 0;
    CHECK(gm->Read(0x2008, &got, sizeof got));
    CHECK_CTX(got == marker, "shift " + std::to_string(shift) + " page 2 after leaving fastmem");
  }
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
  // `Map(..., kNone)` is a *guard reservation* now, not a rejection: this line asserted the old contract
  // while `guard_pages_are_representable_and_re_armable` below asserted the new one, so the two halves of
  // this file disagreed and the binary could not go green. What is still refused is a *file* mapping with
  // no permissions — it would map nothing and report success, which is the shape the guard reservation
  // replaced. See the `prot == kNone` case in `MapFile`.
  CHECK(!gm->MapFile(0x1000, 0x1000, kNone, -1, 0, false, &err));
  CHECK(!gm->Protect(0x8000, 0x1000, kRead, &err));          // nothing mapped there
  CHECK(!gm->Map(0xFFFFFF00, 0x40000, kRead | kWrite, &err));  // runs off the end of the window
  CHECK(!gm->Read(0xFFFFF800, nullptr, 0x10000));             // would wrap past the window end
}

/// `err` is an output, not an input. A string left dirty by an earlier refusal used to make the next
/// *successful* map roll itself back and return false, because success was inferred from "`err` stayed
/// empty" and nothing cleared it on entry. The launcher reuses one `err` across its whole map sequence,
/// so this is the shape that reaches a device — and the rollback it caused then made the report blame the
/// guest image for a page the memory model had just refused to keep.
void a_dirty_error_string_does_not_poison_a_later_map() {
  auto gm = make(12);
  if (!gm) return;
  std::string err = "text left here by an earlier refusal";
  CHECK(gm->Map(0x1000, 0x1000, kRead | kWrite, &err));
  CHECK(err.empty());  // the successful call owns the string and leaves it empty
  CHECK(gm->IsMapped(0x1000));
  CHECK(gm->Host(0x1000) != nullptr);
  // The refusal path still reports: clearing on entry must not turn a rejection into silence.
  CHECK(!gm->Map(0x1000, 0, kRead | kWrite, &err));
  CHECK(!err.empty());
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

// ------------------------------------------------------- guard pages

/// A `kNone` map is a guard reservation: mapped (so the loader's placement scan skips it), with no
/// permissions (so every access refuses), and *re-armable* — the first draft's Protect skipped
/// `kNone` pages, which made a guard page a one-way trap.
void guard_pages_are_representable_and_re_armable() {
  auto gm = make(12);
  if (!gm) return;
  std::string err;
  CHECK(gm->Map(0x1000, 0x1000, kNone, &err));
  CHECK(gm->IsMapped(0x1000));  // claimed, so placement scans and Protect see it
  CHECK(gm->mapped_page_count() == 1);
  CHECK(gm->ProtAt(0x1000) == kNone);
  unsigned char b[4] = {};
  CHECK(!gm->Read(0x1000, b, sizeof b));
  CHECK(!gm->Write(0x1000, b, sizeof b));
  CHECK(gm->Host(0x1000) == nullptr);
  CHECK(!gm->IsFastmem(0x1000));

  // Re-arm: a guard page can become live.
  CHECK(gm->Protect(0x1000, 0x1000, kRead | kWrite, &err));
  const std::uint32_t marker = 0x0BADF00Du;
  CHECK(gm->Write(0x1000, &marker, sizeof marker));
  std::uint32_t got = 0;
  CHECK(gm->Read(0x1000, &got, sizeof got));
  CHECK(got == marker);

  // And back: re-protect to kNone re-traps it.
  CHECK(gm->Protect(0x1000, 0x1000, kNone, &err));
  CHECK(!gm->Read(0x1000, &got, sizeof got));
  CHECK(!gm->Write(0x1000, &marker, sizeof marker));
}

/// The in-place file form needs every page in the granule to be the same file at consecutive
/// offsets. The first draft took the first live page's fd and offset for the whole granule, so two
/// files sharing a host granule both read back the first one's bytes. With the contiguity rule the
/// granule goes out of line and each page reads its own file.
void two_files_in_one_granule_read_their_own_bytes() {
  auto gm = make(14);  // one granule holds four guest pages on the injected shift
  if (!gm) return;
  std::string err;
  int fds[2] = {-1, -1};
#if defined(SYS_memfd_create)
  fds[0] = static_cast<int>(syscall(SYS_memfd_create, "rb-file-a", 0u));
  fds[1] = static_cast<int>(syscall(SYS_memfd_create, "rb-file-b", 0u));
#endif
  if (fds[0] < 0 || fds[1] < 0) {
    ++skipped;
    std::printf("SKIP   two-files-in-one-granule (no memfd: %s)\n", std::strerror(errno));
    return;
  }
  std::vector<unsigned char> a(kGuestPage, 0xAA), b(kGuestPage, 0xBB);
  CHECK(::pwrite(fds[0], a.data(), a.size(), 0) == static_cast<ssize_t>(a.size()));
  CHECK(::pwrite(fds[1], b.data(), b.size(), 0) == static_cast<ssize_t>(b.size()));

  CHECK(gm->MapFile(0x0000, kGuestPage, kRead | kWrite, fds[0], 0, false, &err));
  CHECK(gm->MapFile(0x1000, kGuestPage, kRead | kWrite, fds[1], 0, false, &err));
  unsigned char back[kGuestPage] = {};
  CHECK(gm->Read(0x0000, back, sizeof back));
  CHECK_CTX(std::memcmp(back, a.data(), kGuestPage) == 0,
            "page 0 read file B's bytes, not file A's");
  CHECK(gm->Read(0x1000, back, sizeof back));
  CHECK_CTX(std::memcmp(back, b.data(), kGuestPage) == 0,
            "page 1 read file A's bytes, not file B's");
  ::close(fds[0]);
  ::close(fds[1]);
}

/// A shared file mapping has to be creatable at all: the first draft set MAP_PRIVATE and MAP_SHARED
/// together, which is EINVAL on Linux, so every uniform shared granule failed outright. With the
/// flag fixed, a write through the file (from the caller's own fd) is visible through the mapping —
/// which is the property "shared" promises.
void shared_file_mapping_succeeds_and_stays_shared() {
  auto gm = make(12);
  if (!gm) return;
  std::string err;
  int fd = -1;
#if defined(SYS_memfd_create)
  fd = static_cast<int>(syscall(SYS_memfd_create, "rb-shared", 0u));
#endif
  if (fd < 0) {
    ++skipped;
    std::printf("SKIP   shared file mapping (no memfd: %s)\n", std::strerror(errno));
    return;
  }
  const std::vector<unsigned char> first(kGuestPage, 0x11);
  CHECK(::pwrite(fd, first.data(), first.size(), 0) == static_cast<ssize_t>(first.size()));
  CHECK(gm->MapFile(0x1000, kGuestPage, kRead | kWrite, fd, 0, /*shared=*/true, &err));
  unsigned char back[4] = {};
  CHECK(gm->Read(0x1000, back, sizeof back));
  CHECK(back[0] == 0x11);

  // The sharing, not just the creation: the file moves, the mapping follows.
  const std::vector<unsigned char> second(kGuestPage, 0x22);
  CHECK(::pwrite(fd, second.data(), second.size(), 0) == static_cast<ssize_t>(second.size()));
  CHECK(gm->Read(0x1000, back, sizeof back));
  CHECK_CTX(back[0] == 0x22, "a write to the shared file did not reach the mapping");
  ::close(fd);
}

/// The in-place file form mmaps from the *granule base*, so the file offset it uses must back up from
/// the first *live* page by the unmapped pages before it. Using the front page's offset there shifted
/// every live page's bytes by that gap — invisible to every row that maps at 0x0, and live on a 16 KiB
/// host for the very first image, which the loader places at 0x2000 (granule 0, pages 0-1 unmapped):
/// the guest would read bytes from the wrong file offset while the load reported success.
void a_file_in_a_granule_with_unmapped_leading_pages_reads_its_own_offset() {
  auto gm = make(14);  // one granule holds four guest pages on the injected shift
  if (!gm) return;
  std::string err;
  int fd = -1;
#if defined(SYS_memfd_create)
  fd = static_cast<int>(syscall(SYS_memfd_create, "rb-shift", 0u));
#endif
  if (fd < 0) {
    ++skipped;
    std::printf("SKIP   granule-base offset (no memfd: %s)\n", std::strerror(errno));
    return;
  }
  // Six pages, each stamped with its own index at the first byte, so a shifted window reads a
  // *different* page's stamp rather than the same bytes from somewhere else.
  std::vector<unsigned char> content(6 * kGuestPage, 0);
  for (std::size_t pg = 0; pg < 6; ++pg) {
    content[pg * kGuestPage] = static_cast<unsigned char>(0xA0 + pg);
  }
  CHECK(::pwrite(fd, content.data(), content.size(), 0) == static_cast<ssize_t>(content.size()));

  // Guest 0x2000 is granule 0's third page and its bytes live at file offset 0x2000, so the granule's
  // in-place window must start at 0x2000 - 2 * 4 KiB = 0. The front page's own offset (0x2000) mapped
  // at the base would instead serve guest 0x2000 from file offset 0x4000 — stamp 0xA4 below.
  CHECK(gm->MapFile(0x2000, kGuestPage, kRead | kWrite, fd, 0x2000, false, &err));
  CHECK(gm->MapFile(0x3000, kGuestPage, kRead | kWrite, fd, 0x3000, false, &err));
  unsigned char back = 0;
  CHECK(gm->Read(0x2000, &back, 1));
  CHECK_CTX(back == 0xA2,
            "guest 0x2000 read stamp " + std::to_string(back) + " (want 162 = 0xA2) — the granule's"
            " leading unmapped pages shifted the file window");
  CHECK(gm->Read(0x3000, &back, 1));
  CHECK_CTX(back == 0xA3, "guest 0x3000 read stamp " + std::to_string(back) + " (want 163)");
  // The two skipped pages must stay unmapped: the in-place window covers their host range, but their
  // table entries were never made, so guest reads there still refuse.
  CHECK(!gm->Read(0x0000, &back, 1));
  ::close(fd);

  // The same shape as the first image of a real load — file offset 0 at guest 0x2000, which is the
  // case that bit on the 16 KiB device class. It also has the *other* kernel's discrimination: the
  // phase above only fails under the old code when the real page is 4 KiB (a 16 KiB kernel makes the
  // old alignment check fail `0x2000 & 0x3FFF` and take the correct out-of-line path by luck), while
  // this shape takes the wrong in-place path under 4 KiB *and* 16 KiB kernels alike.
  auto gm0 = make(14);
  if (!gm0) return;
  int fd0 = -1;
#if defined(SYS_memfd_create)
  fd0 = static_cast<int>(syscall(SYS_memfd_create, "rb-shift0", 0u));
#endif
  if (fd0 < 0) {
    ++skipped;
    std::printf("SKIP   first-image offset (no memfd: %s)\n", std::strerror(errno));
    return;
  }
  CHECK(::pwrite(fd0, content.data(), content.size(), 0) == static_cast<ssize_t>(content.size()));
  CHECK(gm0->MapFile(0x2000, kGuestPage, kRead | kWrite, fd0, 0, false, &err));
  CHECK(gm0->MapFile(0x3000, kGuestPage, kRead | kWrite, fd0, kGuestPage, false, &err));
  CHECK(gm0->Read(0x2000, &back, 1));
  CHECK_CTX(back == 0xA0,
            "guest 0x2000 read stamp " + std::to_string(back) + " (want 160 = 0xA0, file offset 0)"
            " — the old code served file offset 0x2000 here, the first-image shift");
  CHECK(gm0->Read(0x3000, &back, 1));
  CHECK_CTX(back == 0xA1, "guest 0x3000 read stamp " + std::to_string(back) + " (want 161)");
  ::close(fd0);
}

// ------------------------------------------------------- pointer crossings

/// A host callee that dereferences guest memory — a GL driver handed a shader string, a vertex array or an
/// output buffer — needs the bytes, not the fastmem property. A file-backed page whose granule cannot be
/// served in place (the file's bytes do not reach back to the granule base) is out of line on every host;
/// `Host` refuses it by design, and `HostForAccess` must answer its backing block.
void host_for_access_answers_out_of_line_pages() {
  auto gm = make(14);
  if (!gm) return;
  std::string err;
  int fd = -1;
#if defined(SYS_memfd_create)
  fd = static_cast<int>(syscall(SYS_memfd_create, "rb-access", 0u));
#endif
  if (fd < 0) {
    ++skipped;
    std::printf("SKIP   host access to an out-of-line page (no memfd: %s)\n", std::strerror(errno));
    return;
  }
  const unsigned char content[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  CHECK(::pwrite(fd, content, sizeof content, 0) == static_cast<ssize_t>(sizeof content));
  // Guest 0x1000 is granule 0's second page with file offset 0: backing the granule up to its base would
  // need file offset -0x1000, so the page is served out of line.
  CHECK(gm->MapFile(0x1000, kGuestPage, kRead, fd, 0, false, &err));
  ::close(fd);
  CHECK(gm->IsMapped(0x1000));
  CHECK(!gm->IsFastmem(0x1000));
  CHECK(gm->Host(0x1000) == nullptr);  // the fastmem query still refuses it
  const void* p = gm->HostForAccess(0x1000);
  CHECK(p != nullptr);
  CHECK_CTX(p != nullptr && static_cast<const unsigned char*>(p)[0] == 0x11, "first byte");
  CHECK_CTX(p != nullptr && static_cast<const unsigned char*>(p)[7] == 0x88, "last byte");
  CHECK(gm->HostForAccess(0x1004) == static_cast<const char*>(p) + 4);  // the intra-page offset is included
  CHECK(gm->HostForAccess(0x9000) == nullptr);                          // never mapped

  // The in-place form answers the same address from both queries.
  CHECK(gm->Map(0x4000, kGuestPage, kRead | kWrite, &err));
  CHECK(gm->IsFastmem(0x4000));
  CHECK(gm->Host(0x4000) != nullptr && gm->HostForAccess(0x4000) == gm->Host(0x4000));

  CHECK(gm->Unmap(0x1000, kGuestPage, &err));
  CHECK(gm->HostForAccess(0x1000) == nullptr);
  CHECK(gm->HostForAccess(0x1004) == nullptr);
}

// ------------------------------------------------------- refusals

void zero_length_and_permissionless_requests_are_refused() {
  auto gm = make(12);
  if (!gm) return;
  std::string err;
  CHECK(gm->Map(0x1000, 0x1000, kRead | kWrite, &err));
  // A zero-length mprotect or munmap is a kernel no-op; the first draft rounded an unaligned
  // address up through `RoundUpPages` and re-protected (or destroyed) a whole page the caller never
  // named.
  CHECK(!gm->Protect(0x1801, 0, kRead, &err));
  CHECK(!gm->Unmap(0x1801, 0, &err));
  CHECK(gm->IsMapped(0x1000));
  CHECK(gm->IsMapped(0x2000) == false);
  std::uint32_t v = 0;
  CHECK(gm->Read(0x1000, &v, sizeof v));  // the page the zero-length calls must not have touched

  // A permissionless *file* mapping maps nothing and used to report success.
  int fd = -1;
#if defined(SYS_memfd_create)
  fd = static_cast<int>(syscall(SYS_memfd_create, "rb-none", 0u));
#endif
  if (fd >= 0) {
    CHECK(!gm->MapFile(0x3000, 0x1000, kNone, fd, 0, false, &err));
    CHECK(!gm->IsMapped(0x3000));
    ::close(fd);
  }
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
  RUN(file_backed_writes_survive_a_protection_change);
  RUN(unmap_clears_both_forms);

  RUN(translate_round_trips_in_both_forms);
  RUN(writes_through_a_host_pointer_are_visible_to_reads);
  RUN(bad_requests_are_refused_not_corrupted);
  RUN(a_dirty_error_string_does_not_poison_a_later_map);
  RUN(host_page_shifts_below_the_guest_page_are_refused);
  RUN(window_size_must_be_page_multiples);
  RUN(native_host_granule_matches_injection);

  RUN(guard_pages_are_representable_and_re_armable);
  RUN(two_files_in_one_granule_read_their_own_bytes);
  RUN(shared_file_mapping_succeeds_and_stays_shared);
  RUN(a_file_in_a_granule_with_unmapped_leading_pages_reads_its_own_offset);
  RUN(host_for_access_answers_out_of_line_pages);
  RUN(zero_length_and_permissionless_requests_are_refused);

  std::printf("%d checks, %d failures, %d skipped (host page shift %u)\n", checks, failures, skipped,
              NativeShift());
  return failures == 0 ? 0 : 1;
}
