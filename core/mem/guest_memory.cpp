/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 */
#include "rb/mem/guest_memory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

namespace rb::mem {
namespace {

unsigned ToHostProt(unsigned prot) {
  unsigned p = 0;
  if (prot & kRead) p |= PROT_READ;
  if (prot & kWrite) p |= PROT_WRITE;
  if (prot & kExec) p |= PROT_EXEC;
  return p ? p : PROT_NONE;
}

std::string SysErr(const char* what, int e) {
  char buf[192];
  std::snprintf(buf, sizeof buf, "%s: %s (errno %d)", what, std::strerror(e), e);
  return buf;
}

std::size_t RoundUpPages(std::uint32_t at, std::size_t len) {
  return (static_cast<std::size_t>(at & (kGuestPage - 1)) + len + kGuestPage - 1) >> kGuestPageShift;
}

}  // namespace

std::unique_ptr<GuestMemory> GuestMemory::Create(std::size_t window_bytes, const AddressModel& model,
                                                 std::string* err) {
  if (window_bytes == 0 || (window_bytes % kGuestPage) != 0) {
    if (err) *err = "window size must be a non-zero multiple of the 4 KiB guest page";
    return nullptr;
  }
  if (model.host_page_shift < kGuestPageShift) {
    if (err) *err = "host page size is smaller than the fixed 4 KiB guest page";
    return nullptr;
  }

  // The kernel, not the model, decides mmap/mprotect granularity: a request for a finer granule than
  // the real page size is rejected with EINVAL rather than honoured. So the injected shift is a
  // grouping policy that may only ever be *coarser* than reality — which is exactly what makes the
  // 16 KiB mixed-granule behaviour testable on a 4 KiB device, and why refusing rather than silently
  // clamping is the right answer here.
  const long real_ps = sysconf(_SC_PAGESIZE);
  unsigned real_shift = kGuestPageShift;
  if (real_ps > 0) {
    while ((static_cast<long>(1) << real_shift) < real_ps) ++real_shift;
  }
  if (model.host_page_shift < real_shift) {
    char buf[192];
    std::snprintf(buf, sizeof buf,
                  "host_page_shift %u is below this kernel's page size (shift %u): mmap/mprotect "
                  "granularity is enforced by the kernel and cannot be simulated at a finer grain",
                  model.host_page_shift, real_shift);
    if (err) *err = buf;
    return nullptr;
  }

  const std::size_t host_page = std::size_t{1} << model.host_page_shift;
  // Two host pages of slack, not one: the granule-sized mprotect/mmap calls in `RebalanceGranule`
  // cover `GranuleBytes()` from the granule base, and the last granule of a window that is not a
  // multiple of the host granule runs past `window_bytes` by up to a granule. With one page of slack
  // that ran past the reservation whenever the aligned base was not already granule-aligned —
  // mprotecting (or worse, MAP_FIXED-mapping) host memory this object does not own, and the
  // destructor never releasing it. Two pages cover aligned-base shift plus granule overrun.
  const std::size_t slack = 2 * host_page;
  void* raw = nullptr;
  if (model.kind == AddressModel::kFlatIdentity) {
    // Flat asks for the window low, so guest addresses stay in a range where a 32-bit truncation is
    // unambiguous. Address 0 itself is refused by the kernel (mmap_min_addr); the lowest hint that
    // worked on the x86_64 Android emulator was 0x10000 — the measurement behind docs/10 §17 U-2.
    raw = mmap(reinterpret_cast<void*>(host_page), window_bytes + slack, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (raw == MAP_FAILED) {
      if (err) *err = SysErr("flat-model low window reservation failed", errno);
      return nullptr;
    }
  }
  if (raw == nullptr || raw == MAP_FAILED) {
    // Biased, or a flat request the kernel refused: take whatever the OS offers and never assume
    // where it landed.
    raw = mmap(nullptr, window_bytes + slack, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) {
      if (err) *err = SysErr("window reservation failed", errno);
      return nullptr;
    }
  }

  const auto raw_addr = reinterpret_cast<std::uintptr_t>(raw);
  const auto aligned = (raw_addr + host_page - 1) & ~static_cast<std::uintptr_t>(host_page - 1);
  if (aligned + window_bytes > raw_addr + window_bytes + slack) {
    if (err) *err = "window reservation could not be aligned to the host page";
    munmap(raw, window_bytes + slack);
    return nullptr;
  }

  // The slack around the aligned base is simply never used: the table below bounds every access, so
  // there is nothing to trim and no way to reach it by a guest address.
  auto self = std::unique_ptr<GuestMemory>(new GuestMemory());
  self->raw_base_ = raw;
  self->raw_bytes_ = window_bytes + slack;
  self->window_base_ = reinterpret_cast<void*>(aligned);
  self->window_bytes_ = window_bytes;
  self->host_page_shift_ = model.host_page_shift;
  self->kind_ = model.kind;
  // The kernel's real page size, kept for the mmap-offset alignment rule: an mmap offset must be
  // aligned to *this*, which is finer than the model granule exactly on the 16 KiB hosts this model
  // exists for.
  self->real_page_shift_ = real_shift;
  // One entry per guest page. At a full 4 GiB window this table is ~32 MiB, which is worth
  // chunking before shipping; see the note in docs/30. Not premature here: nothing maps more than
  // a module yet, and a chunked table would make the mixed-granule logic harder to read.
  self->pages_.resize(window_bytes / kGuestPage);
  return self;
}

GuestMemory::~GuestMemory() {
  for (const auto& block : out_of_line_) {
    munmap(block.host, block.bytes);
  }
  // The mappings go away with the address space first, so closing these last only matters for the
  // next `GuestMemory` in the same process — which the tests do build, one per case.
  for (const int fd : owned_fds_) {
    close(fd);
  }
  if (raw_base_) munmap(raw_base_, raw_bytes_);
}

std::size_t GuestMemory::mapped_page_count() const {
  std::size_t n = 0;
  for (const auto& p : pages_) {
    if (p.mapped) ++n;
  }
  return n;
}

void GuestMemory::DropPage(std::size_t page_idx) {
  Page& p = pages_[page_idx];
  if (!p.backing) return;
  if (IsFastmem(PageAddr(page_idx))) return;  // in the window: nothing to release
  const void* want = p.backing;
  out_of_line_.erase(std::remove_if(out_of_line_.begin(), out_of_line_.end(),
                                    [&](const OutOfLine& b) {
                                      if (b.host != want) return false;
                                      munmap(b.host, b.bytes);
                                      return true;
                                    }),
                     out_of_line_.end());
}

bool GuestMemory::Map(std::uint32_t at, std::size_t len, unsigned prot, std::string* err) {
  struct NotifyOnExit {  // every exit, a failure included: a partial change still moved pages
    const GuestMemory& m; std::uint32_t a; std::size_t n;
    ~NotifyOnExit() { m.Notify(a, n); }
  } notify_on_exit{*this, at, len};
  // Success is reported by leaving `err` empty, so a string that arrives already carrying last call's
  // text must not be read as this call's failure. It was: a caller reusing one `std::string` across a
  // sequence of maps had a *successful* map roll itself back and return false, and the launcher is exactly
  // such a caller. Cleared here rather than left as an unwritten precondition on every call site.
  if (err != nullptr) err->clear();
  if (len == 0) {
    if (err) *err = "zero-length mapping";
    return false;
  }
  const std::size_t first = at >> kGuestPageShift;
  const std::size_t last = first + RoundUpPages(at, len);
  if (last > pages_.size()) {
    if (err) *err = "mapping exceeds the guest window";
    return false;
  }

  std::vector<std::size_t> fresh;
  fresh.reserve(last - first);
  for (std::size_t i = first; i < last; ++i) {
    DropPage(i);
    pages_[i] = Page{};
    pages_[i].prot = prot;
    // A `kNone` map is a guard reservation: mapped (the loader's placement scan and `Protect` see
    // it), with no permissions and no backing, so every access refuses. The first draft refused
    // `kNone` here outright, which made a guard page unrepresentable — and a range that can be
    // reserved is a range a guest can be kept out of, which the launcher needs for its host-entry
    // window.
    pages_[i].mapped = true;
    fresh.push_back(i);
  }
  Rebalance(first, last, err, fresh);
  if (err && !err->empty()) {
    // `DropPage` first, then clear: dropping is what munmaps an out-of-line block, and it finds the block
    // through `Page::backing`. Zeroing the `Page` first threw that pointer away, so the block was never
    // unmapped — the rejection path leaked the mapping until some later overwrite happened to reuse the
    // address, which is the shape "refused, not corrupted" is supposed to rule out.
    for (std::size_t i = first; i < last; ++i) DropPage(i);
    for (std::size_t i = first; i < last; ++i) pages_[i] = Page{};
    Rebalance(first, last, nullptr, {});
    return false;
  }
  return true;
}

bool GuestMemory::MapFile(std::uint32_t at, std::size_t len, unsigned prot, int fd,
                          std::uint64_t offset, bool shared, std::string* err) {
  struct NotifyOnExit {  // every exit, a failure included: a partial change still moved pages
    const GuestMemory& m; std::uint32_t a; std::size_t n;
    ~NotifyOnExit() { m.Notify(a, n); }
  } notify_on_exit{*this, at, len};
  // Same precondition as `Map`: the caller's error string is an output, not an input.
  if (err != nullptr) err->clear();
  if (len == 0) {
    if (err) *err = "zero-length file mapping";
    return false;
  }
  if (prot == kNone) {
    // Refused rather than silently mapped-as-nothing, as a first draft did: a file mapping with no
    // permissions maps nothing and reports success, and the caller's bytes never reach memory.
    if (err) *err = "file mapping with no permissions";
    return false;
  }
  const std::size_t first = at >> kGuestPageShift;
  const std::size_t last = first + RoundUpPages(at, len);
  if (last > pages_.size()) {
    if (err) *err = "file mapping exceeds the guest window";
    return false;
  }

  // The caller's fd is duplicated and owned from here on. Keeping the number it handed over is a
  // latent dangling descriptor: `Page::fd` is used again by every later re-granulation (a protection
  // change, a `Protect` on one page of a loaded module), and a loader that closes its fd once the
  // mapping exists is doing nothing wrong — the mapping has its own reference.
  //
  // Found that way, on the tier-diff test's `Protect` of a module's text page: `Bad file descriptor
  // (errno 9)`, a symptom as far from its cause as this API can arrange. `guest_memory_test.cpp`'s
  // file-backed case now closes the caller's fd for the same reason and fails 12 checks if this line
  // ever goes back to `= fd`, because the failed re-map drops the pages it was rebalancing and every
  // read after it is a fault on unmapped memory.
  const int owned = ::dup(fd);
  if (owned < 0) {
    if (err) *err = SysErr("dup() of the file descriptor failed", errno);
    return false;
  }
  owned_fds_.push_back(owned);

  for (std::size_t i = first; i < last; ++i) {
    DropPage(i);
    Page& p = pages_[i];
    p = Page{};
    p.mapped = true;
    p.prot = prot;
    p.fd = owned;
    p.file_backed = true;
    p.shared = shared;
    p.file_off = offset + ((i - first) * kGuestPage) + (at & (kGuestPage - 1));
  }
  Rebalance(first, last, err, {});
  if (err && !err->empty()) {
    // The pages this call claimed go back the same way `Map`'s do, and the duplicated descriptor goes with
    // them: it was taken before the mapping existed and no page references it after the loop below. Leaving
    // it in `owned_fds_` would keep a reference to a file this call never managed to map.
    for (std::size_t i = first; i < last; ++i) DropPage(i);
    for (std::size_t i = first; i < last; ++i) pages_[i] = Page{};
    Rebalance(first, last, nullptr, {});
    ::close(owned);
    owned_fds_.pop_back();
    return false;
  }
  return true;
}

bool GuestMemory::Protect(std::uint32_t at, std::size_t len, unsigned prot, std::string* err) {
  struct NotifyOnExit {  // every exit, a failure included: a partial change still moved pages
    const GuestMemory& m; std::uint32_t a; std::size_t n;
    ~NotifyOnExit() { m.Notify(a, n); }
  } notify_on_exit{*this, at, len};
  if (len == 0) {
    // A zero-length mprotect is a kernel no-op; taking the unaligned-address path a first draft took
    // re-protected (or unmapped) a whole page the caller never named.
    if (err) *err = "zero-length mprotect";
    return false;
  }
  const std::size_t first = at >> kGuestPageShift;
  const std::size_t last = first + RoundUpPages(at, len);
  if (last > pages_.size()) {
    if (err) *err = "mprotect range exceeds the guest window";
    return false;
  }
  bool any = false;
  for (std::size_t i = first; i < last; ++i) {
    if (pages_[i].mapped) {
      // Gated on `mapped`, not on a live protection: a guard page has to be re-protectable, or the
      // reservation is a one-way trap and the re-protection the caller asks for reports "unmapped".
      pages_[i].prot = prot;
      any = true;
    }
  }
  if (!any) {
    if (err) *err = "mprotect on an unmapped range";
    return false;
  }
  Rebalance(first, last, err, {});
  return true;
}

bool GuestMemory::Unmap(std::uint32_t at, std::size_t len, std::string* err) {
  struct NotifyOnExit {  // every exit, a failure included: a partial change still moved pages
    const GuestMemory& m; std::uint32_t a; std::size_t n;
    ~NotifyOnExit() { m.Notify(a, n); }
  } notify_on_exit{*this, at, len};
  if (len == 0) {
    if (err) *err = "zero-length munmap";
    return false;
  }
  const std::size_t first = at >> kGuestPageShift;
  const std::size_t last = first + RoundUpPages(at, len);
  if (last > pages_.size()) {
    if (err) *err = "munmap range exceeds the guest window";
    return false;
  }
  for (std::size_t i = first; i < last; ++i) {
    DropPage(i);
    pages_[i] = Page{};
  }
  // No holes are ever punched in the window: an unmapped granule is left mapped PROT_NONE, which
  // keeps the table-to-host-address relation stable and still faults on any guest touch.
  Rebalance(first, last, err, {});
  return true;
}

void GuestMemory::Rebalance(std::size_t first_page, std::size_t last_page, std::string* err,
                           const std::vector<std::size_t>& fresh) {
  if (first_page >= last_page) return;
  std::set<std::size_t> granules;
  for (std::size_t i = first_page; i < last_page; ++i) granules.insert(GranuleOf(i));
  for (std::size_t g : granules) {
    if (!RebalanceGranule(g, err, fresh)) return;
  }
}

bool GuestMemory::RebalanceGranule(std::size_t granule, std::string* err,
                                   const std::vector<std::size_t>& fresh) {
  const std::size_t base = granule << (host_page_shift_ - kGuestPageShift);
  const std::size_t end = std::min(base + PagesPerGranule(), pages_.size());
  void* granule_host = static_cast<char*>(window_base_) + (base << kGuestPageShift);

  std::vector<std::size_t> live;
  for (std::size_t i = base; i < end; ++i) {
    if (pages_[i].prot != kNone) live.push_back(i);
  }
  if (live.empty()) {
    if (mprotect(granule_host, GranuleBytes(), PROT_NONE) != 0) {
      if (err) *err = SysErr("withdrawing an empty granule failed", errno);
      return false;
    }
    return true;
  }

  const unsigned prot = pages_[live.front()].prot;
  bool uniform = true;
  bool any_file = false;
  bool all_file = true;
  bool any_shared_file = false;
  bool any_guard = false;
  for (std::size_t i = base; i < end; ++i) {
    const Page& p = pages_[i];
    if (p.mapped && p.prot == kNone) any_guard = true;
    if (!p.mapped || p.prot == kNone) continue;
    if (p.prot != prot) uniform = false;
    if (p.file_backed) any_file = true; else all_file = false;
    if (p.file_backed && p.shared) any_shared_file = true;
  }

  // A shared file mapping whose granule has to leave the window cannot work: writes would have to
  // propagate to the file through a private copy. Refusing beats silently losing them, and the
  // loader never produces this shape for an ELF segment.
  if (!uniform && any_shared_file) {
    if (err) *err = "shared file mapping cannot leave fastmem in a mixed-protection host granule";
    return false;
  }

  // The in-place form needs every live page to be the same page of the same file at consecutive
  // offsets. The first draft took the *first* live page's fd and offset for the whole granule: two
  // libraries loaded into one host granule both read back the first one's bytes, and for a
  // MAP_SHARED granule the post-remap snapshot restore wrote the second library's contents through
  // the first one's file. Contiguity is also what makes the single (fd, offset) pair correct at all.
  bool contiguous = true;
  if (all_file) {
    const Page& front = pages_[live.front()];
    for (std::size_t k = 1; k < live.size(); ++k) {
      const Page& p = pages_[live[k]];
      // The distance is the *guest* distance, not the live-list index: a granule with a hole (pages 0
      // and 2 live, 1 unmapped) has k = 1 for page 2, and comparing against `front.file_off + 1 page`
      // accepted a set the in-place mmap then serves from the wrong file offsets. A hole means no
      // single (fd, offset) pair describes every live page, so the granule must leave the window.
      const std::size_t span = (live[k] - live.front()) * static_cast<std::size_t>(kGuestPage);
      if (p.fd != front.fd || p.shared != front.shared ||
          p.file_off != front.file_off + span) {
        contiguous = false;
        break;
      }
    }
  }
  // mmap offsets are aligned to the *real* page size, not the model granule: a 4 KiB ELF segment
  // offset on a 16 KiB host cannot serve its granule in place at all. The offset this checks is the
  // one the mmap below will really use — `granule_off`, backed up from the first *live* page to the
  // granule base — not `front.file_off` itself: the mmap starts at the granule base, so a granule whose
  // leading pages are unmapped needs the file offset to back up by exactly those pages. Using the
  // front page's offset at the base shifted every live page's bytes by that gap — invisible to every
  // test that maps at 0x0, and live on a 16 KiB host for the very first image, which is placed at
  // 0x2000 (granule 0 with pages 0-1 unmapped). A backing-up that would go negative (the file's bytes
  // do not reach back to the base) or that leaves the real-page alignment falls out of line instead.
  const std::size_t real_page = std::size_t{1} << real_page_shift_;
  const std::uint64_t back = (live.front() - base) * static_cast<std::uint64_t>(kGuestPage);
  const bool off_aligned =
      !all_file || (pages_[live.front()].file_off >= back &&
                    ((pages_[live.front()].file_off - back) & (real_page - 1)) == 0);
  // A guard page shares the granule's host mapping while it is in place, so remapping the granule
  // with the survivors' protection would hand the guard page live host permissions behind its
  // `kNone` table entry. Guard-bearing granules go out of line, where the window stays PROT_NONE.
  if (uniform && !any_guard && !(any_file && !all_file) && contiguous && off_aligned) {
    // In place: one host mapping serving the whole granule, addressed by window_base + guest.
    int fd = -1;
    std::uint64_t off = 0;
    int flags = MAP_FIXED | (any_shared_file ? MAP_SHARED : MAP_PRIVATE);
    if (all_file) {
      fd = pages_[live.front()].fd;
      // `granule_off`, not the front page's offset: the mmap targets `granule_host` (the granule base),
      // and `off_aligned` above already established that backing up by the leading unmapped pages is
      // both non-negative and real-page aligned. See its comment for the bug this closes.
      off = pages_[live.front()].file_off - back;
    } else {
      flags |= MAP_ANONYMOUS;
    }

    // Snapshot everything currently held in this granule, so migrating preserves guest bytes.
    //
    // This has to include pages that are *already* in place, not just out-of-line ones: the mmap
    // below is MAP_FIXED over the top of them, and re-mapping a private file page from its fd resets
    // it to the file's contents. That is not a theoretical cost — it is what a loader does. Applying
    // relocations writes the GOT and .data.rel.ro while the segment is mapped writable, and the very
    // next step is the protection pass that drops write; without the snapshot every relocated word
    // silently reverts to its pre-relocation file value and the image runs with a GOT full of linker
    // stub addresses. A kernel's mprotect never re-reads the file, and neither may this.
    std::vector<std::pair<std::size_t, std::vector<unsigned char>>> saved;
    for (std::size_t i : live) {
      Page& p = pages_[i];
      if (p.backing) {
        std::vector<unsigned char> bytes(kGuestPage);
        std::memcpy(bytes.data(), p.backing, kGuestPage);
        saved.emplace_back(i, std::move(bytes));
        DropPage(i);
        p.backing = nullptr;
      }
    }

    // Map writable first, then apply the final protection: fresh anonymous pages have to be zeroed
    // by us (no remap commits them), and a guest that maps RX straight away would otherwise fault
    // on the very memset that is supposed to clear the page.
    void* got = mmap(granule_host, GranuleBytes(), ToHostProt(prot) | PROT_WRITE, flags, fd,
                     static_cast<off_t>(off));
    if (got == MAP_FAILED) {
      // Fall back to out-of-line for this granule rather than failing the mapping — and carry the
      // snapshot, which at this moment holds every live page's bytes (the snapshot above cleared
      // all the backings). Falling through with empty carriers is what silently zeroed the granule.
      std::vector<std::pair<std::size_t, std::vector<unsigned char>>> carries{std::move(saved)};
      saved.clear();
      return RebalanceGranuleOutOfLine(granule, err, std::move(carries));
    }
    for (auto& [idx, bytes] : saved) {
      std::memcpy(static_cast<char*>(window_base_) + (idx << kGuestPageShift), bytes.data(),
                  kGuestPage);
    }
    // Fresh anonymous pages must read back as zero, as a real mmap would. Done after the remap so
    // it cannot be undone by it, and only for pages the caller actually just mapped.
    if (!any_file) {
      for (std::size_t idx : fresh) {
        if (idx >= base && idx < end) {
          std::memset(static_cast<char*>(window_base_) + (idx << kGuestPageShift), 0, kGuestPage);
        }
      }
    }
    if (mprotect(granule_host, GranuleBytes(), ToHostProt(prot)) != 0) {
      if (err) *err = SysErr("applying in-place granule protection failed", errno);
      return false;
    }
    for (std::size_t i : live) {
      pages_[i].backing = InPlaceHostOf(i);
    }
    return true;
  }

  // A shared file granule that cannot serve in place has no honest home: out-of-line writes would
  // not reach the file. Refusing names the reason; the loader (the only shared-file caller) can
  // place the mapping differently.
  if (any_shared_file) {
    if (err) *err = all_file && !off_aligned
                        ? "shared file mapping's offset is below the kernel's page alignment"
                        : "shared file mapping's pages are not contiguous in one file";
    return false;
  }

  return RebalanceGranuleOutOfLine(granule, err, {});
}

/// The out-of-line form: each live page gets its own host-granule block, so its protection is its
/// own. `carries` holds snapshots of pages that already left the window (the in-place attempt's
/// fall-through); pages still in place are snapshotted here, before the granule is withdrawn, because
/// their bytes are only readable while the granule's window mapping has permissions.
bool GuestMemory::RebalanceGranuleOutOfLine(std::size_t granule, std::string* err,
                                            std::vector<std::pair<std::size_t, std::vector<unsigned char>>>&& carries) {
  const std::size_t base = granule << (host_page_shift_ - kGuestPageShift);
  const std::size_t end = std::min(base + PagesPerGranule(), pages_.size());
  void* granule_host = static_cast<char*>(window_base_) + (base << kGuestPageShift);

  std::vector<std::size_t> live;
  for (std::size_t i = base; i < end; ++i) {
    if (pages_[i].prot != kNone) live.push_back(i);
  }

  for (std::size_t i : live) {
    Page& p = pages_[i];
    if (p.backing && IsFastmem(PageAddr(i))) {
      std::vector<unsigned char> bytes(kGuestPage);
      std::memcpy(bytes.data(), p.backing, kGuestPage);
      carries.emplace_back(i, std::move(bytes));
      p.backing = nullptr;  // about to lose permission; the pointer must not be used again
    }
  }

  if (mprotect(granule_host, GranuleBytes(), PROT_NONE) != 0) {
    // Unchecked once, and the failure mode was exactly the one the comment below promises cannot
    // happen: the table said the granule was withdrawn while the host pages kept live permissions.
    if (err) *err = SysErr("withdrawing the window granule failed", errno);
    return false;
  }

  for (std::size_t i : live) {
    Page& p = pages_[i];
    if (p.backing) {
      // Already out of line: unaffected by the granule above, just re-permission its block.
      for (auto& block : out_of_line_) {
        if (block.host == p.backing) {
          mprotect(block.host, block.bytes, ToHostProt(p.prot));
          break;
        }
      }
      continue;
    }

    OutOfLine block;
    block.bytes = GranuleBytes();
    block.host = mmap(nullptr, block.bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block.host == MAP_FAILED) {
      if (err) *err = SysErr("out-of-line backing allocation failed", errno);
      return false;
    }
    auto carried = std::find_if(carries.begin(), carries.end(),
                                [&](const auto& c) { return c.first == i; });
    if (carried != carries.end()) {
      std::memcpy(block.host, carried->second.data(), kGuestPage);  // migrated out of place
    } else if (p.file_backed) {
      // Private file page: pull the bytes in with pread; writes stay private, matching MAP_PRIVATE
      // semantics. pread rather than a throwaway mmap because an mmap offset must be aligned to the
      // kernel's real page size — a 4 KiB ELF segment on a 16 KiB host is not — and the mmap this
      // replaced failed EINVAL and was *ignored*, so the page silently read back as zeros while the
      // load reported success. A short read past EOF leaves the tail zeroed, which is what a real
      // kernel's mapping of a partial last page reads as.
      std::size_t got = 0;
      bool ok = true;
      while (got < kGuestPage) {
        const ssize_t n = pread(p.fd, static_cast<char*>(block.host) + got, kGuestPage - got,
                                static_cast<off_t>(p.file_off) + static_cast<off_t>(got));
        if (n < 0) {
          if (errno == EINTR) continue;
          if (err) *err = SysErr("reading a file-backed page out of line failed", errno);
          ok = false;
          break;
        }
        if (n == 0) break;  // past EOF: the rest of the page reads as zeros
        got += static_cast<std::size_t>(n);
      }
      if (!ok) {
        munmap(block.host, block.bytes);
        return false;
      }
    }  // else: freshly claimed anonymous page, already zeroed by the kernel
    if (mprotect(block.host, block.bytes, ToHostProt(p.prot)) != 0) {
      if (err) *err = SysErr("protecting an out-of-line block failed", errno);
      munmap(block.host, block.bytes);
      return false;
    }
    out_of_line_.push_back(block);
    p.backing = block.host;
  }
  return true;
}


void* GuestMemory::Host(std::uint32_t at) const {
  if (!Contains(at)) return nullptr;
  const std::size_t idx = PageIndex(at);
  const Page& p = pages_[idx];
  if (!p.backing || !IsFastmem(at)) return nullptr;
  // Intra-page offset included: the address of `at`, not of its page, so a caller can use it the way
  // it would use a pointer returned by mmap.
  return static_cast<char*>(p.backing) + (at & (kGuestPage - 1));
}

bool GuestMemory::Translate(const void* host, std::uint32_t* out) const {
  const auto addr = reinterpret_cast<std::uintptr_t>(host);
  const auto base = reinterpret_cast<std::uintptr_t>(window_base_);
  if (addr >= base && addr - base < window_bytes_) {
    const auto guest = static_cast<std::uint32_t>(addr - base);
    if (!IsMapped(guest)) return false;
    if (out) *out = guest;
    return true;
  }
  for (const auto& block : out_of_line_) {
    const auto b = reinterpret_cast<std::uintptr_t>(block.host);
    if (addr >= b && addr - b < kGuestPage) {
      // Bounded by the guest page the block serves, not by `block.bytes`: a block is one host
      // granule holding one 4 KiB guest page, and a pointer past the page in the unused granule
      // space translated to a *different* guest page's address — a wrong answer rather than a
      // refusal, from a stale or foreign pointer.
      for (std::size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].backing == block.host) {
          if (out) *out = static_cast<std::uint32_t>((i << kGuestPageShift) + (addr - b));
          return true;
        }
      }
      return false;  // a released block: not guest-addressable any more
    }
  }
  return false;  // RetroBug's own memory, deliberately indistinguishable from guest memory
}

bool GuestMemory::Read(std::uint32_t at, void* dst, std::size_t len) const {
  if (len == 0 || !Contains(at) || static_cast<std::size_t>(at) + len > window_bytes_) return false;
  const std::size_t idx = PageIndex(at);
  const Page& p = pages_[idx];
  if (!p.backing || !(p.prot & kRead)) return false;
  const std::size_t within = at & (kGuestPage - 1);
  if (within + len > kGuestPage) {
    std::size_t done = 0;
    while (done < len) {
      const std::uint32_t cur = at + static_cast<std::uint32_t>(done);
      const std::size_t take =
          std::min<std::size_t>(kGuestPage - (cur & (kGuestPage - 1)), len - done);
      if (!Read(cur, static_cast<unsigned char*>(dst) + done, take)) return false;
      done += take;
    }
    return true;
  }
  std::memcpy(dst, static_cast<unsigned char*>(p.backing) + within, len);
  return true;
}

bool GuestMemory::Write(std::uint32_t at, const void* src, std::size_t len) {
  if (len == 0 || !Contains(at) || static_cast<std::size_t>(at) + len > window_bytes_) return false;
  // Verified before anything is copied. The recursive form a first draft used wrote page by page and
  // returned false on the first bad one, so a write straddling a writable page and an unmapped (or
  // read-only) one left the earlier pages holding new bytes while reporting failure — and a caller
  // like the monitor's `read` then answered EFAULT with the guest buffer half-filled, retrying into
  // the same partial state forever. Every page is checked first, so a refusal writes nothing.
  for (std::size_t done = 0; done < len;) {
    const std::uint32_t cur = at + static_cast<std::uint32_t>(done);
    const Page& p = pages_[PageIndex(cur)];
    if (!p.backing || !(p.prot & kWrite)) return false;
    done += std::min<std::size_t>(kGuestPage - (cur & (kGuestPage - 1)), len - done);
  }
  const Page& p = pages_[PageIndex(at)];
  const std::size_t within = at & (kGuestPage - 1);
  if (within + len > kGuestPage) {
    std::size_t done = 0;
    while (done < len) {
      const std::uint32_t cur = at + static_cast<std::uint32_t>(done);
      const std::size_t take =
          std::min<std::size_t>(kGuestPage - (cur & (kGuestPage - 1)), len - done);
      if (!Write(cur, static_cast<const unsigned char*>(src) + done, take)) return false;
      done += take;
    }
    return true;
  }
  std::memcpy(static_cast<unsigned char*>(p.backing) + within, src, len);
  return true;
}

bool GuestMemory::IsMapped(std::uint32_t at) const {
  // `mapped`, not the protection: a guard page is mapped with no permissions, and callers that ask
  // "is this guest page claimed" (the loader's placement scan, pointer translation) need to see it.
  return Contains(at) && pages_[PageIndex(at)].mapped;
}

bool GuestMemory::IsFastmem(std::uint32_t at) const {
  if (!Contains(at)) return false;
  const std::size_t idx = PageIndex(at);
  const Page& p = pages_[idx];
  return p.backing && p.backing == InPlaceHostOf(idx);
}

unsigned GuestMemory::ProtAt(std::uint32_t at) const {
  return Contains(at) ? pages_[PageIndex(at)].prot : kNone;
}

void GuestMemory::Audit(diag::Auditor& audit) const {
  std::size_t live = 0, guards = 0, mapped = 0;
  std::size_t missing_backing = 0, stray_backing = 0, bad_fd = 0, out_refs = 0;
  const auto* window = static_cast<const std::uint8_t*>(window_base_);
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    const Page& p = pages_[i];
    if (!p.mapped) {
      // An unmapped page holding a block is host memory nothing will ever release.
      if (p.backing != nullptr) ++stray_backing;
      continue;
    }
    ++mapped;
    if (p.prot == kNone) {
      // A guard/reservation is mapped with no backing by design — a backing here would mean a module
      // could have been placed on a page the layout believes reserved.
      ++guards;
      if (p.backing != nullptr) ++stray_backing;
      continue;
    }
    ++live;
    if (p.backing == nullptr) {
      ++missing_backing;
      continue;
    }
    if (p.backing == static_cast<const void*>(window + (i << kGuestPageShift))) {
      // In place: the fastmem window itself.
    } else {
      bool referenced = false;
      for (const OutOfLine& b : out_of_line_) {
        if (p.backing == b.host) {
          referenced = true;
          ++out_refs;
          break;
        }
      }
      if (!referenced) ++stray_backing;
    }
    if (p.file_backed && p.fd < 0) ++bad_fd;
  }

  audit.Check("mem/page-table", missing_backing == 0 && stray_backing == 0 && bad_fd == 0,
              "missing_backing=" + std::to_string(missing_backing) + " stray_backing=" +
                  std::to_string(stray_backing) + " file_pages_without_fd=" + std::to_string(bad_fd));
  audit.Check("mem/out-of-line-blocks", out_refs == out_of_line_.size(),
              std::to_string(out_of_line_.size()) + " block(s) held, " + std::to_string(out_refs) +
                  " referenced — the rest is host memory no page will release");
  audit.Check("mem/page-count", mapped == mapped_page_count(),
              "walk counted " + std::to_string(mapped) + ", accessor says " +
                  std::to_string(mapped_page_count()));
  audit.Info("mem/pages", std::to_string(live) + " live, " + std::to_string(guards) +
                             " reservation(s), window " + std::to_string(window_bytes_) + " bytes");
}

}  // namespace rb::mem

namespace rb::mem {

int GuestMemory::AddObserver(Observer fn) {
  const int id = next_observer_++;
  observers_.emplace(id, std::move(fn));
  return id;
}

void GuestMemory::RemoveObserver(int id) { observers_.erase(id); }

void GuestMemory::Notify(std::uint32_t at, std::size_t len) const {
  if (observers_.empty() || len == 0) return;
  const std::uint64_t g = GranuleBytes();
  const std::uint64_t from = (static_cast<std::uint64_t>(at) / g) * g;
  std::uint64_t to = ((static_cast<std::uint64_t>(at) + len + g - 1) / g) * g;
  if (to > window_bytes_) to = window_bytes_;
  if (to <= from) return;
  for (const auto& [id, fn] : observers_) fn(static_cast<std::uint32_t>(from), static_cast<std::size_t>(to - from));
}

}  // namespace rb::mem
