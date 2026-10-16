// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Writes the 64-bit stand-in for one 32-bit library: an AArch64 ET_DYN that exports the same `Java_*` names the
// 32-bit library exports (and always `JNI_OnLoad`), so ART's own native lookup binds them. Each export is 16 bytes,
//
//     adr x9, RbStubTable ; mov w10, #index ; ldr x16, <GOT slot> ; br x16
//
// landing in `rb_stub_capture` (librbguest.so, android/jni/stub_entry_aarch64.S) with the caller's x0-x7, d0-d7
// and stack untouched (x9, x10, x16 are AAPCS64 scratch). It saves them and calls `rb_stub_dispatch(table, index,
// &captured)`, which reads this file's RbStubTable (android/jni/runtime_boot.cpp keeps the C side):
//
//     u32 magic 'RBST' (0x54534252), u32 version 1, u32 lib_off, u32 count,
//     count x { u32 name_off, u32 shorty_off, u32 flags }   — offsets from the table's start; flags 1 = JNI_OnLoad,
//     then the NUL-terminated strings.                          2 = static (informational)
//
// Layout: one R+X PT_LOAD (header, phdrs, .hash, .dynsym, .dynstr, .rela.dyn, .text, the table), one RW PT_LOAD
// (.dynamic, .got — one 8-byte slot, R_AARCH64_GLOB_DAT against rb_stub_capture), PT_DYNAMIC, PT_GNU_STACK, and
// section headers (bionic's linker refuses an ELF without them for targetSdk >= 26). DT_NEEDED librbguest.so,
// DT_SONAME = the library's own file name. Segments are 64 KiB-aligned, so the stub loads on 4, 16 and 64 KiB pages.
//
// Pure Java, no android.* import: tools/run_java_tests.sh runs it on the PC.
package com.retrobug.launcher;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

final class StubElfWriter {
    private StubElfWriter() {}

    static final int MAGIC = 0x54534252, VERSION = 1, FLAG_ONLOAD = 1, FLAG_STATIC = 2;
    static final String CAPTURE = "rb_stub_capture", RUNTIME = "librbguest.so", ONLOAD = "JNI_OnLoad";
    static final int R_AARCH64_GLOB_DAT = 1025;
    private static final long ALIGN = 0x10000;

    /** One exported symbol: its name, the JNI shorty of what it stands for, and whether the method is static. */
    static final class Export {
        final String symbol, shorty;
        final boolean isStatic;
        Export(String symbol, String shorty, boolean isStatic) {
            this.symbol = symbol; this.shorty = shorty; this.isStatic = isStatic;
        }
    }

    /** The stub for {@code libName}: {@code JNI_OnLoad} first (index 0), then {@code exports} in order. */
    static byte[] write(String libName, List<Export> exports) {
        List<Export> all = new ArrayList<>();
        all.add(new Export(ONLOAD, "ILL", false));
        for (Export e : exports) if (!e.symbol.equals(ONLOAD)) all.add(e);
        final int n = all.size();

        // ---- .dynstr: "", needed, soname, capture, every export
        Strtab dynstr = new Strtab();
        int needed = dynstr.add(RUNTIME), soname = dynstr.add(libName), capture = dynstr.add(CAPTURE);
        int[] names = new int[n];
        for (int i = 0; i < n; i++) names[i] = dynstr.add(all.get(i).symbol);

        // ---- the table (in the R+X segment, after .text)
        Strtab tstr = new Strtab();
        int libOff = tstr.add(libName);
        int[] tname = new int[n], tshorty = new int[n];
        for (int i = 0; i < n; i++) { tname[i] = tstr.add(all.get(i).symbol); tshorty[i] = tstr.add(all.get(i).shorty); }
        final int tableHead = 16 + 12 * n;

        // ---- R+X layout
        final int ehdr = 64, phnum = 4, phdrs = ehdr, nsym = n + 2;  // null, capture, exports
        long hashOff = align(phdrs + 56L * phnum, 8);
        long hashSize = 4L * (2 + 1 + nsym);  // one bucket
        long dynsymOff = align(hashOff + hashSize, 8);
        long dynstrOff = dynsymOff + 24L * nsym;
        long relaOff = align(dynstrOff + dynstr.size(), 8);
        long textOff = align(relaOff + 24, 16);
        long tableOff = textOff + 16L * n;
        long rxEnd = tableOff + tableHead + tstr.size();

        // ---- RW layout: congruent with its file offset modulo ALIGN, on the next ALIGN boundary
        long rwOff = align(rxEnd, 16);
        long rwVaddr = align(rxEnd, ALIGN) + (rwOff % ALIGN);
        final int ndyn = 13;
        long dynOff = rwOff, dynVaddr = rwVaddr, dynSize = 16L * ndyn;
        long gotOff = dynOff + dynSize, gotVaddr = dynVaddr + dynSize;
        long rwEnd = gotOff + 8;

        // ---- section header string table and headers, after the RW data
        Strtab shstr = new Strtab();
        int sHash = shstr.add(".hash"), sDynsym = shstr.add(".dynsym"), sDynstr = shstr.add(".dynstr"),
            sRela = shstr.add(".rela.dyn"), sText = shstr.add(".text"), sRodata = shstr.add(".rodata"),
            sDynamic = shstr.add(".dynamic"), sGot = shstr.add(".got"), sShstr = shstr.add(".shstrtab");
        long shstrOff = rwEnd;
        long shOff = align(shstrOff + shstr.size(), 8);
        final int shnum = 10;
        long fileSize = shOff + 64L * shnum;

        ByteBuffer b = ByteBuffer.allocate((int) fileSize).order(ByteOrder.LITTLE_ENDIAN);
        // ELF header
        b.put(new byte[] {0x7f, 'E', 'L', 'F', 2, 1, 1, 0});
        b.position(16);
        b.putShort((short) 3).putShort((short) 183).putInt(1);   // ET_DYN, EM_AARCH64, EV_CURRENT
        b.putLong(0).putLong(phdrs).putLong(shOff).putInt(0);     // entry, phoff, shoff, flags
        b.putShort((short) 64).putShort((short) 56).putShort((short) phnum);
        b.putShort((short) 64).putShort((short) shnum).putShort((short) 9);
        // program headers
        b.position(phdrs);
        phdr(b, 1, 5, 0, 0, rxEnd, rxEnd, ALIGN);                                    // PT_LOAD R+X
        phdr(b, 1, 6, rwOff, rwVaddr, rwEnd - rwOff, rwEnd - rwOff, ALIGN);          // PT_LOAD RW
        phdr(b, 2, 6, dynOff, dynVaddr, dynSize, dynSize, 8);                        // PT_DYNAMIC
        phdr(b, 0x6474e551, 6, 0, 0, 0, 0, 16);                                      // PT_GNU_STACK, not X
        // .hash: one bucket, every symbol on one chain (1 -> 2 -> ... -> nsym-1)
        b.position((int) hashOff);
        b.putInt(1).putInt(nsym).putInt(nsym > 1 ? 1 : 0);
        for (int i = 0; i < nsym; i++) b.putInt(i + 1 < nsym && i > 0 ? i + 1 : 0);
        // .dynsym
        b.position((int) dynsymOff);
        b.put(new byte[24]);
        sym(b, capture, 0x12, 0, 0, 0);                         // GLOBAL FUNC, undefined
        for (int i = 0; i < n; i++) sym(b, names[i], 0x12, 6, textOff + 16L * i, 16);  // in .text (section 6)
        // .dynstr
        b.position((int) dynstrOff);
        b.put(dynstr.bytes());
        // .rela.dyn: the GOT slot <- rb_stub_capture
        b.position((int) relaOff);
        b.putLong(gotVaddr).putLong(((long) 1 << 32) | R_AARCH64_GLOB_DAT).putLong(0);
        // .text
        b.position((int) textOff);
        for (int i = 0; i < n; i++) {
            long pc = textOff + 16L * i;
            b.putInt(adr(9, tableOff - pc));
            b.putInt(0x52800000 | (i & 0xffff) << 5 | 10);                      // movz w10, #i
            b.putInt(0x58000000 | (int) (((gotVaddr - (pc + 8)) >> 2) & 0x7ffff) << 5 | 16);  // ldr x16, got
            b.putInt(0xd61f0200);                                                // br x16
        }
        // the table
        b.position((int) tableOff);
        b.putInt(MAGIC).putInt(VERSION).putInt(tableHead + libOff).putInt(n);
        for (int i = 0; i < n; i++) {
            Export e = all.get(i);
            b.putInt(tableHead + tname[i]).putInt(tableHead + tshorty[i])
             .putInt((i == 0 ? FLAG_ONLOAD : 0) | (e.isStatic ? FLAG_STATIC : 0));
        }
        b.put(tstr.bytes());
        // .dynamic
        b.position((int) dynOff);
        dyn(b, 1, needed);            // DT_NEEDED
        dyn(b, 14, soname);           // DT_SONAME
        dyn(b, 4, hashOff);           // DT_HASH
        dyn(b, 5, dynstrOff);         // DT_STRTAB
        dyn(b, 6, dynsymOff);         // DT_SYMTAB
        dyn(b, 10, dynstr.size());    // DT_STRSZ
        dyn(b, 11, 24);               // DT_SYMENT
        dyn(b, 7, relaOff);           // DT_RELA
        dyn(b, 8, 24);                // DT_RELASZ
        dyn(b, 9, 24);                // DT_RELAENT
        dyn(b, 30, 8);                // DT_FLAGS = DF_BIND_NOW
        dyn(b, 0x6ffffffb, 1);        // DT_FLAGS_1 = DF_1_NOW
        dyn(b, 0, 0);                 // DT_NULL
        // .got stays 0 until the linker writes rb_stub_capture's address
        // .shstrtab and section headers
        b.position((int) shstrOff);
        b.put(shstr.bytes());
        b.position((int) shOff);
        b.put(new byte[64]);                                                                       // 0
        shdr(b, sHash, 5, 2, hashOff, hashOff, hashSize, 2, 0, 8, 4);                              // 1 .hash
        shdr(b, sDynsym, 11, 2, dynsymOff, dynsymOff, 24L * nsym, 3, 1, 8, 24);                     // 2 .dynsym
        shdr(b, sDynstr, 3, 2, dynstrOff, dynstrOff, dynstr.size(), 0, 0, 1, 0);                   // 3 .dynstr
        shdr(b, sRela, 4, 2, relaOff, relaOff, 24, 2, 0, 8, 24);                                   // 4 .rela.dyn
        shdr(b, sRodata, 1, 2, tableOff, tableOff, rxEnd - tableOff, 0, 0, 4, 0);                  // 5 .rodata
        shdr(b, sText, 1, 6, textOff, textOff, 16L * n, 0, 0, 16, 0);                              // 6 .text
        shdr(b, sDynamic, 6, 3, dynVaddr, dynOff, dynSize, 3, 0, 8, 16);                           // 7 .dynamic
        shdr(b, sGot, 1, 3, gotVaddr, gotOff, 8, 0, 0, 8, 8);                                      // 8 .got
        shdr(b, sShstr, 3, 0, 0, shstrOff, shstr.size(), 0, 0, 1, 0);                              // 9 .shstrtab
        return b.array();
    }

    /** The {@code Java_*} and {@code JNI_OnLoad} functions a 32-bit ARM ELF defines in its .dynsym. */
    static Set<String> jniExportsOf(byte[] elf) throws IOException {
        if (elf.length < 52 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' || elf[4] != 1 || elf[5] != 1)
            throw new IOException("not a 32-bit little-endian ELF");
        ByteBuffer b = ByteBuffer.wrap(elf).order(ByteOrder.LITTLE_ENDIAN);
        try {
            int shoff = b.getInt(0x20), shentsize = b.getShort(0x2e) & 0xffff, shnum = b.getShort(0x30) & 0xffff;
            Set<String> out = new TreeSet<>();
            for (int i = 0; i < shnum; i++) {
                int sh = shoff + shentsize * i;
                if (b.getInt(sh + 4) != 11) continue;  // SHT_DYNSYM
                int off = b.getInt(sh + 16), size = b.getInt(sh + 20), link = b.getInt(sh + 24);
                int strOff = b.getInt(shoff + shentsize * link + 16);
                for (int s = off + 16; s + 16 <= off + size; s += 16) {
                    int info = elf[s + 12] & 0xff, shndx = b.getShort(s + 14) & 0xffff;
                    if (shndx == 0 || (info & 0xf) != 2 || (info >> 4) == 0) continue;  // defined, FUNC, not LOCAL
                    String name = cstr(elf, strOff + b.getInt(s));
                    if (name.startsWith("Java_") || name.equals(ONLOAD)) out.add(name);
                }
                return out;
            }
            throw new IOException("no .dynsym section");
        } catch (IndexOutOfBoundsException ex) {
            throw new IOException("ELF malformed: " + ex, ex);
        }
    }

    // ------------------------------------------------------------------------------------------------ helpers

    private static long align(long v, long a) { return (v + a - 1) / a * a; }

    private static int adr(int rd, long delta) {
        int imm = (int) delta;
        return 0x10000000 | (imm & 3) << 29 | ((imm >> 2) & 0x7ffff) << 5 | rd;
    }

    private static void phdr(ByteBuffer b, int type, int flags, long off, long vaddr, long filesz, long memsz, long align) {
        b.putInt(type).putInt(flags).putLong(off).putLong(vaddr).putLong(vaddr).putLong(filesz).putLong(memsz).putLong(align);
    }

    private static void sym(ByteBuffer b, int name, int info, int shndx, long value, long size) {
        b.putInt(name).put((byte) info).put((byte) 0).putShort((short) shndx).putLong(value).putLong(size);
    }

    private static void dyn(ByteBuffer b, long tag, long val) { b.putLong(tag).putLong(val); }

    private static void shdr(ByteBuffer b, int name, int type, long flags, long addr, long off, long size, int link,
                             int info, long align, long entsize) {
        b.putInt(name).putInt(type).putLong(flags).putLong(addr).putLong(off).putLong(size).putInt(link).putInt(info)
         .putLong(align).putLong(entsize);
    }

    private static String cstr(byte[] b, int p) {
        int e = p;
        while (b[e] != 0) e++;
        return new String(b, p, e - p, StandardCharsets.UTF_8);
    }

    /** A string table: a leading NUL, then each string once, NUL-terminated. */
    private static final class Strtab {
        private final ByteArrayOutputStream out = new ByteArrayOutputStream();
        private final java.util.Map<String, Integer> at = new java.util.HashMap<>();
        Strtab() { out.write(0); }
        int add(String s) {
            Integer have = at.get(s);
            if (have != null) return have;
            int off = out.size();
            byte[] bytes = s.getBytes(StandardCharsets.UTF_8);
            out.write(bytes, 0, bytes.length);
            out.write(0);
            at.put(s, off);
            return off;
        }
        int size() { return out.size(); }
        byte[] bytes() { return out.toByteArray(); }
    }
}
