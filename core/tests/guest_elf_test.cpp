/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the guest ELF loader (C05), against a real armeabi-v7a shared object built by
 * tools/run_engine_tests.sh.
 *
 * Deliberately free of hard-coded addresses. The expected value of every relocation, the identity
 * of every export and import, the RELRO range and the BSS extent are all read out of the fixture's
 * own program headers, .dynamic and symbol table, then compared against what the loader put into
 * guest memory. Hardcoding offsets here would have made the test quietly wrong the first time the
 * fixture's source changed — it already moved twice while this file was being written.
 *
 * What this is really asserting: that RELATIVE and JUMP_SLOT are applied to the right words with
 * the right values (a swapped relocation constant produces no error and a corrupted pointer), that
 * imports are refused rather than bound to zero, and that write-permission state after the load
 * matches the image's intent — RELRO and text both readable, neither writable, BSS zero.
 */
#include "rb/loader/elf_defs.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifndef RB_WEAK_FIXTURE_PATH
#define RB_WEAK_FIXTURE_PATH "/data/local/tmp/librbweak.so"
#endif
#ifndef RB_FIXTURE_PATH
#define RB_FIXTURE_PATH "/data/local/tmp/librb_fixture.so"
#endif

namespace {

using namespace rb;
using namespace rb::mem;
namespace elf = rb::loader::elf;

int failures = 0;
int checks = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK(...)                 \
  do {                             \
    ++checks;                      \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)

#define CHECK_CTX(cond, detail)              \
  do {                                       \
    ++checks;                                \
    if (!(cond)) fail(#cond, __LINE__, detail); \
  } while (0)

std::string Hex(std::uint32_t v) {
  char buf[12];
  std::snprintf(buf, sizeof(buf), "%x", v);
  return buf;
}

std::string Join(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& e : v) {
    if (!s.empty()) s += ", ";
    s += e;
  }
  return s;
}

std::vector<std::uint8_t> read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

/// The fixture, parsed the way the loader parses it — used only to state expectations.
struct Fixture {
  std::vector<std::uint8_t> bytes;
  bool ok = false;
  std::uint32_t entry = 0;
  std::vector<elf::Phdr32> loads;
  std::uint32_t relro_vaddr = 0, relro_memsz = 0;
  std::uint32_t strtab = 0, strsz = 0, symtab = 0, yment = 0, symtab_bytes = 0;
  /// One past the largest symbol index any relocation in this image refers to. Independent of the
  /// section header table, so it constrains symtab_bytes from a source the bound is not derived
  /// from — a scan truncated to one entry would otherwise agree with itself.
  std::uint32_t sym_index_high_water = 0;
  std::uint32_t rel = 0, relsz = 0, relent = 0;
  std::uint32_t jmprel = 0, pltrelsz = 0;
  std::uint32_t init_array = 0, init_array_sz = 0;
  std::vector<std::string> needed;
  std::vector<std::uint32_t> needed_off;  ///< raw DT_NEEDED values, before .dynstr is located
  std::vector<std::string> import_names;
  std::vector<std::pair<std::string, std::uint32_t>> export_funcs;  ///< name, st_value
  std::string last_named;       ///< name of the highest-index enumerated symbol in .dynsym
  bool last_named_undef = false;

  std::optional<std::size_t> file_off(std::uint32_t vaddr) const {
    for (const auto& p : loads) {
      if (vaddr >= p.p_vaddr && vaddr < p.p_vaddr + p.p_filesz) {
        return static_cast<std::size_t>(p.p_offset + (vaddr - p.p_vaddr));
      }
    }
    return std::nullopt;
  }
  template <typename T>
  std::optional<T> at_vaddr(std::uint32_t vaddr) const {
    auto off = file_off(vaddr);
    if (!off || *off + sizeof(T) > bytes.size()) return std::nullopt;
    T v;
    std::memcpy(&v, bytes.data() + *off, sizeof(T));
    return v;
  }
  /// A string at an offset **into .dynstr** — which is what `st_name`, DT_NEEDED and DT_SONAME all
  /// hold. Taking the same value as a vaddr instead (the mistake this replaced) lands inside the
  /// ELF header and returns garbage as a dependency name; the loader was right and this test was
  /// reporting a mismatch it had caused itself.
  std::string strtab_at(std::uint32_t strtab_offset) const {
    auto off = file_off(strtab + strtab_offset);
    if (!off) return {};
    std::string s;
    for (std::size_t i = *off; i < bytes.size() && bytes[i]; ++i) s.push_back(
        static_cast<char>(bytes[i]));
    return s;
  }
  /// The .dynsym section header, located the only way it can be: ELF has no DT_SYMSZ, so the
  /// dynamic section says where the table starts (DT_SYMTAB) and how wide an entry is (DT_SYMENT,
  /// 16) but never how long the table is. Matching on sh_addr as well as sh_type keeps this off the
  /// non-alloc .symtab, whose symbols index a different string table.
  std::optional<elf::Shdr32> dynsym() const {
    const auto* h = reinterpret_cast<const elf::Ehdr32*>(bytes.data());
    if (!h->e_shoff || h->e_shentsize < sizeof(elf::Shdr32)) return std::nullopt;
    for (std::uint16_t i = 0; i < h->e_shnum; ++i) {
      const std::size_t off = h->e_shoff + std::size_t{i} * h->e_shentsize;
      if (off + sizeof(elf::Shdr32) > bytes.size()) break;
      elf::Shdr32 s;
      std::memcpy(&s, bytes.data() + off, sizeof(s));
      if (s.sh_type != elf::SHT_DYNSYM && s.sh_type != elf::SHT_SYMTAB) continue;
      if (s.sh_addr != symtab) continue;
      return s;
    }
    return std::nullopt;
  }
};

Fixture parse_fixture() {
  Fixture f;
  f.bytes = read_file(RB_FIXTURE_PATH);
  if (f.bytes.size() < sizeof(elf::Ehdr32)) {
    fail("fixture missing at " RB_FIXTURE_PATH, __LINE__);
    return f;
  }
  const auto* h = reinterpret_cast<const elf::Ehdr32*>(f.bytes.data());
  f.entry = h->e_entry;
  for (std::uint16_t i = 0; i < h->e_phnum; ++i) {
    const auto* p = reinterpret_cast<const elf::Phdr32*>(
        f.bytes.data() + h->e_phoff + std::size_t{i} * h->e_phentsize);
    if (p->p_type == elf::PT_LOAD) f.loads.push_back(*p);
    if (p->p_type == elf::PT_GNU_RELRO) {
      f.relro_vaddr = p->p_vaddr;
      f.relro_memsz = p->p_memsz;
    }
    if (p->p_type == elf::PT_DYNAMIC) {
      const auto* d = reinterpret_cast<const elf::Dyn32*>(f.bytes.data() + p->p_offset);
      for (std::size_t o = 0; o < p->p_filesz / sizeof(elf::Dyn32); ++o) {
        // Must stop at DT_NULL: the segment is padded past the terminator, and continuing reads the
        // filler as tags — which is what produced relocation entries at addresses the image does not
        // have and made this test report the loader as broken when the test itself was wrong.
        if (d[o].d_tag == elf::DT_NULL) break;
        switch (d[o].d_tag) {
          case elf::DT_STRTAB: f.strtab = d[o].d_val; break;
          case elf::DT_STRSZ: f.strsz = d[o].d_val; break;
          case elf::DT_SYMTAB: f.symtab = d[o].d_val; break;
          case elf::DT_SYMENT: f.yment = d[o].d_val; break;
          case elf::DT_REL: f.rel = d[o].d_val; break;
          case elf::DT_RELSZ: f.relsz = d[o].d_val; break;
          case elf::DT_RELENT: f.relent = d[o].d_val; break;
          case elf::DT_JMPREL: f.jmprel = d[o].d_val; break;
          case elf::DT_PLTRELSZ: f.pltrelsz = d[o].d_val; break;
          case elf::DT_NEEDED: f.needed_off.push_back(d[o].d_val); break;
          case elf::DT_INIT_ARRAY: f.init_array = d[o].d_val; break;
          case elf::DT_INIT_ARRAYSZ: f.init_array_sz = d[o].d_val; break;
          default: break;
        }
      }
    }
  }
  // DT_NEEDED values are .dynstr offsets, but DT_STRTAB itself may appear after them in the table,
  // so names can only be resolved once the whole table has been read. Filling `f.needed` in place
  // here looked reasonable and silently kept only the last dependency.
  for (std::uint32_t off : f.needed_off) f.needed.push_back(f.strtab_at(off));
  if (f.symtab) {
    if (auto s = f.dynsym()) f.symtab_bytes = s->sh_size;
  }
  if (f.symtab && f.symtab_bytes) {
    for (std::size_t o = 0; o + sizeof(elf::Sym32) <= f.symtab_bytes; o += sizeof(elf::Sym32)) {
      auto s = f.at_vaddr<elf::Sym32>(f.symtab + static_cast<std::uint32_t>(o));
      if (!s) break;
      const auto name = f.strtab_at(s->st_name);
      if (name.empty()) continue;
      const std::uint8_t type = elf::ELF32_ST_TYPE(s->st_info);
      if (s->st_shndx == elf::SHN_UNDEF) {
        f.import_names.push_back(name);
      } else if (type == elf::STT_FUNC) {
        f.export_funcs.emplace_back(name, s->st_value);
      }
      // Only the types the loader enumerates, or the last-named probe would ask for a symbol the
      // loader is entitled to skip.
      if (type == elf::STT_FUNC || type == elf::STT_OBJECT || type == elf::STT_GNU_IFUNC) {
        f.last_named = name;
        f.last_named_undef = (s->st_shndx == elf::SHN_UNDEF);
      }
    }
  }
  {
    const auto note = [&](std::uint32_t table, std::uint32_t bytes, std::uint32_t ent) {
      for (std::size_t o = 0; o + ent <= bytes; o += ent) {
        auto r = f.at_vaddr<elf::Rel32>(table + static_cast<std::uint32_t>(o));
        if (!r) break;
        f.sym_index_high_water =
            std::max(f.sym_index_high_water, elf::ELF32_R_SYM(r->r_info) + 1u);
      }
    };
    const std::uint32_t relent = f.relent ? f.relent : sizeof(elf::Rel32);
    note(f.rel, f.relsz, relent);
    note(f.jmprel, f.pltrelsz, sizeof(elf::Rel32));
  }
  f.ok = !f.loads.empty() && f.symtab && f.symtab_bytes && (f.rel || f.jmprel);
  // Counts on stderr: when the loader and this disagree, the first question is which side parsed
  // badly, and guessing cost two iterations already.
  std::fprintf(stderr,
               "[fixture] loads=%zu rel=0x%x relsz=%u jmprel=0x%x pltrelsz=%u symtab=0x%x "
               "symtab_bytes=%u yment=%u symidx_hi=%u strtab=0x%x exports=%zu imports=%zu "
               "needed=%zu [%s] ok=%d\n",
               f.loads.size(), f.rel, f.relsz, f.jmprel, f.pltrelsz, f.symtab, f.symtab_bytes,
               f.yment, f.sym_index_high_water, f.strtab, f.export_funcs.size(),
               f.import_names.size(), f.needed.size(), Join(f.needed).c_str(), f.ok ? 1 : 0);
  std::fflush(stderr);
  if (!f.ok) fail("fixture parsing produced nothing to assert against", __LINE__);
  return f;
}

std::unique_ptr<GuestMemory> make_mem() {
  long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = kGuestPageShift;
  while (static_cast<long>(1) << shift < ps) ++shift;
  AddressModel m;
  m.kind = AddressModel::kBiased;
  m.host_page_shift = shift;
  std::string err;
  auto gm = GuestMemory::Create(32u << 20, m, &err);
  if (!gm) fail("memory creation failed", __LINE__, err);
  return gm;
}

/// A distinct, recognisable address per import: if the loader ever writes a wrong symbol's address
/// into a GOT slot, the value is still a plausible pointer, so the checks below compare each slot
/// against the address this resolver assigned to that slot's own symbol name.
loader::SymbolResolver make_resolver(const Fixture& f, std::map<std::string, std::uint32_t>& given) {
  for (std::size_t i = 0; i < f.import_names.size(); ++i) {
    given[f.import_names[i]] = 0x0900'0000u + static_cast<std::uint32_t>(i) * 0x100u;
  }
  return [&given](const std::string& name) -> std::optional<std::uint32_t> {
    auto it = given.find(name);
    return it == given.end() ? std::nullopt : std::optional<std::uint32_t>{it->second};
  };
}

std::uint32_t read_word(const GuestMemory& gm, std::uint32_t addr) {
  std::uint32_t v = 0xBADFA11;
  if (!const_cast<GuestMemory&>(gm).Read(addr, &v, 4)) return 0xBADFA11;
  return v;
}

// ------------------------------------------------------------------------ tests

void recognises_only_aarch32_images() {
  Fixture f = parse_fixture();
  if (f.bytes.empty()) return;
  CHECK(loader::LooksLikeAArch32Elf(f.bytes.data(), f.bytes.size()));
  CHECK(!loader::LooksLikeAArch32Elf(nullptr, 0));

  auto self = read_file("/proc/self/exe");
  CHECK(!self.empty());
  CHECK(!loader::LooksLikeAArch32Elf(self.data(), self.size()));  // x86_64 or arm64, either way no

  auto bad_magic = f.bytes;
  bad_magic[4] = 2;  // ELFCLASS64
  CHECK(!loader::LooksLikeAArch32Elf(bad_magic.data(), bad_magic.size()));
  auto bad_machine = f.bytes;
  bad_machine[18] = 0x3E;  // e_machine -> x86-64
  CHECK(!loader::LooksLikeAArch32Elf(bad_machine.data(), bad_machine.size()));
}

void imports_are_refused_not_bound_to_zero() {
  Fixture f = parse_fixture();
  auto gm = make_mem();
  if (!f.ok || !gm) return;
  loader::LoadOptions opts;  // deliberately no resolver
  std::string err;
  auto mod = loader::LoadFromBytes(*gm, f.bytes, "librb_fixture.so", opts, &err);
  CHECK(!mod.has_value());
  // Any of the fixture's imports is an acceptable name to report; what matters is that the failure
  // is loud and names something, because a silent zero becomes a guest branch to address 0.
  bool named = !err.empty();
  for (const auto& n : f.import_names) {
    if (err.find(n) != std::string::npos) named = true;
  }
  CHECK(named);
  // A failed load must not leave guest pages behind: a partially-mapped image would be reached by a
  // later load's free-space scan and reported as corruption somewhere else entirely.
  CHECK(gm->mapped_page_count() == 0);
}

void applies_every_relocation_the_image_declares() {
  Fixture f = parse_fixture();
  auto gm = make_mem();
  if (!f.ok || !gm) return;

  std::map<std::string, std::uint32_t> given;
  loader::LoadOptions opts;
  opts.resolve = make_resolver(f, given);
  std::string err;
  auto mod = loader::LoadFromBytes(*gm, f.bytes, "librb_fixture.so", opts, &err);
  if (!mod) {
    fail("load failed", __LINE__, err);
    return;
  }
  const std::uint32_t base = mod->base;
  CHECK(err.empty());
  CHECK(mod->unsupported_relocs.empty());
  CHECK(mod->entry == loader::CodeAddress{f.entry ? base + (f.entry & ~1u) : 0, (f.entry & 1u) != 0});
  CHECK_CTX(mod->needed == f.needed,
            "loader=[" + Join(mod->needed) + "] fixture=[" + Join(f.needed) + "]");

  // DT_INIT_ARRAY, entry by entry against an independent read of the same image. The array is the tag
  // every real image actually uses — `DT_INIT` is zero on the NDK and device images measured here, so
  // before the loader had a case for it a module with constructors reported that it had none.
  //
  // On *this* fixture the loop below is vacuous and the count is `0 == 0`: `llvm-readelf -d` reports no
  // `DT_INIT_ARRAY` for `librb_fixture.so`, which the printed line at the end makes visible rather than
  // leaving a green row implying coverage it does not have. The case that can fail lives in
  // `rb_guest_libc_tests`, where the loaded image is a real libc with five entries.
  CHECK_CTX(mod->init_array.size() == f.init_array_sz / 4u,
            "loader recorded " + std::to_string(mod->init_array.size()) + " init entries, DT_INIT_ARRAYSZ "
            "says " + std::to_string(f.init_array_sz / 4u));
  for (std::size_t i = 0; i < mod->init_array.size(); ++i) {
    const auto raw = f.at_vaddr<std::uint32_t>(
        f.init_array + static_cast<std::uint32_t>(i * 4u));
    CHECK_CTX(raw.has_value(), "init_array[" + std::to_string(i) +
                                   "] is not readable in the image, so the loader's entry cannot be "
                                   "checked against anything");
    if (!raw) break;
    const loader::CodeAddress want{*raw ? base + (*raw & ~1u) : 0u, (*raw & 1u) != 0u};
    CHECK_CTX(mod->init_array[i] == want,
              "init_array[" + std::to_string(i) +
                  "] differs from an independent read of the image — the bias or the Thumb tag is wrong");
  }
  std::fprintf(stderr, "[fixture] init_array=%u sz=%u entries=%zu\n", f.init_array, f.init_array_sz,
               mod->init_array.size());

  // The symbol table's extent, cross-checked against something other than the section header table
  // the loader read: the largest symbol index any relocation names has to be inside it.
  CHECK(f.yment == 0 || f.yment == sizeof(elf::Sym32));
  CHECK_CTX(f.symtab_bytes / sizeof(elf::Sym32) >= f.sym_index_high_water,
              "symtab_bytes=" + std::to_string(f.symtab_bytes) +
                  " but a relocation names symbol index " +
                  std::to_string(f.sym_index_high_water - 1));
  // And the loader has to have walked that far. This is the exact shape of the bug DT_SYMSZ
  // caused: a bound of 16 bytes means one entry, which is the null symbol, which yields two empty
  // lists and a successful-looking load.
  CHECK(!f.last_named.empty());
  {
    const auto last = std::find_if(mod->exports.begin(), mod->exports.end(),
                                   [&](const loader::Symbol& s) { return s.name == f.last_named; });
    const auto last_imp = std::find_if(mod->imports.begin(), mod->imports.end(),
                                       [&](const loader::Symbol& s) { return s.name == f.last_named; });
    CHECK_CTX(last != mod->exports.end() || last_imp != mod->imports.end(),
              "highest-index symbol " + f.last_named + " is missing from the loader's tables");
    CHECK_CTX((last_imp != mod->imports.end()) == f.last_named_undef,
              "highest-index symbol " + f.last_named + " has the wrong defined/undefined state");
  }

  std::uint32_t top = 0;
  for (const auto& p : f.loads) top = std::max(top, p.p_vaddr + p.p_memsz);
  CHECK(mod->extent >= ((top + kGuestPage - 1) & ~(kGuestPage - 1)));
  CHECK(mod->base + mod->extent <= gm->window_bytes());

  // .rel.dyn — R_ARM_RELATIVE: expected value is the word already in the file plus the load bias.
  std::size_t relative_seen = 0;
  const std::uint32_t relent = f.relent ? f.relent : sizeof(elf::Rel32);
  for (std::size_t o = 0; o + relent <= f.relsz; o += relent) {
    auto r = f.at_vaddr<elf::Rel32>(f.rel + static_cast<std::uint32_t>(o));
    if (!r) break;
    if (elf::ELF32_R_TYPE(r->r_info) != elf::R_ARM_RELATIVE) continue;
    ++relative_seen;
    const auto addend = f.at_vaddr<std::uint32_t>(r->r_offset);
    if (!addend) continue;
    const std::uint32_t want = *addend + base;
    const std::uint32_t got = read_word(*gm, base + r->r_offset);
    CHECK_CTX(got == want,
              "R_ARM_RELATIVE at guest vaddr 0x" + Hex(r->r_offset) + " want 0x" + Hex(want) +
                  " got 0x" + Hex(got) + " (file word 0x" + Hex(*addend) + ", base 0x" + Hex(base) + ")");
  }
  CHECK(relative_seen > 0);

  // .rel.plt — R_ARM_JUMP_SLOT: each GOT slot must hold the address this resolver gave that slot's
  // own symbol. Swapping JUMP_SLOT/RELATIVE, or writing one symbol's address into another's slot,
  // fails here.
  std::size_t slot_seen = 0;
  for (std::size_t o = 0; o + sizeof(elf::Rel32) <= f.pltrelsz; o += sizeof(elf::Rel32)) {
    auto r = f.at_vaddr<elf::Rel32>(f.jmprel + static_cast<std::uint32_t>(o));
    if (!r) break;
    if (elf::ELF32_R_TYPE(r->r_info) != elf::R_ARM_JUMP_SLOT) continue;
    auto s = f.at_vaddr<elf::Sym32>(f.symtab + elf::ELF32_R_SYM(r->r_info) * sizeof(elf::Sym32));
    if (!s) break;
    const std::string name = f.strtab_at(s->st_name);
    auto it = given.find(name);
    if (it == given.end()) continue;
    ++slot_seen;
    const std::uint32_t got = read_word(*gm, base + r->r_offset);
    CHECK_CTX(got == it->second, "JUMP_SLOT for " + name + " at 0x" + Hex(r->r_offset) +
                                     " want 0x" + Hex(it->second) + " got 0x" + Hex(got));
  }
  CHECK(slot_seen > 0);

  // Exports: every function the image defines is reported at st_value + bias, mapped, and inside a
  // segment the loader has made read-only executable. Every export in this fixture is a Thumb
  // function, so this is also where the bit-0 tag has to come out right: branch to the unmasked value
  // and the guest runs its Thumb bytes through the ARM decoder, which does not fault — it does
  // something else.
  for (const auto& [name, value] : f.export_funcs) {
    auto found = mod->FindExport(name);
    CHECK_CTX(found.has_value(), "export " + name);
    if (!found) continue;
    CHECK_CTX(found->addr == base + (value & ~1u),
              name + " address: want 0x" + Hex(base + (value & ~1u)) + " got 0x" + Hex(found->addr));
    CHECK_CTX(found->thumb == ((value & 1u) != 0), name + " Thumb flag (st_value 0x" + Hex(value) + ")");
    CHECK(gm->IsMapped(found->addr));
    std::uint32_t scratch = 0;
    CHECK_CTX(gm->Write(found->addr, &scratch, 4) == false, name + " must not be writable");
  }
  CHECK(!f.export_funcs.empty());

  // Imports are surfaced for the binding layer (AD-5 builds its native index from these). The count
  // is exact because both sides use the same criterion — a named symbol with st_shndx == SHN_UNDEF —
  // while exports are only ">=" because this test records functions and the loader also lists
  // objects and IFUNCs.
  CHECK_CTX(mod->imports.size() == f.import_names.size(),
              "loader imports=" + std::to_string(mod->imports.size()) +
                  " fixture imports=" + std::to_string(f.import_names.size()));
  CHECK_CTX(mod->exports.size() >= f.export_funcs.size(),
              "loader exports=" + std::to_string(mod->exports.size()) +
                  " fixture functions=" + std::to_string(f.export_funcs.size()));
  for (const auto& n : f.import_names) {
    const bool seen = std::any_of(mod->imports.begin(), mod->imports.end(),
                                  [&](const loader::Symbol& s) { return s.name == n && !s.defined; });
    CHECK_CTX(seen, "import " + n + " not reported");
  }

  // RELRO: relocated, then read-only. This is the step that makes the mixed 4 KiB/16 KiB granule
  // path fire on a real image rather than only in the memory model's own tests.
  if (f.relro_memsz) {
    const std::uint32_t where = base + f.relro_vaddr;
    std::uint32_t v = 0;
    CHECK(gm->Write(where, &v, 4) == false);
    CHECK(gm->Read(where, &v, 4));
  }

  // BSS: memsz > filesz, and the tail must read zero.
  for (const auto& p : f.loads) {
    if (p.p_memsz <= p.p_filesz) continue;
    for (std::uint32_t i = p.p_filesz; i < p.p_memsz; i += 4) {
      std::uint32_t v = 0xA5A5A5A5u;
      if (!gm->Read(base + p.p_vaddr + i, &v, 4)) continue;  // past the window end
      CHECK_CTX(v == 0, "BSS byte at +" + std::to_string(i));
      break;  // one probe is enough; the whole tail is checked by the model's own zeroing
    }
  }
}

void two_loads_are_independent_copies() {
  Fixture f = parse_fixture();
  auto gm = make_mem();
  if (!f.ok || !gm) return;
  std::map<std::string, std::uint32_t> given;
  loader::LoadOptions opts;
  opts.resolve = make_resolver(f, given);
  std::string err;
  auto a = loader::LoadFromBytes(*gm, f.bytes, "a.so", opts, &err);
  auto b = loader::LoadFromBytes(*gm, f.bytes, "b.so", opts, &err);
  CHECK(a.has_value());
  CHECK(b.has_value());
  if (!a || !b) return;
  CHECK(a->base != b->base);
  CHECK(b->base >= a->base + a->extent);

  // Both copies relocated against the same imports, each with its own bias.
  std::size_t checked = 0;
  const std::uint32_t relent = f.relent ? f.relent : sizeof(elf::Rel32);
  for (std::size_t o = 0; o + relent <= f.relsz; o += relent) {
    auto r = f.at_vaddr<elf::Rel32>(f.rel + static_cast<std::uint32_t>(o));
    if (!r) break;
    if (elf::ELF32_R_TYPE(r->r_info) != elf::R_ARM_RELATIVE) continue;
    auto addend = f.at_vaddr<std::uint32_t>(r->r_offset);
    if (!addend) continue;
    CHECK(read_word(*gm, a->base + r->r_offset) == *addend + a->base);
    CHECK(read_word(*gm, b->base + r->r_offset) == *addend + b->base);
    ++checked;
  }
  CHECK(checked > 0);
}

void truncated_image_is_refused() {
  Fixture f = parse_fixture();
  auto gm = make_mem();
  if (!f.ok || !gm) return;
  loader::LoadOptions opts;
  std::map<std::string, std::uint32_t> given;
  opts.resolve = make_resolver(f, given);
  std::vector<std::uint8_t> cut(f.bytes.begin(), f.bytes.begin() + 0x40);
  std::string err;
  CHECK(!loader::LoadFromBytes(*gm, cut, "cut.so", opts, &err));
  CHECK(!err.empty());

  // Truncated *past the header table*: the phdrs still parse, so the refusal has to come from the
  // file-range check rather than a short read. This is the shape a partial APK extraction produces,
  // and without the check the segment mapped anyway and the first access — the tail-zero write or a
  // relocation — raised SIGBUS in the host instead of naming a stage.
  const std::size_t half = f.bytes.size() / 2;
  std::vector<std::uint8_t> mid(f.bytes.begin(), f.bytes.begin() + half);
  CHECK(!loader::LoadFromBytes(*gm, mid, "cut-mid.so", opts, &err));
  CHECK_CTX(!err.empty(), "a half-truncated image loaded: " + err);
}

/// A stripped or hand-built image with no section header table has no way to say how long .dynsym
/// is. The loader must say so: loading it "successfully" with an empty export set is the failure
/// this whole path exists to prevent, because the empty set is indistinguishable from a library
/// that genuinely exports nothing and only shows up later as a native method that will not bind.
void symbol_table_without_a_bound_is_refused() {
  Fixture f = parse_fixture();
  auto gm = make_mem();
  if (!f.ok || !gm) return;
  auto bytes = f.bytes;
  auto* eh = reinterpret_cast<elf::Ehdr32*>(bytes.data());
  CHECK(eh->e_shoff != 0);
  eh->e_shoff = 0;

  std::map<std::string, std::uint32_t> given;
  loader::LoadOptions opts;
  opts.resolve = make_resolver(f, given);
  std::string err;
  CHECK(!loader::LoadFromBytes(*gm, bytes, "noshdrs.so", opts, &err));
  CHECK_CTX(err.find("symbol table") != std::string::npos, "error was: " + err);
  CHECK(gm->mapped_page_count() == 0);
}

/// A string table declared past the end of the image is refused by name.
///
/// This is the half of the earlier bound fix that was only *applied* rather than enforced: a `DT_STRSZ`
/// larger than the file left the table unresolved, every name came back empty, and the load reported "no
/// exports / no soname" about an image whose string table was simply damaged. Nothing crashed and nothing
/// was named, which is why it needed a test rather than a careful reader.
void a_string_table_past_the_end_of_the_image_is_refused() {
  Fixture f = parse_fixture();
  auto gm = make_mem();
  if (!f.ok || !gm) return;
  auto bytes = f.bytes;
  const auto* h = reinterpret_cast<const elf::Ehdr32*>(bytes.data());
  bool patched = false;
  for (std::uint16_t i = 0; i < h->e_phnum && !patched; ++i) {
    const std::size_t ph = h->e_phoff + std::size_t{i} * h->e_phentsize;
    if (ph + sizeof(elf::Phdr32) > bytes.size()) break;
    elf::Phdr32 p;
    std::memcpy(&p, bytes.data() + ph, sizeof(p));
    if (p.p_type != elf::PT_DYNAMIC || p.p_offset + p.p_filesz > bytes.size()) continue;
    for (std::size_t o = 0; o * sizeof(elf::Dyn32) + sizeof(elf::Dyn32) <= p.p_filesz; ++o) {
      auto* d = reinterpret_cast<elf::Dyn32*>(bytes.data() + p.p_offset + o * sizeof(elf::Dyn32));
      if (d->d_tag == elf::DT_NULL) break;
      if (d->d_tag != elf::DT_STRSZ) continue;
      // Past the end of the image, and small enough that the sum cannot wrap on any host.
      d->d_val = static_cast<std::uint32_t>(bytes.size()) * 4u;
      patched = true;
      break;
    }
  }
  CHECK_CTX(patched, "the fixture has no DT_STRSZ entry to damage");

  std::map<std::string, std::uint32_t> given;
  loader::LoadOptions opts;
  opts.resolve = make_resolver(f, given);
  std::string err;
  CHECK(!loader::LoadFromBytes(*gm, bytes, "badstrsz.so", opts, &err));
  CHECK_CTX(err.find("DT_STRSZ") != std::string::npos,
            "a string table past the end of the image was not refused by name: " + err);
}

/// A `vaddr + memsz` that would wrap the 32-bit address space is refused rather than allowed to fold
/// each consumer's 32-bit sum to a different small number: the placement scan would "fit" a range it
/// never checked and the segment would map wherever the wrap landed — the null page included — while
/// the load reported success. Patching a real fixture rather than hand-building an image keeps the
/// rest of it valid, so only the new check can be what refuses this one.
void a_wrapping_pt_load_is_refused() {
  Fixture f = parse_fixture();
  auto gm = make_mem();
  if (!f.ok || !gm) return;
  auto bytes = f.bytes;
  const auto* h = reinterpret_cast<const elf::Ehdr32*>(bytes.data());
  bool patched = false;
  for (std::uint16_t i = 0; i < h->e_phnum && !patched; ++i) {
    const std::size_t ph = h->e_phoff + std::size_t{i} * h->e_phentsize;
    if (ph + sizeof(elf::Phdr32) > bytes.size()) break;
    auto* p = reinterpret_cast<elf::Phdr32*>(bytes.data() + ph);
    if (p->p_type != elf::PT_LOAD) continue;
    p->p_vaddr = 0xffff'e000u;  // one 8 KiB page below the top of the address space
    p->p_memsz = 0x0000'2000u;  // so vaddr + memsz == 2^32 exactly, the end that folds to zero
    patched = true;
  }
  CHECK_CTX(patched, "the fixture has no PT_LOAD entry to damage");

  std::map<std::string, std::uint32_t> given;
  loader::LoadOptions opts;
  opts.resolve = make_resolver(f, given);
  std::string err;
  CHECK(!loader::LoadFromBytes(*gm, bytes, "wrap.so", opts, &err));
  CHECK_CTX(err.find("guest window") != std::string::npos,
            "a wrapping PT_LOAD was not refused by name: " + err);
  CHECK_CTX(gm->mapped_page_count() == 0, "a refused wrapping image left pages mapped");
}

/// `PT_ARM_EXIDX` is recorded biased, with the entry count the file's own program header gives, and the
/// first entry read back through guest memory is the file's first entry — so a missing bias, a byte
/// count reported as entries, or an index aimed at the wrong segment each fail here. It is what
/// `dl_unwind_find_exidx` answers from; answering NULL instead made every C++ throw in Angry Birds
/// terminate on the SM-X710.
void the_unwind_index_is_recorded_biased() {
  Fixture f = parse_fixture();
  auto gm = make_mem();
  if (!f.ok || !gm) return;
  const auto* h = reinterpret_cast<const elf::Ehdr32*>(f.bytes.data());
  std::uint32_t vaddr = 0, memsz = 0, offset = 0;
  for (std::uint16_t i = 0; i < h->e_phnum; ++i) {
    const std::size_t ph = h->e_phoff + std::size_t{i} * h->e_phentsize;
    if (ph + sizeof(elf::Phdr32) > f.bytes.size()) break;
    const auto* p = reinterpret_cast<const elf::Phdr32*>(f.bytes.data() + ph);
    if (p->p_type != elf::PT_ARM_EXIDX) continue;
    vaddr = p->p_vaddr;
    memsz = p->p_memsz;
    offset = p->p_offset;
  }
  CHECK_CTX(memsz != 0, "the NDK fixture carries no PT_ARM_EXIDX, so this row cannot test anything");
  if (memsz == 0) return;

  std::map<std::string, std::uint32_t> given;
  loader::LoadOptions opts;
  opts.resolve = make_resolver(f, given);
  std::string err;
  auto mod = loader::LoadFromBytes(*gm, f.bytes, "exidx.so", opts, &err);
  CHECK_CTX(mod.has_value(), err);
  if (!mod) return;
  CHECK_CTX(mod->base != 0, "base 0 cannot tell a biased address from an unbiased one");
  CHECK_CTX(mod->exidx == mod->base + vaddr,
            "exidx=" + std::to_string(mod->exidx) + " want base+vaddr=" + std::to_string(mod->base + vaddr));
  CHECK_CTX(mod->exidx_count == memsz / 8u,
            "count=" + std::to_string(mod->exidx_count) + " want " + std::to_string(memsz / 8u));
  // Entry 0's first word (a prel31 offset to its function) as the guest sees it, against the file.
  std::uint32_t file_word = 0;
  std::memcpy(&file_word, f.bytes.data() + offset, 4);
  CHECK_CTX(read_word(*gm, mod->exidx) == file_word, "the recorded index does not hold the file's entries");
}

/// An unresolved *weak* reference is bound to 0, even when the resolver answers misses with a trap
/// address. Angry Birds Go's own `__cxa_guard_acquire` calls `__google_potentially_blocking_region_begin`
/// only when its address is non-null; with the launcher's `0xbad0000` trap in the slot the first guarded
/// static initialiser faulted. The row runs both ways: with `unresolved_trap` set the word is 0, without
/// it the loader takes the resolver at its word and the word is the trap — so neither a loader that
/// ignores the option nor one that zeroes every weak answer passes.
void an_unresolved_weak_reference_is_null_not_the_trap() {
  const auto bytes = read_file(RB_WEAK_FIXTURE_PATH);
  CHECK_CTX(!bytes.empty(), "fixture missing at " RB_WEAK_FIXTURE_PATH);
  if (bytes.empty()) return;
  static constexpr std::uint32_t kTrap = 0x0bad'0000u;
  for (const bool with_trap : {true, false}) {
    auto gm = make_mem();
    if (!gm) return;
    loader::LoadOptions opts;
    opts.resolve = [](const std::string&) -> std::optional<std::uint32_t> { return kTrap; };
    if (with_trap) opts.unresolved_trap = kTrap;
    std::string err;
    auto mod = loader::LoadFromBytes(*gm, bytes, "librbweak.so", opts, &err);
    CHECK_CTX(mod.has_value(), err);
    if (!mod) return;
    std::uint32_t at = 0;
    for (const auto& s : mod->exports) {
      if (s.defined && s.name == "g_weak_ptr") at = s.value;
    }
    CHECK_CTX(at != 0, "librbweak.so exports no g_weak_ptr");
    if (at == 0) return;
    const std::uint32_t word = read_word(*gm, at);
    CHECK_CTX(word == (with_trap ? 0u : kTrap), std::string{with_trap ? "with" : "without"} +
                                                     " unresolved_trap, g_weak_ptr = " + std::to_string(word));
  }
}

}  // namespace

int main() {
  recognises_only_aarch32_images();
  imports_are_refused_not_bound_to_zero();
  applies_every_relocation_the_image_declares();
  two_loads_are_independent_copies();
  truncated_image_is_refused();
  symbol_table_without_a_bound_is_refused();
  a_string_table_past_the_end_of_the_image_is_refused();
  a_wrapping_pt_load_is_refused();
  the_unwind_index_is_recorded_biased();
  an_unresolved_weak_reference_is_null_not_the_trap();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
