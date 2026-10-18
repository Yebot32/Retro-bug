// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Lists the method references in a DEX file ("Lpkg/Class;->name"), so the repack report can name the risky
// platform calls a game makes (ShimPlan.riskReport), and the class hierarchy the library needs to tell a
// NativeActivity game from a Java one (C31). Only the id tables are read — string_ids, type_ids, method_ids,
// class_defs — following the layout in the DEX format documentation
// (source.android.com/docs/core/runtime/dex-format). A reference names the class used at the call site,
// so a call made through a subclass (`this.bindService(...)` in an Activity) appears under that subclass;
// the risk table matches such names by method name alone.
//
// Pure Java, no android.* import: tools/run_java_tests.sh runs it on the PC against a d8-built fixture.
package com.retrobug.launcher;

import java.io.IOException;
import java.util.HashMap;
import java.util.Map;
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

    /**
     * Every class this DEX defines, as its descriptor to its superclass's ({@code "Lpkg/Sub;" -> "Lpkg/Base;"}): what
     * the library walks to find out whether a launcher activity is a NativeActivity (C31). A class with no superclass —
     * java/lang/Object, and every interface — is left out, so a walk that reaches one simply stops.
     */
    static Map<String, String> superclasses(byte[] dex) throws IOException {
        if (dex.length < 0x70 || dex[0] != 'd' || dex[1] != 'e' || dex[2] != 'x' || dex[3] != '\n')
            throw new IOException("not a DEX file (no \"dex\\n\" magic)");
        try {
            Ids ids = new Ids(dex);
            int nClass = s32(dex, 0x60), classOff = s32(dex, 0x64);
            Map<String, String> out = new HashMap<>();
            for (int i = 0; i < nClass; i++) {
                int cd = classOff + 32 * i;   // class_def_item: class_idx, access_flags, superclass_idx, ...
                int sup = s32(dex, cd + 8);
                if (sup == -1) continue;      // NO_INDEX
                out.put(ids.descriptor(s32(dex, cd)), ids.descriptor(sup));
            }
            return out;
        } catch (IndexOutOfBoundsException ex) {
            throw new IOException("DEX malformed: " + ex, ex);
        }
    }

    /** The header fields a descriptor lookup needs, read once per DEX. */
    private static final class Ids {
        final byte[] dex;
        final int nStr, strOff, nType, typeOff;

        Ids(byte[] dex) {
            this.dex = dex;
            nStr = s32(dex, 0x38); strOff = s32(dex, 0x3c);
            nType = s32(dex, 0x40); typeOff = s32(dex, 0x44);
        }

        /** The descriptor of type_ids[{@code typeIdx}] ({@code "Lpkg/Class;"}), through its string_data_item. */
        String descriptor(int typeIdx) throws IOException {
            if (typeIdx < 0 || typeIdx >= nType) throw new IOException("type_id " + typeIdx + " outside its table");
            int desc = s32(dex, typeOff + 4 * typeIdx);
            if (desc < 0 || desc >= nStr) throw new IOException("type_id " + typeIdx + " points outside string_ids");
            return string(dex, s32(dex, strOff + 4 * desc));
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
