/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Guest physical/virtual memory model (component C03).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rb::mem {

/// Guest page granule is fixed at 4 KiB by AD-7: every armeabi-v7a ELF and guest libc assumption is
/// built on it, and it is not a tunable.
inline constexpr unsigned kGuestPageShift = 12;
inline constexpr std::uint32_t kGuestPage = 1u << kGuestPageShift;
inline constexpr std::size_t kDefaultWindowBytes = std::size_t{4} << 30;  // the fastmem window (U-2)

enum Prot : unsigned {
  kNone = 0,
  kRead = 1u << 0,
  kWrite = 1u << 1,
  kExec = 1u << 2,
};

struct AddressModel {
  /// kFlatIdentity: host address is exactly window_base + guest address, so guest pointers are
  /// usable by host code after one add. kBiased is the same thing with a nonzero base chosen by the
  /// OS. Both keep `guest_base` a variable — hard-coding 0 is the iOS-only bug AD-6 exists to avoid.
  enum Kind { kFlatIdentity, kBiased } kind = kFlatIdentity;

  /// Host page size, as a shift. Supplied by platform glue from getpagesize()/AT_PAGESZ.
  ///
  /// Must be **no finer** than the kernel's real page size: mmap/mprotect granularity is enforced by
  /// the kernel, so a finer value is refused by Create rather than silently mis-simulated. A coarser
  /// value is accepted, which is what lets a 4 KiB device test the 16 KiB mixed-granule path.
  unsigned host_page_shift = 12;
};

/**
 * Guest memory backed by a single reserved window, with fastmem where the host granule permits it.
 *
 * The rule that makes this non-trivial: host mappings are granular at the *host* page size, but
 * guest protection is tracked at 4 KiB. When the guest pages inside one host granule ask for
 * different protections, the granule cannot serve them all — so per AD-7 those pages **leave
 * fastmem** and are served from out-of-line backing through Read/Write, rather than the model
 * growing a software TLB. Leaving fastmem is therefore a normal, correct state, not a degraded one.
 *
 * No page-zero policy lives here. A guest `mmap` at address 0 must be refused with EPERM the way a
 * real 32-bit Android refuses it (see docs/30 §Step 3b), and that is the syscall layer's decision
 * (C07), not the memory model's.
 */
class GuestMemory {
 public:
  /// Reserves the window. Returns nullptr and fills `err` if the reservation fails — callers report
  /// it rather than crashing, because failure means the biased model is still available (U-2).
  static std::unique_ptr<GuestMemory> Create(std::size_t window_bytes, const AddressModel&,
                                             std::string* err);
  ~GuestMemory();

  GuestMemory(const GuestMemory&) = delete;
  GuestMemory& operator=(const GuestMemory&) = delete;

  /// Anonymous, zero-filled. Replaces any existing mapping over the range, like MAP_FIXED: `at` and
  /// `len` are rounded to the guest page and the previous contents of the overlap are dropped.
  bool Map(std::uint32_t at, std::size_t len, unsigned prot, std::string* err);
  /// File-backed mapping used by the guest ELF loader. `fd` is owned by the caller.
  bool MapFile(std::uint32_t at, std::size_t len, unsigned prot, int fd, std::uint64_t offset,
               bool shared, std::string* err);
  bool Protect(std::uint32_t at, std::size_t len, unsigned prot, std::string* err);
  bool Unmap(std::uint32_t at, std::size_t len, std::string* err);

  /// Directly addressable host pointer, or nullptr when the page is out-of-line and must be reached
  /// through Read/Write. Translation-unit hot paths cache this per block and re-check on fault.
  void* Host(std::uint32_t at) const;

  /// Reverse mapping for host→guest pointer crossings (AD-5/AD-6). False if the pointer is not
  /// guest memory at all — including RetroBug's own allocations, which is the property that keeps
  /// a stray host pointer from being mistaken for a guest one.
  bool Translate(const void* host, std::uint32_t* out) const;

  /// Bounds- and permission-checked copies. These are the path every out-of-line access takes, and
  /// they are also what tests assert content through, so both states are exercised identically.
  bool Read(std::uint32_t at, void* dst, std::size_t len) const;
  bool Write(std::uint32_t at, const void* src, std::size_t len);

  bool IsMapped(std::uint32_t at) const;
  bool IsFastmem(std::uint32_t at) const;
  unsigned ProtAt(std::uint32_t at) const;

  std::size_t window_bytes() const { return window_bytes_; }
  const void* window_base() const { return window_base_; }
  AddressModel::Kind model_kind() const { return kind_; }
  unsigned host_page_shift() const { return host_page_shift_; }
  std::size_t out_of_line_page_count() const { return out_of_line_.size(); }
  std::size_t mapped_page_count() const;

 private:
  struct Page {
    void* backing = nullptr;  ///< == window_base_ + guest when in place; else out-of-line block.
    unsigned prot = kNone;
    int fd = -1;              ///< File-backed mappings retain their fd for re-granulation.
    std::uint64_t file_off = 0;
    bool shared = false;
    bool file_backed = false;
  };

  /// One host-granule-sized anonymous block standing in for a single out-of-line guest page. A full
  /// granule rather than 4 KiB, because on a 16 KiB host mprotect is also 16 KiB granular: a 4 KiB
  /// block could not carry its own protection without affecting a sibling.
  struct OutOfLine {
    void* host = nullptr;
    std::size_t bytes = 0;
  };

  GuestMemory() = default;

  bool Contains(std::uint32_t at) const { return at < window_bytes_; }
  std::size_t PageIndex(std::uint32_t at) const { return at >> kGuestPageShift; }
  std::uint32_t PageAddr(std::size_t idx) const { return static_cast<std::uint32_t>(idx << kGuestPageShift); }
  std::size_t GranuleOf(std::size_t page_idx) const {
    return (page_idx << kGuestPageShift) >> host_page_shift_;
  }
  std::size_t PagesPerGranule() const { return std::size_t{1} << (host_page_shift_ - kGuestPageShift); }
  std::size_t GranuleBytes() const { return std::size_t{1} << host_page_shift_; }
  void* InPlaceHostOf(std::size_t page_idx) const {
    return static_cast<char*>(window_base_) + (page_idx << kGuestPageShift);
  }

  /// Re-establish in-place vs out-of-line form for every host granule touching [first, last).
  /// `fresh` lists pages just mapped anonymously, which must read back as zero.
  void Rebalance(std::size_t first_page, std::size_t last_page, std::string* err,
                 const std::vector<std::size_t>& fresh);
  bool RebalanceGranule(std::size_t granule, std::string* err,
                        const std::vector<std::size_t>& fresh);
  void DropPage(std::size_t page_idx);

  void* window_base_ = nullptr;   ///< Host address of guest address 0.
  std::size_t window_bytes_ = 0;
  /// The reservation as actually made, before the usable window was aligned out of it. Must be
  /// released as a whole: unmapping from the aligned base with the rounded-up size would run past
  /// the end of the reservation and could unmap unrelated host memory.
  void* raw_base_ = nullptr;
  std::size_t raw_bytes_ = 0;
  unsigned host_page_shift_ = 12;
  AddressModel::Kind kind_ = AddressModel::kFlatIdentity;
  std::vector<Page> pages_;
  std::vector<OutOfLine> out_of_line_;
  /// Descriptors duplicated from the `MapFile` arguments, closed with the instance. See `MapFile`: the
  /// caller may close its own fd as soon as the mapping exists, and a later re-granulation still needs
  /// a valid one.
  std::vector<int> owned_fds_;
};

}  // namespace rb::mem
