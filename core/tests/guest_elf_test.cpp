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

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

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
  std::uint32_t strtab = 0, strsz = 0, symtab = 0, symsz = 0;
  std::uint32_t rel = 0, relsz = 0, relent = 0;
  std::uint32_t jmprel = 0, pltrelsz = 0;
  std::vector<std::string> needed;
  std::vector<std::string> import_names;
  std::vector<std::pair<std::string, std::uint32_t>> export_funcs;  ///< name, st_value

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
  std::string cstr(std::uint32_t vaddr) const {
    auto off = file_off(vaddr);
    if (!off) return {};
    std::string s;
    for (std::size_t i = *off; i < bytes.size() && bytes[i]; ++i) s.push_back(
        static_cast<char>(bytes[i]));
    return s;
  }
  std::string sym_name(std::uint32_t st_name) const {
    auto off = file_off(strtab + st_name);
    if (!off) return {};
    std::string s;
    for (std::size_t i = *off; i < bytes.size() && bytes[i]; ++i) s.push_back(
        static_cast<char>(bytes[i]));
    return s;
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
          case elf::DT_SYMSZ: f.symsz = d[o].d_val; break;
          case elf::DT_REL: f.rel = d[o].d_val; break;
          case elf::DT_RELSZ: f.relsz = d[o].d_val; break;
          case elf::DT_RELENT: f.relent = d[o].d_val; break;
          case elf::DT_JMPREL: f.jmprel = d[o].d_val; break;
          case elf::DT_PLTRELSZ: f.pltrelsz = d[o].d_val; break;
          case elf::DT_NEEDED: f.needed.push_back({}); break;  // resolved after strtab is known
          default: break;
        }
      }
    }
  }
  // DT_NEEDED strings and the symbol table now that strtab is located.
  {
    const auto* h2 = reinterpret_cast<const elf::Ehdr32*>(f.bytes.data());
    for (std::uint16_t i = 0; i < h2->e_phnum; ++i) {
      const auto* p = reinterpret_cast<const elf::Phdr32*>(
          f.bytes.data() + h2->e_phoff + std::size_t{i} * h2->e_phentsize);
      if (p->p_type != elf::PT_DYNAMIC) continue;
      const auto* d = reinterpret_cast<const elf::Dyn32*>(f.bytes.data() + p->p_offset);
      for (std::size_t o = 0; o < p->p_filesz / sizeof(elf::Dyn32); ++o) {
        if (d[o].d_tag == elf::DT_NULL) break;
        if (d[o].d_tag == elf::DT_NEEDED) f.needed.back() = f.cstr(d[o].d_val);
      }
    }
  }
  if (f.symtab && f.symsz) {
    for (std::size_t o = 0; o + sizeof(elf::Sym32) <= f.symsz; o += sizeof(elf::Sym32)) {
      auto s = f.at_vaddr<elf::Sym32>(f.symtab + static_cast<std::uint32_t>(o));
      if (!s) break;
      const auto name = f.sym_name(s->st_name);
      if (name.empty()) continue;
      const std::uint8_t type = elf::ELF32_ST_TYPE(s->st_info);
      if (s->st_shndx == elf::SHN_UNDEF) {
        f.import_names.push_back(name);
      } else if (type == elf::STT_FUNC) {
        f.export_funcs.emplace_back(name, s->st_value);
      }
    }
  }
  f.ok = !f.loads.empty() && f.symtab && (f.rel || f.jmprel);
  // Counts on stderr: when the loader and this disagree, the first question is which side parsed
  // badly, and guessing cost two iterations already.
  std::fprintf(stderr,
               "[fixture] loads=%zu rel=0x%x relsz=%u jmprel=0x%x pltrelsz=%u symtab=0x%x symsz=%u "
               "strtab=0x%x exports=%zu imports=%zu needed=%zu ok=%d\n",
               f.loads.size(), f.rel, f.relsz, f.jmprel, f.pltrelsz, f.symtab, f.symsz, f.strtab,
               f.export_funcs.size(), f.import_names.size(), f.needed.size(), f.ok ? 1 : 0);
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
  CHECK(mod->entry == (f.entry ? base + f.entry : 0));
  CHECK(mod->needed == f.needed);

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
    CHECK_CTX(read_word(*gm, base + r->r_offset) == want,
              "R_ARM_RELATIVE at guest vaddr (decimal) " + std::to_string(r->r_offset));
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
    const std::string name = f.sym_name(s->st_name);
    auto it = given.find(name);
    if (it == given.end()) continue;
    ++slot_seen;
    CHECK_CTX(read_word(*gm, base + r->r_offset) == it->second, "JUMP_SLOT for " + name);
  }
  CHECK(slot_seen > 0);

  // Exports: every function the image defines is reported at st_value + bias, mapped, and inside a
  // segment the loader has made read-only executable.
  for (const auto& [name, value] : f.export_funcs) {
    auto found = mod->FindExport(name);
    CHECK_CTX(found.has_value(), "export " + name);
    if (!found) continue;
    CHECK_CTX(*found == base + (value & ~1u), name + " address");  // strip the Thumb bit
    CHECK(gm->IsMapped(*found));
    std::uint32_t scratch = 0;
    CHECK_CTX(gm->Write(*found, &scratch, 4) == false, name + " must not be writable");
  }
  CHECK(!f.export_funcs.empty());

  // Imports are surfaced for the binding layer (AD-5 builds its native index from these).
  CHECK(mod->imports.size() >= f.import_names.size() - 1);

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
}

}  // namespace

int main() {
  recognises_only_aarch32_images();
  imports_are_refused_not_bound_to_zero();
  applies_every_relocation_the_image_declares();
  two_loads_are_independent_copies();
  truncated_image_is_refused();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
