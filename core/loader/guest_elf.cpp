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
using elf::PT_GNU_RELRO; using elf::PT_ARM_EXIDX;
using elf::PF_X; using elf::PF_W; using elf::PF_R;
using elf::SHN_UNDEF; using elf::SHN_ABS;
using elf::SHT_DYNSYM; using elf::SHT_SYMTAB;
using elf::STT_OBJECT; using elf::STT_FUNC; using elf::STT_GNU_IFUNC;
using elf::STB_GLOBAL; using elf::STB_WEAK;
using elf::ELF32_ST_BIND; using elf::ELF32_ST_TYPE; using elf::ELF32_R_SYM; using elf::ELF32_R_TYPE;
using elf::R_ARM_NONE; using elf::R_ARM_ABS32; using elf::R_ARM_COPY;
using elf::R_ARM_GLOB_DAT; using elf::R_ARM_JUMP_SLOT; using elf::R_ARM_RELATIVE;
using elf::DT_NULL; using elf::DT_NEEDED; using elf::DT_PLTGOT; using elf::DT_PLTRELSZ;
using elf::DT_STRTAB; using elf::DT_STRSZ; using elf::DT_SYMTAB; using elf::DT_SYMENT;
using elf::DT_INIT; using elf::DT_FINI; using elf::DT_SONAME;
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
  return (h->e_machine & ~EF_ARM_MASK) == EM_ARM;
}

std::optional<CodeAddress> GuestModule::FindExport(const std::string& name) const {
  for (const auto& s : exports) {
    if (s.name != name) continue;
    // `exports` holds defined symbols and `imports` the rest, but a name can appear in both — an
    // image may reference a symbol it also defines, and returning the undefined one's value (0) as
    // an entry point is how a guest ends up jumping to the null page.
    if (!s.defined) continue;
    return CodeAddress{s.value & ~1u, s.thumb};
  }
  return std::nullopt;
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
  {
    std::size_t off;
    if (dyn.present && dyn.strtab && guest_to_file(dyn.strtab, off)) strtab = img.CStr(off);
  }
  auto sym_name = [&](std::uint32_t st_name) -> std::string {
    if (!strtab || st_name >= dyn.strsz) return {};
    return std::string(strtab + st_name);
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
        const std::uint32_t type = ELF32_R_TYPE(r_info);
        const std::uint32_t sidx = ELF32_R_SYM(r_info);

        Sym32 sym{};
        std::string sname;
        bool have_sym = false;
        if (type != R_ARM_RELATIVE && type != R_ARM_NONE) {
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
            break;
          default:
            // Unsupported kinds are reported, not applied wrongly. TLS and COPY are the realistic
            // ones; a silent skip here would corrupt a guest pointer instead.
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
    if (dyn.soname && strtab) mod.soname = std::string(strtab + dyn.soname);
    for (auto n : dyn.needed) {
      if (strtab) mod.needed.emplace_back(strtab + n);
    }
    if (symtab && symtab_bytes) {
      const std::size_t nsyms = symtab_bytes / sizeof(Sym32);
      for (std::size_t i = 0; i < nsyms; ++i) {
        Sym32 sym;
        if (!symbol_at(static_cast<std::uint32_t>(i), sym)) break;
        const std::uint8_t type = ELF32_ST_TYPE(sym.st_info);
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_GNU_IFUNC) continue;
        Symbol out;
        out.name = sym_name(sym.st_name);
        if (out.name.empty()) continue;
        out.size = sym.st_size;
        out.is_function = (type == STT_FUNC || type == STT_GNU_IFUNC);
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
        const auto* desc = img.data + p.p_offset + off + hdr + AlignUp(namesz, 4u);
        mod.build_id.assign(desc, desc + descsz);
      }
      off += hdr + AlignUp(namesz, 4u) + AlignUp(descsz, 4u);
    }
  }

  rollback.committed = true;
  return mod;
}

}  // namespace rb::loader
