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
  void* raw = nullptr;
  if (model.kind == AddressModel::kFlatIdentity) {
    // Flat asks for the window low, so guest addresses stay in a range where a 32-bit truncation is
    // unambiguous. Address 0 itself is refused by the kernel (mmap_min_addr); the lowest hint that
    // worked on the x86_64 Android emulator was 0x10000 — the measurement behind docs/10 §17 U-2.
    raw = mmap(reinterpret_cast<void*>(host_page), window_bytes + host_page, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (raw == MAP_FAILED) {
      if (err) *err = SysErr("flat-model low window reservation failed", errno);
      return nullptr;
    }
  }
  if (raw == nullptr || raw == MAP_FAILED) {
    // Biased, or a flat request the kernel refused: take whatever the OS offers and never assume
    // where it landed.
    raw = mmap(nullptr, window_bytes + host_page, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) {
      if (err) *err = SysErr("window reservation failed", errno);
      return nullptr;
    }
  }

  const auto raw_addr = reinterpret_cast<std::uintptr_t>(raw);
  const auto aligned = (raw_addr + host_page - 1) & ~static_cast<std::uintptr_t>(host_page - 1);
  if (aligned + window_bytes > raw_addr + window_bytes + host_page) {
    if (err) *err = "window reservation could not be aligned to the host page";
    munmap(raw, window_bytes + host_page);
    return nullptr;
  }

  // The slack around the aligned base is simply never used: the table below bounds every access, so
  // there is nothing to trim and no way to reach it by a guest address.
  auto self = std::unique_ptr<GuestMemory>(new GuestMemory());
  self->raw_base_ = raw;
  self->raw_bytes_ = window_bytes + host_page;
  self->window_base_ = reinterpret_cast<void*>(aligned);
  self->window_bytes_ = window_bytes;
  self->host_page_shift_ = model.host_page_shift;
  self->kind_ = model.kind;
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
  if (raw_base_) munmap(raw_base_, raw_bytes_);
}

std::size_t GuestMemory::mapped_page_count() const {
  std::size_t n = 0;
  for (const auto& p : pages_) {
    if (p.prot != kNone) ++n;
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
  if (prot == kNone) {
    if (err) *err = "mapping with no permissions";
    return false;
  }

  std::vector<std::size_t> fresh;
  fresh.reserve(last - first);
  for (std::size_t i = first; i < last; ++i) {
    DropPage(i);
    pages_[i] = Page{};
    pages_[i].prot = prot;
    fresh.push_back(i);
  }
  Rebalance(first, last, err, fresh);
  if (err && !err->empty()) {
    for (std::size_t i = first; i < last; ++i) pages_[i] = Page{};
    Rebalance(first, last, nullptr, {});
    return false;
  }
  return true;
}

bool GuestMemory::MapFile(std::uint32_t at, std::size_t len, unsigned prot, int fd,
                          std::uint64_t offset, bool shared, std::string* err) {
  if (len == 0) {
    if (err) *err = "zero-length file mapping";
    return false;
  }
  const std::size_t first = at >> kGuestPageShift;
  const std::size_t last = first + RoundUpPages(at, len);
  if (last > pages_.size()) {
    if (err) *err = "file mapping exceeds the guest window";
    return false;
  }

  for (std::size_t i = first; i < last; ++i) {
    DropPage(i);
    Page& p = pages_[i];
    p = Page{};
    p.prot = prot;
    p.fd = fd;
    p.file_backed = true;
    p.shared = shared;
    p.file_off = offset + ((i - first) * kGuestPage) + (at & (kGuestPage - 1));
  }
  Rebalance(first, last, err, {});
  if (err && !err->empty()) return false;
  return true;
}

bool GuestMemory::Protect(std::uint32_t at, std::size_t len, unsigned prot, std::string* err) {
  const std::size_t first = at >> kGuestPageShift;
  const std::size_t last = first + RoundUpPages(at, len);
  if (last > pages_.size()) {
    if (err) *err = "mprotect range exceeds the guest window";
    return false;
  }
  bool any = false;
  for (std::size_t i = first; i < last; ++i) {
    if (pages_[i].prot != kNone) {
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
    mprotect(granule_host, GranuleBytes(), PROT_NONE);
    return true;
  }

  const unsigned prot = pages_[live.front()].prot;
  bool uniform = true;
  bool any_file = false;
  bool all_file = true;
  bool any_shared_file = false;
  for (std::size_t i : live) {
    const Page& p = pages_[i];
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

  if (uniform && !(any_file && !all_file)) {
    // In place: one host mapping serving the whole granule, addressed by window_base + guest.
    int fd = -1;
    std::uint64_t off = 0;
    int flags = MAP_FIXED | MAP_PRIVATE;
    if (all_file) {
      fd = pages_[live.front()].fd;
      off = pages_[live.front()].file_off;
      if (any_shared_file) flags |= MAP_SHARED;
    } else {
      flags |= MAP_ANONYMOUS;
    }

    // Snapshot anything currently held out of line, so migrating in preserves guest bytes.
    std::vector<std::pair<std::size_t, std::vector<unsigned char>>> saved;
    for (std::size_t i : live) {
      Page& p = pages_[i];
      if (p.backing && !IsFastmem(PageAddr(i))) {
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
      // Fall back to out-of-line for this granule rather than failing the whole mapping.
      if (all_file) {
        if (err) *err = SysErr("in-place file mapping failed", errno);
        return false;
      }
    } else {
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
  }

  // Out of line: each live page gets its own host-granule block, so its protection is its own.
  //
  // Order matters here, and getting it wrong faults: the bytes of a page that is still in place are
  // only readable while the granule's window mapping has permissions, so every in-place page must be
  // snapshotted *before* the granule is withdrawn.
  std::vector<std::pair<std::size_t, std::vector<unsigned char>>> carries;
  for (std::size_t i : live) {
    Page& p = pages_[i];
    if (p.backing && IsFastmem(PageAddr(i))) {
      std::vector<unsigned char> bytes(kGuestPage);
      std::memcpy(bytes.data(), p.backing, kGuestPage);
      carries.emplace_back(i, std::move(bytes));
      p.backing = nullptr;  // about to lose permission; the pointer must not be used again
    }
  }

  mprotect(granule_host, GranuleBytes(), PROT_NONE);

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
      // Private file page: pull the bytes in; writes stay private, matching MAP_PRIVATE semantics.
      void* tmp = mmap(nullptr, kGuestPage, PROT_READ, MAP_PRIVATE, p.fd, static_cast<off_t>(p.file_off));
      if (tmp != MAP_FAILED) {
        std::memcpy(block.host, tmp, kGuestPage);
        munmap(tmp, kGuestPage);
      }
    }  // else: freshly claimed anonymous page, already zeroed by the kernel
    mprotect(block.host, block.bytes, ToHostProt(p.prot));
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
    if (addr >= b && addr - b < block.bytes) {
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
  const Page& p = pages_[PageIndex(at)];
  if (!p.backing || !(p.prot & kWrite)) return false;
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
  return Contains(at) && pages_[PageIndex(at)].prot != kNone && pages_[PageIndex(at)].backing != nullptr;
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

}  // namespace rb::mem
