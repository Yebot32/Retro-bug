/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 */
#include "rb/loader/guest_elf.h"
#include "rb/loader/elf_defs.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rb::loader {
namespace {

// The ELF32/AArch32 wire structures and constants live in rb/loader/elf_defs.h, shared with the
// loader tests so they can read the same tables the loader reads rather than trusting hard-coded
// offsets that shift whenever the fixture's source changes.
namespace elf = rb::loader::elf;
using elf::Ehdr32; using elf::Phdr32; using elf::Shdr32; using elf::Dyn32;
using elf::Rel32; using elf::Rela32; using elf::Sym32;
using elf::ET_DYN; using elf::EM_ARM; using elf::EF_ARM_MASK;
using elf::PT_LOAD; using elf::PT_DYNAMIC; using elf::PT_NOTE;
using elf::PT_GNU_RELRO; using elf::PT_ARM_EXIDX; using elf::PT_TLS;
using elf::PF_X; using elf::PF_W; using elf::PF_R;
using elf::SHN_UNDEF; using elf::SHN_ABS;
using elf::SHT_DYNSYM; using elf::SHT_SYMTAB;
using elf::STT_OBJECT; using elf::STT_FUNC; using elf::STT_GNU_IFUNC;
using elf::STB_GLOBAL; using elf::STB_WEAK;
using elf::ELF32_ST_BIND; using elf::ELF32_ST_TYPE; using elf::ELF32_R_SYM; using elf::ELF32_R_TYPE;
using elf::R_ARM_NONE; using elf::R_ARM_ABS32; using elf::R_ARM_COPY;
using elf::R_ARM_GLOB_DAT; using elf::R_ARM_JUMP_SLOT; using elf::R_ARM_RELATIVE;
using elf::R_ARM_IRELATIVE; using elf::R_ARM_TLS_DTPMOD32; using elf::R_ARM_TLS_DTPOFF32;
using elf::R_ARM_TLS_TPOFF32;
using elf::DT_NULL; using elf::DT_NEEDED; using elf::DT_PLTGOT; using elf::DT_PLTRELSZ;
using elf::DT_STRTAB; using elf::DT_STRSZ; using elf::DT_SYMTAB; using elf::DT_SYMENT;
using elf::DT_INIT; using elf::DT_FINI; using elf::DT_SONAME;
using elf::DT_INIT_ARRAY; using elf::DT_INIT_ARRAYSZ;
using elf::DT_REL; using elf::DT_RELSZ; using elf::DT_RELENT;
using elf::DT_RELA; using elf::DT_RELASZ; using elf::DT_RELAENT; using elf::DT_JMPREL;
using elf::DT_PLTREL;
using elf::DT_RELACOUNT; using elf::DT_RELCOUNT; using elf::DT_FLAGS;
using elf::NT_GNU_BUILD_ID;

std::string Stage(std::string what, const std::string& detail) {
  return what + (detail.empty() ? "" : ": " + detail);
}

std::uint32_t AlignUp(std::uint32_t v, std::uint32_t a) { return (v + a - 1) & ~(a - 1); }
std::uint32_t AlignDown(std::uint32_t v, std::uint32_t a) { return v & ~(a - 1); }

/// Read-only view over the file image.
struct Image {
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;

  template <typename T>
  const T* At(std::size_t offset) const {
    if (offset + sizeof(T) > size) return nullptr;
    return reinterpret_cast<const T*>(data + offset);
  }
  const char* CStr(std::size_t offset) const {
    if (offset >= size) return "";
    return reinterpret_cast<const char*>(data + offset);
  }
};

struct Dynamic {
  std::uint32_t strtab = 0, symtab = 0, yment = 0, strsz = 0;
  std::uint32_t rel = 0, relsz = 0, relent = 0;
  std::uint32_t rela = 0, relasz = 0, relaent = 0;
  std::uint32_t jmprel = 0, pltrelsz = 0, pltgot = 0, pltrel_kind = 0;
  std::uint32_t init = 0, fini = 0, soname = 0;
  std::uint32_t init_array = 0, init_array_sz = 0;
  std::vector<std::uint32_t> needed;
  bool present = false;
};

}  // namespace

bool LooksLikeAArch32Elf(const std::uint8_t* data, std::size_t len) {
  static_assert(sizeof(Ehdr32) == 52, "ELF32 header layout assumed by the loader");
  static_assert(sizeof(Phdr32) == 32, "ELF32 program header layout assumed by the loader");
  static_assert(sizeof(Sym32) == 16, "ELF32 symbol layout assumed by the loader");
  static_assert(sizeof(Rel32) == 8 && sizeof(Rela32) == 12, "ELF32 relocation layout");
  static_assert(sizeof(Shdr32) == 40, "ELF32 section header layout");
  if (!data || len < sizeof(Ehdr32)) return false;
  if (data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') return false;
  if (data[4] != 1 /* ELFCLASS32 */) return false;
  if (data[5] != 1 /* ELFDATA2LSB */) return false;
  const auto* h = reinterpret_cast<const Ehdr32*>(data);
  // Exactly EM_ARM. The first draft masked with EF_ARM_MASK — whose bits belong in `e_flags`, not
  // `e_machine` — so any word whose low byte was 40 (`0xFF28` among them) passed the gate and went on
  // to be parsed as ARM.
  return h->e_machine == EM_ARM;
}

namespace {
/// A dynamic symbol name without its version: `strcmp@@LIBC` and `strcmp` are the same caller's
/// question, and the device's `libc.so` uses the suffixed form in `.dynsym` while the relocations that
/// name it do not always.
[[nodiscard]] std::string BaseName(const std::string& name) {
  const auto at = name.find('@');
  return at == std::string::npos ? name : name.substr(0, at);
}
}  // namespace

std::optional<CodeAddress> GuestModule::FindExport(const std::string& name) const {
  /**
   * An IFUNC export is the trap in this file.
   *
   * `st_value` for `strcmp`, `memcpy`, `strlen` and the rest is the **resolver**, so handing it back as
   * "the address of strcmp" makes a caller run the resolver and take its return value — a *pointer to code*
   * — as the result of the call. Measured, not predicted: with the slot resolved and the export still read
   * naively, `strcmp("abc","abd")` came back as `612033` (`0x95601`, inside libc's text) and `memcpy`
   * copied nothing at all, while both looked like successful returns. The answer a caller wants is the
   * implementation the resolver chose, so if a slot for this name has been resolved, that is what gets
   * returned; the resolver stays reachable through `ifunc_slots` for anything that still needs it.
   */
  const std::string base = BaseName(name);
  for (const auto& s : ifunc_slots) {
    if (s.implementation == 0 || s.name.empty() || BaseName(s.name) != base) continue;
    return CodeAddress{s.implementation & ~1u, (s.implementation & 1u) != 0};
  }
  for (const auto& s : exports) {
    if (s.name != name && BaseName(s.name) != base) continue;
    // `exports` holds defined symbols and `imports` the rest, but a name can appear in both — an
    // image may reference a symbol it also defines, and returning the undefined one's value (0) as
    // an entry point is how a guest ends up jumping to the null page.
    if (!s.defined) continue;
    return CodeAddress{s.value & ~1u, s.thumb};
  }
  return std::nullopt;
}

const LocalSymbol* GuestModule::FindLocalSymbolEntry(const std::string& name) const {
  if (!local_symbols || name.empty()) return nullptr;
  // Two stages, each refusing ambiguity: an exact spelling first, then the mangled identifier. A name that
  // two different addresses answer (two files' `static` helpers of one name) is not something to guess at.
  for (const bool by_ident : {false, true}) {
    const LocalSymbol* hit = nullptr;
    bool ambiguous = false;
    for (const LocalSymbol& s : *local_symbols) {
      if ((by_ident ? s.ident : s.name) != name) continue;
      if (hit != nullptr && hit->addr != s.addr) ambiguous = true;
      hit = hit == nullptr ? &s : hit;
    }
    if (ambiguous) return nullptr;
    if (hit != nullptr) return hit;
  }
  return nullptr;
}

std::optional<std::uint32_t> GuestModule::FindLocalSymbol(const std::string& name) const {
  const LocalSymbol* s = FindLocalSymbolEntry(name);
  if (s == nullptr) return std::nullopt;
  return s->addr;
}

std::vector<std::string> GuestModule::ExportNames() const {
  std::vector<std::string> out;
  out.reserve(exports.size());
  for (const auto& s : exports) out.push_back(s.name);
  return out;
}

std::optional<GuestModule> LoadFromBytes(mem::GuestMemory& mem, const std::vector<std::uint8_t>& image,
                                         const std::string& path, const LoadOptions& opts,
                                         std::string* err) {
  // Round-trip through a memfd so there is one mapping path. This is not a test-only shortcut:
  // guest code reached from an APK is handed around as bytes or an asset fd, and the isolated
  // environment passes the same kind of descriptor over Binder (docs/10 §C9), so an in-memory image
  // has to be loadable without ever touching a writable file — which it could not be executed from
  // anyway (docs/10 §7).
  //
  // Called via syscall() because the memfd_create() wrapper is only declared from API 30 and the
  // core builds against android-26.
  int fd = -1;
#if defined(SYS_memfd_create)
  fd = static_cast<int>(syscall(SYS_memfd_create, "rb-guest-elf", MFD_CLOEXEC));
#endif
  if (fd < 0) {
    if (err) *err = Stage("memfd_create unavailable", std::strerror(errno));
    return std::nullopt;
  }
  std::size_t off = 0;
  while (off < image.size()) {
    const ssize_t w = ::write(fd, image.data() + off, image.size() - off);
    if (w <= 0) {
      ::close(fd);
      if (err) *err = Stage("memfd write failed", std::strerror(errno));
      return std::nullopt;
    }
    off += static_cast<std::size_t>(w);
  }
  auto mod = LoadFromFd(mem, fd, path, opts, err);
  ::close(fd);
  return mod;
}

std::optional<GuestModule> LoadFromFd(mem::GuestMemory& mem, int fd, const std::string& path,
                                      const LoadOptions& opts, std::string* err) {
  auto fail = [&](const std::string& msg) -> std::optional<GuestModule> {
    if (err) *err = msg;
    return std::nullopt;
  };

  struct stat st;
  if (::fstat(fd, &st) != 0) return fail(Stage("fstat", std::strerror(errno)));
  const std::size_t fsize = static_cast<std::size_t>(st.st_size);
  if (fsize < sizeof(Ehdr32)) return fail("file too small to be an ELF image");

  auto* raw = static_cast<std::uint8_t*>(
      mmap(nullptr, fsize, PROT_READ, MAP_PRIVATE, fd, 0));
  if (raw == MAP_FAILED) return fail(Stage("mapping the file read-only", std::strerror(errno)));
  struct Guard {
    std::uint8_t* p;
    std::size_t n;
    ~Guard() { munmap(p, n); }
  } guard{raw, fsize};
  Image img{raw, fsize};

  const auto* h = img.At<Ehdr32>(0);
  if (!h) return fail("short header");
  if (!LooksLikeAArch32Elf(img.data, img.size)) {
    return fail("not an AArch32 (ARM, 32-bit, little-endian) ELF image");
  }
  if (h->e_type != ET_DYN) return fail("guest images must be shared objects (ET_DYN)");
  if (h->e_phentsize < sizeof(Phdr32)) return fail("unsupported program header entry size");

  std::vector<Phdr32> phdrs;
  for (std::uint16_t i = 0; i < h->e_phnum; ++i) {
    const auto* p = img.At<Phdr32>(h->e_phoff + static_cast<std::size_t>(i) * h->e_phentsize);
    if (!p) return fail("program headers run past the file");
    phdrs.push_back(*p);
  }

  // Every file range the headers claim must exist in the file. Checked here rather than discovered
  // by a fault: mmap of pages past EOF succeeds, and the first access — this loader's own tail-zero
  // write, or a relocation — raises SIGBUS in the *host*, which is the one failure a loader must
  // never convert its refusals into. A truncated `.so` (a partial APK extraction is the realistic
  // route) keeps an intact header table, so this is the check that catches it.
  for (const auto& p : phdrs) {
    if (p.p_type != PT_LOAD && p.p_type != PT_DYNAMIC && p.p_type != PT_NOTE) continue;
    // The segment's page-aligned end, in 64 bits — checked for every PT_LOAD including BSS-only
    // (`p_filesz == 0`) ones, which skip the file-range checks below and still reach the same 32-bit
    // sums: the image bounds at `hi`, `total_end`, `base + map_start`, and the placement scan's
    // `cand + hi`. A crafted `vaddr + memsz` near the top of the address space makes each of those
    // wrap to a *different* small value: the scan then "fits" a range it never checked and the segment
    // maps wherever the wrap lands, the null page included — and for a BSS-only segment the load would
    // go on to succeed outright. A native
    // linker cannot produce this shape; a refused end also covers the segment whose page-aligned end is
    // exactly 2^32, which the 32-bit AlignUp would fold to zero. (The offset-slack check further down
    // deliberately does *not* apply to `p_filesz == 0` segments: a BSS-only PT_LOAD's `p_offset` is
    // unconstrained by ELF and is commonly 0.)
    if (p.p_type == PT_LOAD) {
      const std::uint64_t end = static_cast<std::uint64_t>(p.p_vaddr) + p.p_memsz;
      const std::uint64_t end_page = (end + (mem::kGuestPage - 1u)) & ~(mem::kGuestPage - 1ull);
      if (end_page > mem.window_bytes()) {
        return fail(Stage("PT_LOAD vaddr+memsz does not fit the guest window",
                          "vaddr " + std::to_string(p.p_vaddr) + " memsz " +
                              std::to_string(p.p_memsz) + " ends " + std::to_string(end_page) +
                              " window " + std::to_string(mem.window_bytes())));
      }
    }
    if (p.p_filesz == 0) continue;
    if (static_cast<std::uint64_t>(p.p_offset) + p.p_filesz > fsize) {
      return fail(Stage("program header claims file bytes past the end of the image",
                        "type " + std::to_string(p.p_type) + " offset " +
                            std::to_string(p.p_offset) + " filesz " + std::to_string(p.p_filesz) +
                            " file " + std::to_string(fsize)));
    }
    if (p.p_type == PT_LOAD) {
      // The mapping's file offset is `p_offset` reduced by the vaddr's page-alignment slack, which
      // must not underflow: an offset smaller than its own slack describes a file that starts
      // negative, and wrapping it would map the *end* of the file as the segment's head.
      const std::uint32_t seg_page = AlignDown(p.p_vaddr, mem::kGuestPage);
      if (p.p_offset < p.p_vaddr - seg_page) {
        return fail(Stage("PT_LOAD file offset below its page alignment",
                          "offset " + std::to_string(p.p_offset) + " vaddr " +
                              std::to_string(p.p_vaddr)));
      }
    }
  }

  // Bounds of everything we must place.
  std::uint32_t lo = 0xffffffffu, hi = 0;
  for (const auto& p : phdrs) {
    if (p.p_type != PT_LOAD) continue;
    lo = std::min(lo, AlignDown(p.p_vaddr, mem::kGuestPage));
    hi = std::max(hi, AlignUp(static_cast<std::uint32_t>(p.p_vaddr + p.p_memsz), mem::kGuestPage));
  }
  if (hi <= lo) return fail("image has no PT_LOAD segments");
  const std::uint32_t span = hi - lo;

  // Placement: honour a caller hint, otherwise the first aligned run with nothing mapped in it.
  //
  // `base` is the **load bias**: an image-relative vaddr V lives at `base + V`, which is how every
  // address below is formed. That is not the same thing as where the image's lowest byte lands, and
  // conflating the two is a real bug waiting for an image whose first PT_LOAD is above 0 — the scan
  // below would check a different range than the one gets mapped, and the image would be placed over
  // somebody else's pages (or past the window) while reporting a clean load.
  std::uint32_t base = 0;
  const std::uint32_t start = AlignUp(opts.hint ? opts.hint : (2 * mem::kGuestPage), mem::kGuestPage);
  for (std::uint32_t cand = start; cand + hi <= mem.window_bytes(); cand += mem::kGuestPage) {
    bool free = true;
    for (std::uint32_t pg = cand + lo; pg < cand + hi; pg += mem::kGuestPage) {
      if (mem.IsMapped(pg)) { free = false; break; }
    }
    if (free) { base = cand; break; }
  }
  if (!base) return fail("no free guest address range large enough for the image");

  GuestModule mod;
  mod.path = path;
  mod.base = base + lo;  // where the image's lowest mapped byte actually sits
  mod.extent = span;     // so [mod.base, mod.base + mod.extent) is exactly the mapped range

  /// Every segment mapped below is undone if the load does not reach the end. Without this, a
  /// rejected image leaves pages mapped in the guest window, and the next load's free-space scan
  /// finds a hole it cannot explain — the corruption surfaces somewhere unrelated to its cause.
  struct Rollback {
    mem::GuestMemory& mem;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    bool committed = false;
    ~Rollback() {
      if (committed) return;
      std::string ignored;
      for (auto& [at, len] : ranges) mem.Unmap(at, len, &ignored);
    }
  } rollback{mem, {}, false};

  // Map every LOAD segment writable; final protections are applied after relocation, because
  // PT_GNU_RELRO regions and the GOT are written here. Doing it in this order is what makes the
  // 16 KiB mixed-granule path fire on real images rather than only in unit tests.
  struct LoadedSeg {
    std::uint32_t vaddr, size, file_off;
    unsigned prot;
  };
  std::vector<LoadedSeg> segs;
  for (const auto& p : phdrs) {
    if (p.p_type != PT_LOAD) continue;
    const std::uint32_t seg_page = AlignDown(p.p_vaddr, mem::kGuestPage);
    const std::uint32_t delta = p.p_vaddr - seg_page;
    const std::uint32_t map_start = seg_page;
    const std::uint32_t total_end =
        AlignUp(p.p_vaddr + std::max(p.p_memsz, p.p_filesz), mem::kGuestPage);
    // Where this segment's own bytes end and where the mapping holding them ends. Those differ
    // whenever p_filesz is not a page multiple, and the gap is *zeros* — the rest of that host page
    // is file content belonging after this segment, not part of it. File-mapping the whole memsz
    // span instead hands the guest the following section's bytes as its BSS, which reads back as
    // plausible garbage and is diagnosed nowhere near here.
    const std::uint32_t content_end = p.p_vaddr + p.p_filesz;
    const std::uint32_t file_end = AlignUp(content_end, mem::kGuestPage);
    LoadedSeg s{};
    s.vaddr = base + map_start;
    s.size = total_end - map_start;
    s.file_off = p.p_offset - delta;
    s.prot = ((p.p_flags & PF_R) ? mem::kRead : 0) | ((p.p_flags & PF_W) ? mem::kWrite : 0) |
             ((p.p_flags & PF_X) ? mem::kExec : 0);
    std::string merr;
    const auto cannot = [&](const char* what) -> std::optional<GuestModule> {
      return fail(Stage(what, std::to_string(s.vaddr) + " " + merr));
    };
    // Writable during relocation regardless of the segment's final flags; restored below.
    if (p.p_filesz == 0) {
      if (!mem.Map(s.vaddr, s.size, s.prot | mem::kWrite, &merr)) return cannot("mapping PT_LOAD at");
    } else {
      if (!mem.MapFile(s.vaddr, file_end - map_start, s.prot | mem::kWrite, fd, s.file_off,
                       /*shared=*/false, &merr)) {
        return cannot("file-mapping PT_LOAD at");
      }
      if (content_end < file_end) {
        static constexpr std::uint8_t kZeros[mem::kGuestPage] = {};
        if (!mem.Write(base + content_end, kZeros, file_end - content_end)) {
          return fail("clearing the tail page of a PT_LOAD");
        }
      }
      if (total_end > file_end &&
          !mem.Map(base + file_end, total_end - file_end, s.prot | mem::kWrite, &merr)) {
        return cannot("mapping the BSS of PT_LOAD at");
      }
    }
    rollback.ranges.emplace_back(s.vaddr, s.size);
    segs.push_back(s);
  }

  // Locate .dynamic and the section headers.
  const Phdr32* dyn_ph = nullptr;
  for (const auto& p : phdrs) {
    if (p.p_type == PT_DYNAMIC) dyn_ph = &p;
  }
  Dynamic dyn;
  if (dyn_ph) {
    dyn.present = true;
    for (std::size_t off = 0; off + sizeof(Dyn32) <= dyn_ph->p_filesz; off += sizeof(Dyn32)) {
      const auto* d = img.At<Dyn32>(dyn_ph->p_offset + off);
      if (!d || d->d_tag == DT_NULL) break;
      switch (d->d_tag) {
        case DT_STRTAB: dyn.strtab = d->d_val; break;
        case DT_STRSZ: dyn.strsz = d->d_val; break;
        case DT_SYMTAB: dyn.symtab = d->d_val; break;
        case DT_SYMENT: dyn.yment = d->d_val; break;
        case DT_REL: dyn.rel = d->d_val; break;
        case DT_RELSZ: dyn.relsz = d->d_val; break;
        case DT_RELENT: dyn.relent = d->d_val; break;
        case DT_RELA: dyn.rela = d->d_val; break;
        case DT_RELASZ: dyn.relasz = d->d_val; break;
        case DT_RELAENT: dyn.relaent = d->d_val; break;
        case DT_JMPREL: dyn.jmprel = d->d_val; break;
        // DT_PLTRELSZ is the length of the JMPREL table. Missing it is not a cosmetic omission:
        // `if (jmprel && pltrelsz)` below then applies nothing, every imported function stays null in
        // the GOT, the refuse-unknown-imports path never runs, and a bad image loads "cleanly".
        case DT_PLTRELSZ: dyn.pltrelsz = d->d_val; break;
        case DT_PLTREL: dyn.pltrel_kind = d->d_val; break;
        case DT_PLTGOT: dyn.pltgot = d->d_val; break;
        case DT_INIT: dyn.init = d->d_val; break;
        case DT_FINI: dyn.fini = d->d_val; break;
        case DT_INIT_ARRAY: dyn.init_array = d->d_val; break;
        case DT_INIT_ARRAYSZ: dyn.init_array_sz = d->d_val; break;
        case DT_SONAME: dyn.soname = d->d_val; break;
        case DT_NEEDED: dyn.needed.push_back(d->d_val); break;
        default: break;
      }
    }
  }

  auto guest_to_file = [&](std::uint32_t vaddr, std::size_t& out) -> bool {
    for (const auto& p : phdrs) {
      if (p.p_type != PT_LOAD) continue;
      if (vaddr >= p.p_vaddr && vaddr < p.p_vaddr + p.p_filesz) {
        out = p.p_offset + (vaddr - p.p_vaddr);
        return true;
      }
    }
    return false;
  };
  // Both helpers take an image-relative vaddr, as relocations do, and apply the load bias
  // themselves. Mixing biased and unbuilt-through addresses between the two is the kind of
  // off-by-base error that would show up as a corrupted pointer far from here.
  //
  // Addends for SHT_REL are read from live guest memory, not from the file image: an earlier
  // relocation can have written the very word a later one reads.
  auto read32 = [&](std::uint32_t vaddr, std::uint32_t& out) -> bool {
    return mem.Read(base + vaddr, &out, 4);
  };
  auto write32 = [&](std::uint32_t vaddr, std::uint32_t value) {
    return mem.Write(base + vaddr, &value, 4);
  };

  const char* strtab = nullptr;
  std::size_t strtab_room = 0;
  {
    std::size_t off;
    if (dyn.present && dyn.strtab && guest_to_file(dyn.strtab, off)) {
      // The whole table has to be in the file image, and names are read bounded by DT_STRSZ (or, if
      // the image omits it, by the file's end): the raw reads a first draft used scanned for a NUL
      // past the table and into whatever the mapping held next, and an offset far past it was a
      // wild-pointer dereference rather than a refusal.
      //
      // A table that runs past the image is a *refusal*, not an empty name. The bound was once only
      // applied: `strtab` was left null, every DT_NEEDED, DT_SONAME and symbol name came back empty, and
      // the report said "no exports / no soname" about an image whose string table was simply damaged.
      // Nothing crashes in that version and the diagnosis blames the guest's own library, which is why
      // it is worse here than the wild read it replaced. The comparison is written subtraction-first so
      // `declared` (a 32-bit DT_STRSZ) cannot wrap the sum.
      const std::size_t declared = dyn.strsz ? dyn.strsz : img.size - off;
      if (declared > img.size || off > img.size - declared) {
        return fail(Stage("DT_STRTAB/DT_STRSZ runs past the end of the image",
                          std::to_string(declared) + " bytes at file offset " + std::to_string(off) +
                              ", image is " + std::to_string(img.size) + " bytes"));
      }
      strtab_room = declared;
      // A zero-size table counts as absent rather than as a one-byte window: every other reader here
      // takes DT_STRSZ == 0 to mean "unset", and DT_STRTAB alone names no length worth trusting.
      if (strtab_room != 0) strtab = img.CStr(off);
    }
  }
  auto str_at = [&](std::uint32_t st_name) -> std::string {
    if (!strtab || st_name >= strtab_room) return {};
    const char* begin = strtab + st_name;
    const char* nul =
        static_cast<const char*>(std::memchr(begin, '\0', strtab_room - st_name));
    return nul ? std::string(begin, nul) : std::string{};
  };
  auto sym_name = [&](std::uint32_t st_name) -> std::string {
    // Bounded by the room actually resolved, not by `dyn.strsz`: for an image that omits DT_STRSZ the
    // only bound available is the file's end, and comparing against a zero `strsz` made every name in
    // such an image empty — the same silent-empty-name shape the refusal above exists to end.
    if (!strtab || st_name >= strtab_room) return {};
    return str_at(st_name);
  };
  const Sym32* symtab = nullptr;
  std::size_t sym_off = 0;
  if (dyn.present && dyn.symtab && guest_to_file(dyn.symtab, sym_off)) symtab = img.At<Sym32>(sym_off);

  // The symbol table's byte length: DT_SYMENT gives an entry's size, not the table's, and no
  // DT_SYMSZ exists, so the section header table is the only authoritative source. Match on
  // sh_addr as much as on sh_type — a linked image also carries a non-alloc .symtab whose symbols
  // index a *different* string table, and pairing DT_SYMTAB's address with that table's entries
  // yields plausible-looking names for the wrong symbols.
  std::uint32_t symtab_bytes = 0;
  if (symtab) {
    if (dyn.yment && dyn.yment != sizeof(Sym32)) {
      return fail("unsupported DT_SYMENT " + std::to_string(dyn.yment));
    }
    if (h->e_shoff && h->e_shentsize >= sizeof(Shdr32)) {
      for (std::uint16_t i = 0; i < h->e_shnum; ++i) {
        const auto* s = img.At<Shdr32>(h->e_shoff + static_cast<std::size_t>(i) * h->e_shentsize);
        if (!s) break;
        if (s->sh_type != SHT_DYNSYM && s->sh_type != SHT_SYMTAB) continue;
        if (s->sh_addr != dyn.symtab) continue;
        if (s->sh_entsize && s->sh_entsize != sizeof(Sym32)) continue;
        symtab_bytes = s->sh_size;
        break;
      }
    }
    // Refusing beats guessing. A scan bounded by nothing gives an image with an empty export set,
    // and the empty set is indistinguishable from a library that genuinely exports nothing — so the
    // failure surfaces much later, as a native method that cannot be registered (AD-5). Guest
    // libraries RetroBug builds itself always have section headers; the case that reaches here is
    // a stripped or hand-built image, which is a finding to report rather than a thing to load.
    if (!symtab_bytes) {
      return fail("cannot bound the symbol table at guest " + std::to_string(dyn.symtab) +
                  ": no matching SHT_DYNSYM section header");
    }
    if (sym_off + symtab_bytes > img.size) {
      return fail("symbol table runs past the file image");
    }
  }

  auto symbol_at = [&](std::uint32_t idx, Sym32& out) -> bool {
    if (!symtab || !dyn.symtab) return false;
    const std::size_t entry_off = sym_off + idx * sizeof(Sym32);
    if (entry_off + sizeof(Sym32) > sym_off + symtab_bytes) return false;
    const auto* p = img.At<Sym32>(entry_off);
    if (!p) return false;
    out = *p;
    return true;
  };

  // ---- PT_TLS, before relocation: a module's own TLS relocations are answered against it.
  for (const auto& p : phdrs) {
    if (p.p_type != PT_TLS) continue;
    if (p.p_filesz > p.p_memsz || p.p_vaddr < lo || p.p_vaddr + p.p_filesz < p.p_vaddr ||
        p.p_vaddr + p.p_filesz > hi) {
      break;  // an image this module's own range does not hold: leave it absent rather than half-trusted
    }
    mod.tls = TlsSegment{base + p.p_vaddr, p.p_filesz, p.p_memsz, p.p_align ? p.p_align : 1u};
    break;
  }

  // ---- relocations
  if (opts.apply_relocations && dyn.present) {
    auto apply = [&](std::uint32_t table, std::uint32_t bytes, std::uint32_t entsize,
                     bool with_addend) -> bool {
      if (!table || !bytes) return true;
      const std::uint32_t ent = entsize ? entsize : (with_addend ? sizeof(Rela32) : sizeof(Rel32));
      for (std::size_t off = 0; off + ent <= bytes; off += ent) {
        std::uint32_t r_offset = 0, r_info = 0, addend = 0;
        if (with_addend) {
          std::size_t f;
          if (!guest_to_file(table + static_cast<std::uint32_t>(off), f)) {
            if (err) *err = "RELA table outside the file image";
            return false;
          }
          const auto* r = img.At<Rela32>(f);
          if (!r) return false;
          r_offset = r->r_offset; r_info = r->r_info; addend = r->r_addend;
        } else {
          std::size_t f;
          if (!guest_to_file(table + static_cast<std::uint32_t>(off), f)) {
            if (err) *err = "REL table outside the file image";
            return false;
          }
          const auto* r = img.At<Rel32>(f);
          if (!r) return false;
          r_offset = r->r_offset; r_info = r->r_info;
          if (!read32(r_offset, addend)) {
            if (err) *err = "cannot read REL addend";
            return false;
          }
        }
        // The target must fall inside the image's own span. Checked even though the write itself is
        // bounds-checked, because a wrapped r_offset (`0xFFFFF000` against a small base) lands on
        // the *previous* module's writable data — mapped, so the write succeeds, the load reports
        // success, and the corruption surfaces somewhere else entirely.
        if (r_offset < lo || r_offset >= hi) {
          if (err) *err = Stage("relocation target outside the image",
                                "r_offset " + std::to_string(r_offset) + " outside [" +
                                    std::to_string(lo) + ", " + std::to_string(hi) + ")");
          return false;
        }
        const std::uint32_t type = ELF32_R_TYPE(r_info);
        const std::uint32_t sidx = ELF32_R_SYM(r_info);

        /**
         * Which relocations have a symbol at all.
         *
         * `R_ARM_IRELATIVE` has no symbol field by definition, and a TLS relocation with `r_sym == 0`
         * refers to *this* module's TLS block. Passing either through the resolver anyway was not
         * harmless: the caller got asked to resolve `""` — the null symbol's name — and an `libc.so`
         * with three TLS slots and one IRELATIVE produced four such queries, which is exactly the noise
         * that had a test reporting a "nameless LOCAL import" that does not exist in the image.
         */
        const bool tls_kind =
            type == R_ARM_TLS_DTPMOD32 || type == R_ARM_TLS_DTPOFF32 || type == R_ARM_TLS_TPOFF32;
        const bool needs_symbol = type != R_ARM_NONE && type != R_ARM_RELATIVE &&
                                  type != R_ARM_IRELATIVE && !(tls_kind && sidx == 0);

        Sym32 sym{};
        std::string sname;
        bool have_sym = false;
        if (needs_symbol) {
          have_sym = symbol_at(sidx, sym);
          sname = have_sym ? sym_name(sym.st_name) : std::string{};
          // A relocation that names a symbol we cannot read must stop the load. Without this the
          // code below treats sym_addr as 0 and writes a null into the GOT: the guest then branches
          // to address 0 at some later, unrelated moment, and the crash points nowhere near here.
          if (!have_sym) {
            if (err) *err = "relocation " + std::to_string(sidx) + " references an unreadable symbol" +
                            " (symtab located: " + (symtab ? "yes" : "no") + ")";
            return false;
          }
        }
        std::uint32_t sym_addr = 0;
        if (have_sym && sym.st_shndx != SHN_UNDEF) {
          sym_addr = (sym.st_shndx == SHN_ABS) ? sym.st_value : base + sym.st_value;
        } else if (have_sym && !opts.resolve) {
          if (ELF32_ST_BIND(sym.st_info) != STB_WEAK) {
            if (err) *err = Stage("imported symbol has no resolver", sname);
            return false;
          }
        } else if (have_sym) {
          const auto found = opts.resolve(sname);
          if (!found) {
            if (ELF32_ST_BIND(sym.st_info) != STB_WEAK) {
              if (err) *err = Stage("unresolved symbol", sname);
              return false;
            }
          } else if (opts.unresolved_trap != 0 && *found - opts.unresolved_trap < opts.unresolved_trap_span &&
                     ELF32_ST_BIND(sym.st_info) == STB_WEAK) {
            sym_addr = 0;  // an unresolved weak reference is null, never the trap (see LoadOptions)
          } else {
            sym_addr = *found;
          }
        }

        switch (type) {
          case R_ARM_NONE:
            break;
          case R_ARM_RELATIVE:
            if (!write32(r_offset, static_cast<std::uint32_t>(addend + base))) {
              if (err) *err = "failed writing R_ARM_RELATIVE";
              return false;
            }
            break;
          case R_ARM_ABS32:
            if (!write32(r_offset, sym_addr + addend)) {
              if (err) *err = "failed writing R_ARM_ABS32";
              return false;
            }
            break;
          case R_ARM_GLOB_DAT:
          case R_ARM_JUMP_SLOT:
            if (!write32(r_offset, sym_addr)) {
              if (err) *err = "failed writing GOT slot";
              return false;
            }
            // A GOT entry for an IFUNC symbol holds the *resolver*, and every caller of it wants the
            // resolver's answer. Writing the resolver is the right placeholder — a guest that calls it
            // today gets the resolver running, which is measurably close to correct (`memcpy`'s returns
            // an implementation pointer in four steps) — but the slot still has to be noticed, because
            // `strlen`'s resolver probes `/dev/cpu_variant:arm` with a syscall and a half-resolved IFUNC
            // otherwise looks like a bug in the string code rather than an unfinished load.
            if (have_sym && ELF32_ST_TYPE(sym.st_info) == STT_GNU_IFUNC) {
              mod.ifunc_slots.push_back(IfuncSlot{
                  base + r_offset, CodeAddress{sym_addr & ~1u, (sym_addr & 1u) != 0}, sym_addr, sname});
            }
            break;
          case R_ARM_IRELATIVE: {
            /**
             * AArch32's REL form has no addend field, so the slot's own contents are the addend: bionic's
             * rule is `*slot = resolver(load_bias + *slot)` **[1A]**, `linker_relocate.cpp` at
             * `android-16.0.0_r1`. This writes the biased resolver — the pre-call value — and records the
             * slot for the linker to finish.
             */
            const std::uint32_t resolver = static_cast<std::uint32_t>(addend + base);
            if (!write32(r_offset, resolver)) {
              if (err) *err = "failed writing R_ARM_IRELATIVE slot";
              return false;
            }
            mod.ifunc_slots.push_back(IfuncSlot{
                base + r_offset, CodeAddress{resolver & ~1u, (resolver & 1u) != 0}, resolver, {}});
            break;
          }
          default:
            if (tls_kind && sidx == 0 && mod.tls && opts.resolve_tls) {
              if (const std::optional<std::uint32_t> v = opts.resolve_tls(type, addend, *mod.tls)) {
                if (!write32(r_offset, *v)) {
                  if (err) *err = "failed writing a TLS relocation slot";
                  return false;
                }
                break;
              }
            }
            // Refused kinds are recorded with their address and type, and — unless the caller set
            // `tolerate_unresolved_relocs` — they fail the load at the end of relocation with every slot
            // named. That flag exists for one reason: IFUNC needs a guest call, so the linker loads the
            // module, resolves the slots itself, and *then* the image is complete. Nobody else should get
            // the permissive behaviour, because a skipped TLS slot is not a missing pointer waiting to be
            // dereferenced at a clean address — bionic computes those offsets unconditionally at load, so
            // the guest simply reads a wrong value and carries on. `docs/30` §Step 26 measured three such
            // slots in a real `libc.so` under the old always-record default.
            mod.unsupported_reloc_types.push_back(type);
            char buf[96];
            std::snprintf(buf, sizeof buf, "reloc type 0x%02x at guest 0x%x (%s)", type, r_offset,
                          sname.empty() ? "?" : sname.c_str());
            mod.unsupported_relocs.emplace_back(buf);
            break;
        }
      }
      return true;
    };

    if (!apply(dyn.rel, dyn.relsz, dyn.relent, false)) return std::nullopt;
    if (!apply(dyn.rela, dyn.relasz, dyn.relaent, true)) return std::nullopt;
    // DT_PLTREL declares whether the JMPREL table is REL (17) or RELA (7) form. Guessing here would
    // read each 8-byte entry with the wrong stride, so an unknown or unsupported kind is refused
    // rather than half-applied.
    if (dyn.jmprel && dyn.pltrelsz) {
      const bool rela_form = (dyn.pltrel_kind == static_cast<std::uint32_t>(DT_RELA));
      const bool rel_form = (dyn.pltrel_kind == static_cast<std::uint32_t>(DT_REL));
      if (!rela_form && !rel_form) {
        return fail("PLT relocation table has unsupported DT_PLTREL kind " +
                    std::to_string(dyn.pltrel_kind));
      }
      if (!apply(dyn.jmprel, dyn.pltrelsz,
                 rela_form ? (dyn.relaent ? dyn.relaent : sizeof(Rela32))
                            : (dyn.relent ? dyn.relent : sizeof(Rel32)),
                 rela_form)) {
        return std::nullopt;
      }
    }

    // Every table has now been walked, so the refusals are final and can be reported together. One error
    // naming all of them beats failing on the first: a library with three TLS slots and one IFUNC is one
    // piece of work, not three debug sessions.
    if (!mod.unsupported_relocs.empty() && !opts.tolerate_unresolved_relocs) {
      std::string what = "refused " + std::to_string(mod.unsupported_relocs.size()) +
                         " relocation(s) that this loader cannot apply: ";
      for (std::size_t i = 0; i < mod.unsupported_relocs.size(); ++i) {
        what += (i ? "; " : "") + mod.unsupported_relocs[i];
      }
      what += " — loading would leave those slots holding file data the guest treats as an address. "
              "Resolve them through the linker, or pass tolerate_unresolved_relocs to say you will";
      return fail(what);
    }
  }

  // ---- IFUNC resolution, while the image is still writable
  //
  // This is the only place it can happen: the RELRO pass below strips write from exactly the segments the
  // GOT entries live in, which `docs/30` §Step 27 measured by trying to write them after the load and
  // being refused all eight times. The loader still runs no guest code of its own — it asks, and the
  // caller who owns an executor answers — so C05's "no execution needed" invariant survives for every
  // caller that leaves the hook unset.
  if (opts.resolve_ifunc) {
    for (auto& s : mod.ifunc_slots) {
      const std::optional<std::uint32_t> answer = opts.resolve_ifunc(s);
      if (!answer) continue;
      if (!mem.Write(s.where, &*answer, 4)) {
        return fail("could not write the resolved IFUNC implementation into guest 0x" +
                    std::to_string(s.where) + " (" + (s.name.empty() ? "<R_ARM_IRELATIVE>" : s.name) +
                    ")");
      }
      s.implementation = *answer;
    }
  }

  // ---- final protections, then RELRO, then read-only text
  for (const auto& s : segs) {
    std::string merr;
    if (!mem.Protect(s.vaddr, s.size, s.prot, &merr)) {
      return fail(Stage("applying final segment protection", merr));
    }
  }
  if (opts.enforce_relro) {
    for (const auto& p : phdrs) {
      if (p.p_type != PT_GNU_RELRO) continue;
      const std::uint32_t from = AlignDown(p.p_vaddr, mem::kGuestPage);
      const std::uint32_t to = AlignUp(static_cast<std::uint32_t>(p.p_vaddr + p.p_memsz),
                                       mem::kGuestPage);
      std::string merr;
      // Read-only after relocation, keeping whatever exec bit the covering segment had.
      unsigned keep = mem::kRead;
      for (const auto& s : segs) {
        if (base + p.p_vaddr >= s.vaddr && base + p.p_vaddr < s.vaddr + s.size) keep |= (s.prot & mem::kExec);
      }
      if (!mem.Protect(base + from, to - from, keep, &merr)) {
        return fail(Stage("applying PT_GNU_RELRO", merr));
      }
    }
  }

  // ---- metadata
  if (dyn.present) {
    // Bit 0 of a code address is the ARM ABI's Thumb tag, not part of the address, and it survives
    // the load bias because bases are page-aligned. Splitting it here is the only place it can be
    // told apart from the address itself; downstream there is nothing left to recover it from.
    const auto code = [&](std::uint32_t v) -> CodeAddress {
      return CodeAddress{v ? base + (v & ~1u) : 0, (v & 1u) != 0};
    };
    mod.entry = code(h->e_entry);
    mod.init = code(dyn.init);
    mod.fini = code(dyn.fini);
    /**
     * `DT_INIT_ARRAY` points at an array of function pointers and `DT_INIT_ARRAYSZ` gives its byte size.
     * Recorded here, still not *called* — that is C06's job, and it needs a dependency scope to run in
     * the right order. What this closes is the gap between what the notes claimed and what the code did:
     * the array was described as "parsed but never run" when the `.dynamic` loop had no case for it at
     * all, so a module whose startup work lives there (every NDK and bionic image, since none of them
     * sets `DT_INIT` — see `CodeAddress init`'s comment) was silently reporting that it had none.
     *
     * Read from live guest memory rather than the file image, because the array sits in a RELRO segment
     * and an image whose entries were relocated before this point would otherwise have stale file
     * contents recorded. A pointer that does not resolve is a malformed image, and refusing with the
     * index that failed is what makes that visible instead of loading a module whose constructors are
     * whatever the next page happened to hold.
     */
    /**
     * Read from live guest memory, which matters more than it looks: `.init_array` slots carry
     * `R_ARM_RELATIVE` (or `R_ARM_ABS32` against a local symbol) relocations, so after the relocation
     * pass each entry is **already a guest address**. Adding the load bias a second time aims every
     * constructor at `base` bytes above its target, which is not a crash but a fetch of whatever is
     * there — zeros in a mapped page, which the front end reports as an unallocated encoding. That is
     * exactly how this was found: the first `rb::linker` run of a real constructor faulted on an
     * instruction that plainly exists, and the address in the message was 0x2000 too high.
     *
     * With relocations switched off the slots still hold image-relative vaddrs, so the bias is theirs to
     * take. Both cases are stated because the difference is invisible in the value and visible the moment
     * something tries to execute it.
     */
    if (dyn.init_array != 0 && dyn.init_array_sz != 0) {
      if (dyn.init_array_sz % 4u != 0u) {
        return fail(Stage("reading DT_INIT_ARRAY",
                          "size " + std::to_string(dyn.init_array_sz) +
                              " is not a multiple of a 32-bit entry"));
      }
      const std::size_t count = dyn.init_array_sz / 4u;
      mod.init_array.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t fn = 0;
        if (!read32(dyn.init_array + static_cast<std::uint32_t>(i * 4u), fn)) {
          return fail(Stage("reading DT_INIT_ARRAY",
                            "entry " + std::to_string(i) + " of " + std::to_string(count) +
                                " is at an unmapped vaddr"));
        }
        // 0 and -1 are not functions: pre-r8 NDK images bracket the array with a -1 head and a 0 tail
        // (Gumball 2013's .fini_array is `ffffffff d8060300 00000000`), those slots carry no relocation,
        // and bionic's `call_function` skips both. Recording -1 made the launcher call 0xfffffffe.
        if (fn == 0u || fn == 0xffff'ffffu) {
          ++mod.init_array_sentinels;
          continue;
        }
        const std::uint32_t entry = opts.apply_relocations ? fn : base + fn;
        mod.init_array.push_back(
            CodeAddress{entry ? (entry & ~1u) : 0u, (entry & 1u) != 0u});
      }
    }
    if (dyn.soname && strtab) mod.soname = str_at(dyn.soname);
    for (auto n : dyn.needed) {
      auto name = str_at(n);
      if (!name.empty()) mod.needed.emplace_back(std::move(name));
    }
    if (symtab && symtab_bytes) {
      const std::size_t nsyms = symtab_bytes / sizeof(Sym32);
      for (std::size_t i = 0; i < nsyms; ++i) {
        Sym32 sym;
        if (!symbol_at(static_cast<std::uint32_t>(i), sym)) break;
        const std::uint8_t type = ELF32_ST_TYPE(sym.st_info);
        // Every undefined weak reference, whatever its type: `imports` below keeps only typed symbols, and
        // a NOTYPE weak one (Gumball 2013's three `__cxa_*`) is still a name the linker leaves at 0.
        if (sym.st_shndx == SHN_UNDEF && ELF32_ST_BIND(sym.st_info) == STB_WEAK) {
          const std::string weak_name = sym_name(sym.st_name);
          if (!weak_name.empty()) mod.weak_imports.push_back(weak_name);
        }
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_GNU_IFUNC) continue;
        Symbol out;
        out.name = sym_name(sym.st_name);
        if (out.name.empty()) continue;
        out.size = sym.st_size;
        out.is_function = (type == STT_FUNC || type == STT_GNU_IFUNC);
        out.ifunc = (type == STT_GNU_IFUNC);
        out.thumb = out.is_function && (sym.st_value & 1u);
        out.weak = ELF32_ST_BIND(sym.st_info) == STB_WEAK;
        if (sym.st_shndx == SHN_UNDEF) {
          out.defined = false;
          mod.imports.push_back(out);
        } else {
          out.defined = true;
          out.value = (sym.st_shndx == SHN_ABS) ? sym.st_value : base + sym.st_value;
          mod.exports.push_back(out);
        }
      }
    }
  }

  // ---- the full `.symtab`, when the image kept one. Read from the file through the section headers —
  // it is not in any PT_LOAD, so guest memory never holds it — and only defined, named FUNC/OBJECT/NOTYPE
  // entries are kept. Every bound is checked against the file; a table that does not fit is skipped
  // (the module simply has no local symbols), because it is diagnostic data and not part of loading.
  if (h->e_shoff != 0 && h->e_shnum != 0 && h->e_shentsize >= sizeof(Shdr32) &&
      h->e_shoff + static_cast<std::uint64_t>(h->e_shnum) * h->e_shentsize <= img.size) {
    const auto shdr = [&](std::uint32_t i) {
      return img.At<Shdr32>(h->e_shoff + static_cast<std::size_t>(i) * h->e_shentsize);
    };
    for (std::uint16_t i = 0; i < h->e_shnum; ++i) {
      const Shdr32* s = shdr(i);
      if (s == nullptr || s->sh_type != SHT_SYMTAB || s->sh_link >= h->e_shnum) continue;
      const Shdr32* str = shdr(s->sh_link);
      if (str == nullptr || s->sh_offset + static_cast<std::uint64_t>(s->sh_size) > img.size ||
          str->sh_offset + static_cast<std::uint64_t>(str->sh_size) > img.size) {
        break;
      }
      auto table = std::make_shared<std::vector<LocalSymbol>>();
      const std::size_t n = s->sh_size / sizeof(Sym32);
      table->reserve(n);
      for (std::size_t k = 1; k < n; ++k) {
        const auto* sym = img.At<Sym32>(s->sh_offset + k * sizeof(Sym32));
        if (sym == nullptr || sym->st_shndx == SHN_UNDEF || sym->st_name >= str->sh_size) continue;
        const std::uint8_t type = ELF32_ST_TYPE(sym->st_info);
        if (type != STT_FUNC && type != STT_OBJECT && type != 0 /* NOTYPE */) continue;
        const char* p = img.CStr(str->sh_offset + sym->st_name);
        const std::size_t room = str->sh_size - sym->st_name;
        const std::string name(p, strnlen(p, room));
        if (name.empty() || name[0] == '$') continue;  // `$a`/`$t`/`$d` mapping symbols, not names
        LocalSymbol out;
        out.name = name;
        out.is_function = type == STT_FUNC;
        out.thumb = out.is_function && (sym->st_value & 1u) != 0;
        const std::uint32_t v = sym->st_shndx == SHN_ABS ? sym->st_value : base + sym->st_value;
        out.addr = out.is_function ? (v & ~1u) : v;
        out.size = sym->st_size;
        // `_Z[L]<len><ident>`: the source name of a mangled plain (non-nested) symbol.
        if (name.size() > 3 && name[0] == '_' && name[1] == 'Z') {
          std::size_t at = 2;
          if (name[at] == 'L') ++at;
          std::size_t len = 0;
          const std::size_t digits = at;
          while (at < name.size() && name[at] >= '0' && name[at] <= '9' && len <= name.size()) {
            len = len * 10 + static_cast<std::size_t>(name[at++] - '0');
          }
          if (at > digits && len != 0 && at + len <= name.size()) out.ident = name.substr(at, len);
        }
        table->push_back(std::move(out));
      }
      mod.local_symbols = std::move(table);
      break;
    }
  }

  // ---- build-id, when present (it is absent on the NDK-built fixture)
  for (const auto& p : phdrs) {
    if (p.p_type != PT_NOTE) continue;
    std::size_t off = 0;
    while (off + 12 <= p.p_filesz) {
      const auto* n = img.At<std::uint32_t>(p.p_offset + off);
      const auto* dsz = img.At<std::uint32_t>(p.p_offset + off + 4);
      const auto* type = img.At<std::uint32_t>(p.p_offset + off + 8);
      if (!n || !dsz || !type) break;
      const std::size_t namesz = *n, descsz = *dsz;
      const std::size_t hdr = 12;
      if (off + hdr + namesz + descsz > p.p_filesz) break;
      if (*type == NT_GNU_BUILD_ID && namesz == 4 &&
          std::memcmp(img.data + p.p_offset + off + hdr, "GNU", 4) == 0) {
        // Bounded by the file, not by the note's own claim: the note header's earlier check bounds
        // against `p_filesz` without the descriptor's alignment padding, and a descsz past the file
        // was a read off the end of the mapping.
        const std::size_t desc_off = p.p_offset + off + hdr + AlignUp(namesz, 4u);
        if (desc_off + descsz <= img.size) {
          mod.build_id.assign(img.data + desc_off, img.data + desc_off + descsz);
        }
      }
      off += hdr + AlignUp(namesz, 4u) + AlignUp(descsz, 4u);
    }
  }

  // ---- the ARM unwind index. Recorded for `dl_unwind_find_exidx`, which libgcc's unwinder reaches
  // through bionic's `__gnu_Unwind_Find_exidx` on every C++ throw: without it no frame has unwind info,
  // no catch can be found, and a thrown exception is `std::terminate` (Angry Birds' first missing asset
  // was exactly that). Entries are 8 bytes, so a size that is not a multiple of 8 is not an index.
  for (const auto& p : phdrs) {
    if (p.p_type != PT_ARM_EXIDX) continue;
    if (p.p_memsz % 8u != 0 || p.p_vaddr + p.p_memsz < p.p_vaddr ||
        p.p_vaddr + p.p_memsz > mod.extent) {
      break;  // not an index this module's own range holds; leave it absent rather than half-trusted
    }
    mod.exidx = mod.base + p.p_vaddr;
    mod.exidx_count = p.p_memsz / 8u;
    break;
  }

  rollback.committed = true;
  return mod;
}

}  // namespace rb::loader
