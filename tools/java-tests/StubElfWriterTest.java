// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for StubElfWriter: the stub's bytes read back with a small ELF64 reader (header, program headers,
// dynamic tags, the exports' 16-byte code decoded, the GOT relocation, the RbStubTable), then — when the NDK is
// present — `llvm-readelf` must list exactly the exports and the one relocation. And jniExportsOf over a 32-bit
// library the NDK builds here.
//
// args: <scratch dir> [<llvm-readelf>] [<a 32-bit ARM .so exporting Java_rb_Fixture_* and JNI_OnLoad>]
package com.retrobug.launcher;

import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

public class StubElfWriterTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static String cstr(byte[] b, int p) {
        int e = p;
        while (b[e] != 0) e++;
        return new String(b, p, e - p, StandardCharsets.UTF_8);
    }

    static final List<StubElfWriter.Export> EXPORTS = Arrays.asList(
        new StubElfWriter.Export("Java_rb_Fixture_mix", "JIJFDIJFDIIIIFFDDFD", true),
        new StubElfWriter.Export("Java_rb_Fixture_echoObject", "LL", false),
        new StubElfWriter.Export("Java_rb_Fixture_half", "DF", true));

    static void layout(byte[] elf) {
        ByteBuffer b = ByteBuffer.wrap(elf).order(ByteOrder.LITTLE_ENDIAN);
        check(elf[0] == 0x7f && elf[1] == 'E' && elf[4] == 2 && elf[5] == 1, "ELF64 little-endian");
        check(b.getShort(16) == 3 && b.getShort(18) == 183, "ET_DYN EM_AARCH64");
        int phoff = (int) b.getLong(32), phnum = b.getShort(56);
        check(b.getLong(40) != 0 && b.getShort(60) > 0 && b.getShort(62) != 0, "section headers present");
        long rxVaddr = -1, rxEnd = 0, rwVaddr = -1, rwOff = 0, rwEnd = 0, dynOff = -1;
        int loads = 0;
        for (int i = 0; i < phnum; i++) {
            int p = phoff + 56 * i, type = b.getInt(p), flags = b.getInt(p + 4);
            long off = b.getLong(p + 8), va = b.getLong(p + 16), fsz = b.getLong(p + 32), align = b.getLong(p + 48);
            if (type == 1) {
                loads++;
                check(align >= 0x4000 && off % align == va % align, "PT_LOAD congruent and >= 16 KiB aligned");
                if (flags == 5) { rxVaddr = va; rxEnd = va + fsz; }
                if (flags == 6) { rwVaddr = va; rwOff = off; rwEnd = va + fsz; }
            }
            if (type == 2) dynOff = off;
            if (type == 0x6474e551) check((flags & 1) == 0, "stack not executable");
        }
        check(loads == 2 && rxVaddr == 0 && rwVaddr > rxEnd, "one R+X and one RW PT_LOAD");
        check(dynOff >= 0, "PT_DYNAMIC");
        Map<Long, Long> dyn = new HashMap<>();
        List<Long> needed = new ArrayList<>();
        for (int p = (int) dynOff; ; p += 16) {
            long tag = b.getLong(p), val = b.getLong(p + 8);
            if (tag == 0) break;
            if (tag == 1) needed.add(val);
            dyn.put(tag, val);
        }
        int strtab = (int) (long) dyn.get(5L), symtab = (int) (long) dyn.get(6L);
        check(needed.size() == 1 && cstr(elf, strtab + (int) (long) needed.get(0)).equals("librbguest.so"), "DT_NEEDED librbguest.so");
        check(cstr(elf, strtab + (int) (long) dyn.get(14L)).equals("libfixture.so"), "DT_SONAME is the library name");
        check(dyn.containsKey(4L) && dyn.get(11L) == 24 && dyn.get(9L) == 24, "DT_HASH, DT_SYMENT, DT_RELAENT");
        // the one relocation: GLOB_DAT on the GOT slot, against rb_stub_capture
        int rela = (int) (long) dyn.get(7L);
        check(dyn.get(8L) == 24, "one relocation");
        long gotVa = b.getLong(rela), info = b.getLong(rela + 8);
        int relSym = (int) (info >>> 32);
        check((info & 0xffffffffL) == StubElfWriter.R_AARCH64_GLOB_DAT, "R_AARCH64_GLOB_DAT");
        check(cstr(elf, strtab + b.getInt(symtab + 24 * relSym)).equals("rb_stub_capture"), "the GOT slot names rb_stub_capture");
        check(b.getShort(symtab + 24 * relSym + 6) == 0, "rb_stub_capture undefined");
        check(gotVa >= rwVaddr && gotVa + 8 <= rwEnd, "GOT slot inside the RW segment");
        // the symbols: JNI_OnLoad then the exports, each 16 bytes of code in R+X decoding to the dispatch sequence
        int nsym = b.getInt((int) (long) dyn.get(4L) + 4);
        check(nsym == EXPORTS.size() + 3, "symbols: null, capture, JNI_OnLoad, exports — " + nsym);
        Map<String, Long> at = new HashMap<>();
        for (int i = 2; i < nsym; i++) {
            int s = symtab + 24 * i;
            String name = cstr(elf, strtab + b.getInt(s));
            long value = b.getLong(s + 8), size = b.getLong(s + 16);
            check((elf[s + 4] & 0xff) == 0x12 && b.getShort(s + 6) != 0, name + ": defined GLOBAL FUNC");
            check(size == 16 && value >= rxVaddr && value + 16 <= rxEnd, name + ": 16 bytes inside R+X");
            at.put(name, value);
            int pc = (int) value;
            int adr = b.getInt(pc), mov = b.getInt(pc + 4), ldr = b.getInt(pc + 8), br = b.getInt(pc + 12);
            check((adr & 0x9f00001f) == 0x10000009, name + ": adr x9");
            int immAdr = ((adr >> 5) & 0x7ffff) << 2 | ((adr >>> 29) & 3);
            immAdr = (immAdr << 11) >> 11;
            int table = pc + immAdr;
            check(b.getInt(table) == StubElfWriter.MAGIC && b.getInt(table + 4) == 1, name + ": adr points at the RBST table");
            check((mov & 0xffe0001f) == 0x5280000a && ((mov >> 5) & 0xffff) == i - 2, name + ": mov w10, #" + (i - 2));
            check((ldr & 0xff00001f) == 0x58000010, name + ": ldr x16, literal");
            int imm19 = ((ldr >> 5) & 0x7ffff) << 13 >> 13;
            check(pc + 8 + 4L * imm19 == gotVa, name + ": ldr reads the GOT slot");
            check(br == 0xd61f0200, name + ": br x16");
            // the table entry names this symbol and its shorty
            int e = table + 16 + 12 * (i - 2);
            check(cstr(elf, table + b.getInt(e)).equals(name), name + ": table entry " + (i - 2) + " names it");
            String shorty = cstr(elf, table + b.getInt(e + 4));
            int flags = b.getInt(e + 8);
            if (i == 2) {
                check(name.equals("JNI_OnLoad") && flags == StubElfWriter.FLAG_ONLOAD, "export 0 is JNI_OnLoad, flag 1");
                check(cstr(elf, table + b.getInt(table + 8)).equals("libfixture.so") && b.getInt(table + 12) == nsym - 2,
                      "table lib name and count");
            } else {
                StubElfWriter.Export x = EXPORTS.get(i - 3);
                check(name.equals(x.symbol) && shorty.equals(x.shorty), name + ": shorty " + shorty);
                check(((flags & StubElfWriter.FLAG_STATIC) != 0) == x.isStatic, name + ": static flag");
            }
        }
        check(at.keySet().size() == EXPORTS.size() + 1, "each export once");
    }

    static String run(String... cmd) throws Exception {
        Process p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
        byte[] out = p.getInputStream().readAllBytes();
        p.waitFor();
        return new String(out, StandardCharsets.UTF_8);
    }

    public static void main(String[] a) throws Exception {
        byte[] elf = StubElfWriter.write("libfixture.so", EXPORTS);
        layout(elf);
        File out = new File(a[0], "libfixture.so");
        Files.write(out.toPath(), elf);
        if (a.length > 1 && new File(a[1]).isFile()) {
            String r = run(a[1], "-d", "-s", "--dyn-relocations", "-W", out.getPath());
            check(!r.contains("warning") && !r.contains("error"), "llvm-readelf reads it cleanly:\n" + r);
            for (StubElfWriter.Export x : EXPORTS) check(r.contains(" " + x.symbol + "\n") || r.contains(" " + x.symbol + "\r"), "readelf lists " + x.symbol);
            check(r.contains("R_AARCH64_GLOB_DAT") && r.contains("rb_stub_capture"), "readelf shows the GOT relocation");
            check(r.contains("(SONAME)") && r.contains("[libfixture.so]") && r.contains("[librbguest.so]"), "readelf dynamic tags");
        } else {
            System.out.println("StubElfWriterTest: no llvm-readelf — readelf rows skipped");
        }
        if (a.length > 2 && new File(a[2]).isFile()) {
            Set<String> ex = StubElfWriter.jniExportsOf(Files.readAllBytes(new File(a[2]).toPath()));
            check(ex.contains("Java_rb_Fixture_mix") && ex.contains("Java_rb_Fixture_half"), "jniExportsOf: " + ex);
            check(!ex.contains("fx_reflect"), "jniExportsOf lists only Java_* and JNI_OnLoad");
        }
        try { StubElfWriter.jniExportsOf(elf); check(false, "a 64-bit ELF accepted as 32-bit"); }
        catch (java.io.IOException e) { check(true, "64-bit refused"); }
        System.out.println("StubElfWriterTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
