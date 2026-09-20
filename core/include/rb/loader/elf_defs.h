/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * ELF32 / AArch32 structures and constants, shared by the loader and its tests.
 *
 * Defined here rather than included from <elf.h> because that header is Linux-only: it does not
 * exist on iOS, and this code is part of the core that must compile unchanged on both targets
 * (docs/10 §13.1). Only the fields RetroBug reads are declared, in wire order, and the
 * static_asserts in guest_elf.cpp check the layout against the ABI at compile time.
 *
 * The relocation numbers below were checked against a real armeabi-v7a image rather than
 * remembered: .rel.dyn carries r_info 0x17 (23 = RELATIVE) and .rel.plt carries 0x16
 * (22 = JUMP_SLOT). Neighbouring ARM values are close enough that a recalled table swaps them,
 * and a swapped table still loads cleanly while corrupting every pointer.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace rb::loader::elf {

struct Ehdr32 {
  std::uint8_t e_ident[16];
  std::uint16_t e_type, e_machine;
  std::uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
  std::uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Phdr32 {
  std::uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
};
struct Shdr32 {
  std::uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info;
  std::uint32_t sh_addralign, sh_entsize;
};
struct Dyn32 {
  std::int32_t d_tag;
  std::uint32_t d_val;
};
struct Rel32 {
  std::uint32_t r_offset, r_info;
};
struct Rela32 {
  std::uint32_t r_offset, r_info, r_addend;
};
struct Sym32 {
  std::uint32_t st_name, st_value, st_size;
  std::uint8_t st_info, st_other;
  std::uint16_t st_shndx;  ///< 16-bit in ELF32 — Elf32_Sym is 12+1+1+2 = 16 bytes exactly.
};

constexpr std::uint16_t ET_DYN = 3;
constexpr std::uint16_t EM_ARM = 40;
constexpr std::uint16_t EF_ARM_MASK = 0xff00u;

constexpr std::uint32_t PT_LOAD = 1, PT_DYNAMIC = 2, PT_NOTE = 4;
constexpr std::uint32_t PT_GNU_RELRO = 0x6474e552u, PT_ARM_EXIDX = 0x70000068u;
constexpr std::uint32_t PF_X = 1, PF_W = 2, PF_R = 4;

constexpr std::uint32_t SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4,
                        SHT_HASH = 6, SHT_DYNAMIC = 21, SHT_NOTE = 7, SHT_REL = 9,
                        SHT_DYNSYM = 11, SHT_GNU_HASH = 0x6ffffff6u, SHT_ARM_EXIDX = 0x70000001u;
constexpr std::uint32_t SHN_UNDEF = 0, SHN_ABS = 0xfff1;

constexpr std::uint8_t STT_OBJECT = 1, STT_FUNC = 2, STT_GNU_IFUNC = 10;
constexpr std::uint8_t STB_GLOBAL = 1, STB_WEAK = 2;
constexpr std::uint8_t ELF32_ST_BIND(std::uint8_t i) { return static_cast<std::uint8_t>(i >> 4); }
constexpr std::uint8_t ELF32_ST_TYPE(std::uint8_t i) { return static_cast<std::uint8_t>(i & 0xfu); }
constexpr std::uint32_t ELF32_R_SYM(std::uint32_t i) { return i >> 8; }
constexpr std::uint32_t ELF32_R_TYPE(std::uint32_t i) { return i & 0xffu; }

constexpr std::uint32_t R_ARM_NONE = 0, R_ARM_PC24 = 1, R_ARM_ABS32 = 2, R_ARM_REL32 = 3;
constexpr std::uint32_t R_ARM_COPY = 20, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22,
                        R_ARM_RELATIVE = 23;
constexpr std::uint32_t R_ARM_GOTOFF = 24, R_ARM_GOTPC = 25, R_ARM_GOT32 = 26;
constexpr std::uint32_t R_ARM_THM_CALL = 10, R_ARM_THM_JUMP24 = 31;
/**
 * The three TLS kinds and the IFUNC one, which every real system library needs and RetroBug does not
 * yet apply. The numbers are not recalled from a table: they are what `llvm-readelf -rW` prints for
 * `/system/lib/libc.so` on the SM-X710 (`0x11`, `0x13`, `0xa0`), and `rb_guest_libc_tests` asserts the
 * multiset, so a wrong constant here shows up as a red lane naming the count.
 *
 * The refusals are not benign. `R_ARM_IRELATIVE`'s target is meant to hold the *result of a guest call*,
 * and bionic writes all three TLS slots unconditionally during relocation, so a slot this loader skips
 * keeps whatever the mapping left in it — a wrong pointer, not a fault waiting to happen.
 */
constexpr std::uint32_t R_ARM_TLS_DTPMOD32 = 17, R_ARM_TLS_DTPOFF32 = 18, R_ARM_TLS_TPOFF32 = 19,
                        R_ARM_IRELATIVE = 160;

// Tag 11 is SYMENT: the size of *one* Elf32_Sym (16), not the length of the table. There is no
// DT_SYMSZ — a symbol table's extent is not in the dynamic section at all, which is easy to miss
// because every neighbour of DT_SYMTAB has an "SZ" partner (STRSZ, RELSZ, PLTRELSZ). Reading tag 11
// as a byte count bounds the export scan at one entry, and one entry is the null symbol, so the
// loader reports an image with no exports and no imports and still says it loaded cleanly.
constexpr std::int32_t DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_PLTGOT = 3, DT_HASH = 4,
                       DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_RELAENT = 9,
                       DT_STRSZ = 10, DT_SYMENT = 11, DT_INIT = 12, DT_FINI = 13, DT_SONAME = 14,
                       DT_REL = 17, DT_RELSZ = 18, DT_RELENT = 19, DT_PLTREL = 20, DT_JMPREL = 23,
                       DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FLAGS = 30,
                       DT_GNU_HASH = 0x6ffffef5, DT_VERNEED = 0x6ffffffe, DT_VERSYM = 0x6fffffff,
                       DT_RELACOUNT = 0x6ffffff9, DT_RELCOUNT = 0x6ffffffa;

constexpr std::uint32_t NT_GNU_BUILD_ID = 3;

}  // namespace rb::loader::elf
