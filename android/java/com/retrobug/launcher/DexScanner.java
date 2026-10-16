// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Lists the method references in a DEX file ("Lpkg/Class;->name"), so the repack report can name the risky
// platform calls a game makes (ShimPlan.riskReport). Only the three id tables that spell a method reference
// are read — string_ids, type_ids, method_ids — following the layout in the DEX format documentation
// (source.android.com/docs/core/runtime/dex-format). A reference names the class used at the call site,
// so a call made through a subclass (`this.bindService(...)` in an Activity) appears under that subclass;
// the risk table matches such names by method name alone.
//
// Pure Java, no android.* import: tools/run_java_tests.sh runs it on the PC against a d8-built fixture.
package com.retrobug.launcher;

import java.io.IOException;
import java.util.Set;
import java.util.TreeSet;

final class DexScanner {
    private DexScanner() {}

    private static int u16(byte[] b, int o) { return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8; }
    private static int s32(byte[] b, int o) {
        return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8 | (b[o + 2] & 0xff) << 16 | (b[o + 3] & 0xff) << 24;
    }

    static Set<String> methodRefs(byte[] dex) throws IOException {
        if (dex.length < 0x70 || dex[0] != 'd' || dex[1] != 'e' || dex[2] != 'x' || dex[3] != '\n')
            throw new IOException("not a DEX file (no \"dex\\n\" magic)");
        try {
            int nStr = s32(dex, 0x38), strOff = s32(dex, 0x3c);
            int nType = s32(dex, 0x40), typeOff = s32(dex, 0x44);
            int nMeth = s32(dex, 0x58), methOff = s32(dex, 0x5c);
            Set<String> out = new TreeSet<>();
            for (int i = 0; i < nMeth; i++) {
                int m = methOff + 8 * i;
                int cls = u16(dex, m), name = s32(dex, m + 4);
                if (cls >= nType || name < 0 || name >= nStr) throw new IOException("method_id " + i + " points outside its tables");
                int desc = s32(dex, typeOff + 4 * cls);
                if (desc < 0 || desc >= nStr) throw new IOException("type_id " + cls + " points outside string_ids");
                out.add(string(dex, s32(dex, strOff + 4 * desc)) + "->" + string(dex, s32(dex, strOff + 4 * name)));
            }
            return out;
        } catch (IndexOutOfBoundsException ex) {
            throw new IOException("DEX malformed: " + ex, ex);
        }
    }

    /** A string_data_item: uleb128 UTF-16 length, then MUTF-8 bytes up to a NUL. */
    private static String string(byte[] b, int p) {
        while ((b[p++] & 0x80) != 0) { }           // skip the uleb128 length; the NUL ends the data
        StringBuilder sb = new StringBuilder();
        for (int c; (c = b[p] & 0xff) != 0; ) {
            if (c < 0x80) { sb.append((char) c); p++; }
            else if ((c & 0xe0) == 0xc0) { sb.append((char) ((c & 0x1f) << 6 | (b[p + 1] & 0x3f))); p += 2; }
            else { sb.append((char) ((c & 0x0f) << 12 | (b[p + 1] & 0x3f) << 6 | (b[p + 2] & 0x3f))); p += 3; }
        }
        return sb.toString();
    }
}
