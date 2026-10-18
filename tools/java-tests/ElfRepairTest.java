// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for ElfRepair: app-protection wrappers (SpongeBob Game Station's "Presto"/ATG libraries) zero
// e_shentsize and e_shstrndx in otherwise intact 32-bit libraries. Android refuses such a library for targetSdk >= 26
// ("has unsupported e_shentsize"), and RetroBug's stub writer cannot find its JNI exports. The repair restores the two
// fields when the section table itself is intact, and leaves everything else alone.
package com.retrobug.launcher;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

public class ElfRepairTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    /** A 32-bit ARM ELF: header, a .shstrtab and a .dynstr, then 4 section headers (NULL, .dynsym, .dynstr, .shstrtab). */
    static byte[] elf() {
        byte[] shstr = "\0.dynsym\0.dynstr\0.shstrtab\0".getBytes(StandardCharsets.US_ASCII);
        byte[] dynstr = "\0libx.so\0".getBytes(StandardCharsets.US_ASCII);
        int strOff = 52, dynOff = strOff + shstr.length, symOff = dynOff + dynstr.length, shoff = (symOff + 16 + 3) & ~3;
        ByteBuffer b = ByteBuffer.allocate(shoff + 4 * 40).order(ByteOrder.LITTLE_ENDIAN);
        b.put(new byte[] {0x7f, 'E', 'L', 'F', 1, 1, 1, 0});
        b.putShort(16, (short) 3).putShort(18, (short) 40).putInt(20, 1);
        b.putInt(32, shoff).putShort(40, (short) 52).putShort(42, (short) 32).putShort(46, (short) 40);
        b.putShort(48, (short) 4).putShort(50, (short) 3);
        b.position(strOff); b.put(shstr); b.put(dynstr);
        int sh = shoff + 40;  // [1] .dynsym
        b.putInt(sh, 1).putInt(sh + 4, 11).putInt(sh + 16, symOff).putInt(sh + 20, 16).putInt(sh + 24, 2).putInt(sh + 36, 16);
        sh += 40;             // [2] .dynstr
        b.putInt(sh, 9).putInt(sh + 4, 3).putInt(sh + 16, dynOff).putInt(sh + 20, dynstr.length);
        sh += 40;             // [3] .shstrtab
        b.putInt(sh, 17).putInt(sh + 4, 3).putInt(sh + 16, strOff).putInt(sh + 20, shstr.length);
        return b.array();
    }

    static int u16(byte[] e, int o) { return (e[o] & 0xff) | (e[o + 1] & 0xff) << 8; }

    public static void main(String[] a) {
        byte[] good = elf();
        check(ElfRepair.repair(good) == good, "an intact library is returned as is");

        byte[] broken = good.clone();
        broken[46] = 0; broken[47] = 0;  // e_shentsize = 0
        broken[50] = 0; broken[51] = 0;  // e_shstrndx = 0
        byte[] fixed = ElfRepair.repair(broken);
        check(fixed != broken, "a protection-zeroed header is repaired into a new array");
        check(u16(fixed, 46) == 40, "e_shentsize restored to 40");
        check(u16(fixed, 50) == 3, "e_shstrndx points at the STRTAB named .shstrtab");
        check(Arrays.equals(fixed, good), "nothing else changes");
        check(broken[46] == 0, "the input is not modified");

        byte[] noTable = good.clone();
        noTable[46] = 0;
        ByteBuffer.wrap(noTable).order(ByteOrder.LITTLE_ENDIAN).putInt(32, noTable.length + 1000);  // e_shoff past the end
        check(ElfRepair.repair(noTable) == noTable, "a section table outside the file is left alone");

        byte[] notNull = broken.clone();
        int shoff = ByteBuffer.wrap(notNull).order(ByteOrder.LITTLE_ENDIAN).getInt(32);
        notNull[shoff + 4] = 1;  // section 0 is not SHT_NULL: not a real table
        check(ElfRepair.repair(notNull) == notNull, "a table whose section 0 is not NULL is left alone");

        check(ElfRepair.repair(new byte[] {1, 2, 3}).length == 3, "not an ELF: as is");

        // The stub writer can read JNI exports again once repaired (it walks the section table).
        try {
            StubElfWriter.jniExportsOf(fixed);
            check(true, "jniExportsOf reads the repaired library");
        } catch (Exception e) {
            check(false, "jniExportsOf reads the repaired library: " + e);
        }

        System.out.println("ElfRepairTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
