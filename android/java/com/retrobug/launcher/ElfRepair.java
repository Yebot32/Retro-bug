// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Restores the section-header fields some app-protection wrappers zero in 32-bit libraries (SpongeBob Game Station's
// ATG/Presto libraries, Kakao's libsimpleencryption, a libgpg build): e_shentsize and e_shstrndx are 0 while the
// section table itself is intact. Android's loader refuses such a library for targetSdk >= 26 ("has unsupported
// e_shentsize"), and StubElfWriter.jniExportsOf cannot walk the table, so the library's Java natives would go unbound.
// Only the two header fields change, and only when the table checks out (in the file, section 0 SHT_NULL, a STRTAB
// holding ".shstrtab"); anything else is returned as is.
package com.retrobug.launcher;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

final class ElfRepair {
    private ElfRepair() {}

    static final int SHDR = 40;  // sizeof(Elf32_Shdr)

    /** The library with e_shentsize/e_shstrndx restored, in a new array; `elf` itself when nothing needs (or can) change. */
    static byte[] repair(byte[] elf) {
        if (elf.length < 52 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' || elf[4] != 1 || elf[5] != 1)
            return elf;
        ByteBuffer b = ByteBuffer.wrap(elf).order(ByteOrder.LITTLE_ENDIAN);
        long shoff = b.getInt(0x20) & 0xffffffffL;
        int shentsize = b.getShort(0x2e) & 0xffff, shnum = b.getShort(0x30) & 0xffff, shstrndx = b.getShort(0x32) & 0xffff;
        if (shoff == 0 || shnum == 0 || shoff + (long) shnum * SHDR > elf.length) return elf;
        for (int i = 0; i < SHDR; i++) if (elf[(int) shoff + i] != 0) return elf;  // section 0 must be SHT_NULL
        int strtab = shstrndx;
        if (!isShstrtab(b, elf, (int) shoff, shnum, strtab)) {
            strtab = -1;
            for (int i = shnum - 1; i > 0 && strtab < 0; i--) if (isShstrtab(b, elf, (int) shoff, shnum, i)) strtab = i;
            if (strtab < 0) return elf;
        }
        if (shentsize == SHDR && strtab == shstrndx) return elf;
        byte[] out = elf.clone();
        ByteBuffer o = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN);
        o.putShort(0x2e, (short) SHDR);
        o.putShort(0x32, (short) strtab);
        return out;
    }

    /** Section i is a SHT_STRTAB inside the file whose bytes name ".shstrtab" (as the section-name table does). */
    private static boolean isShstrtab(ByteBuffer b, byte[] elf, int shoff, int shnum, int i) {
        if (i <= 0 || i >= shnum) return false;
        int sh = shoff + i * SHDR;
        if (b.getInt(sh + 4) != 3) return false;  // SHT_STRTAB
        long off = b.getInt(sh + 16) & 0xffffffffL, size = b.getInt(sh + 20) & 0xffffffffL;
        if (size == 0 || off + size > elf.length) return false;
        return new String(elf, (int) off, (int) size, StandardCharsets.ISO_8859_1).contains(".shstrtab");
    }
}
