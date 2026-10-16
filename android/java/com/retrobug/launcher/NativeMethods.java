// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Every `native` method a DEX file declares, under the names ART's JNI lookup tries for it: the short name
// (`Java_<class>_<method>`) and the long name (short + `__` + the mangled parameter descriptors), mangled as the
// JNI specification's "Resolving Native Method Names" says (`_` -> `_1`, `;` -> `_2`, `[` -> `_3`, `/` -> `_`,
// anything not an ASCII letter or digit -> `_0xxxx`). The repackager keeps the names the 32-bit library actually
// exports and writes a stub exporting the same ones (StubElfWriter), so ART's own lookup finds them.
//
// Read tables (DEX format, source.android.com/docs/core/runtime/dex-format): string_ids, type_ids, proto_ids,
// method_ids, class_defs and each class_data_item's encoded methods (access flag 0x100 = native, 0x8 = static).
//
// Pure Java, no android.* import: tools/run_java_tests.sh runs it on the PC.
package com.retrobug.launcher;

import java.io.IOException;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

final class NativeMethods {
    private NativeMethods() {}

    static final int ACC_STATIC = 0x8, ACC_NATIVE = 0x100;

    /** One native method: its JNI descriptor ("(IJ)V") and whether it is static. */
    static final class Native {
        final String descriptor;
        final boolean isStatic;
        Native(String descriptor, boolean isStatic) { this.descriptor = descriptor; this.isStatic = isStatic; }
    }

    private static int u16(byte[] b, int o) { return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8; }
    private static int s32(byte[] b, int o) {
        return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8 | (b[o + 2] & 0xff) << 16 | (b[o + 3] & 0xff) << 24;
    }

    /** Mangled JNI name -> JNI descriptor, for every native method (see {@link #natives}). */
    static Map<String, String> of(byte[] dex) throws IOException {
        Map<String, String> out = new TreeMap<>();
        for (Map.Entry<String, Native> e : natives(dex).entrySet()) out.put(e.getKey(), e.getValue().descriptor);
        return out;
    }

    /**
     * Mangled JNI name -> method, for every native method of every class. Each gets its long name; its short name
     * too unless another native of the same class shares the method name (an overload: ART would bind the short
     * name to whichever it resolves first, so only the long names say which is which).
     */
    static Map<String, Native> natives(byte[] dex) throws IOException {
        if (dex.length < 0x70 || dex[0] != 'd' || dex[1] != 'e' || dex[2] != 'x' || dex[3] != '\n')
            throw new IOException("not a DEX file (no \"dex\\n\" magic)");
        try {
            int strOff = s32(dex, 0x3c), typeOff = s32(dex, 0x44), protoOff = s32(dex, 0x4c), methOff = s32(dex, 0x5c);
            int nClass = s32(dex, 0x60), classOff = s32(dex, 0x64);
            Map<String, Native> out = new TreeMap<>();
            for (int c = 0; c < nClass; c++) {
                int def = classOff + 32 * c;
                int dataOff = s32(dex, def + 24);
                if (dataOff == 0) continue;
                int[] p = {dataOff};
                int nStaticF = uleb(dex, p), nInstF = uleb(dex, p), nDirect = uleb(dex, p), nVirtual = uleb(dex, p);
                for (int i = 0; i < 2 * (nStaticF + nInstF); i++) uleb(dex, p);  // field_idx_diff, access_flags
                Map<String, Native> byShort = new HashMap<>();
                Set<String> overloaded = new HashSet<>();
                for (int list = 0; list < 2; list++) {
                    int idx = 0;
                    for (int i = 0, n = list == 0 ? nDirect : nVirtual; i < n; i++) {
                        idx += uleb(dex, p);
                        int access = uleb(dex, p);
                        uleb(dex, p);  // code_off
                        if ((access & ACC_NATIVE) == 0) continue;
                        int m = methOff + 8 * idx;
                        String cls = type(dex, strOff, typeOff, u16(dex, m));
                        String name = string(dex, s32(dex, strOff + 4 * s32(dex, m + 4)));
                        int proto = protoOff + 12 * u16(dex, m + 2);
                        StringBuilder params = new StringBuilder();
                        int plist = s32(dex, proto + 8);
                        if (plist != 0) {
                            for (int k = 0, size = s32(dex, plist); k < size; k++)
                                params.append(type(dex, strOff, typeOff, u16(dex, plist + 4 + 2 * k)));
                        }
                        String desc = "(" + params + ")" + type(dex, strOff, typeOff, s32(dex, proto + 4));
                        Native nat = new Native(desc, (access & ACC_STATIC) != 0);
                        String shortName = "Java_" + mangle(cls.substring(1, cls.length() - 1)) + "_" + mangle(name);
                        out.put(shortName + "__" + mangle(params.toString()), nat);
                        if (byShort.put(shortName, nat) != null) overloaded.add(shortName);
                    }
                }
                for (Map.Entry<String, Native> e : byShort.entrySet())
                    if (!overloaded.contains(e.getKey())) out.put(e.getKey(), e.getValue());
            }
            return out;
        } catch (IndexOutOfBoundsException ex) {
            throw new IOException("DEX malformed: " + ex, ex);
        }
    }

    /** The JNI shorty of a descriptor: return then parameters, every reference or array as {@code L}. */
    static String shortyOf(String descriptor) {
        StringBuilder params = new StringBuilder();
        int i = 1;
        for (; descriptor.charAt(i) != ')'; i = next(descriptor, i)) params.append(kind(descriptor.charAt(i)));
        return kind(descriptor.charAt(i + 1)) + params.toString();
    }

    private static char kind(char c) { return c == '[' ? 'L' : c; }

    /** The index after the one type that starts at {@code i}. */
    private static int next(String d, int i) {
        while (d.charAt(i) == '[') i++;
        return d.charAt(i) == 'L' ? d.indexOf(';', i) + 1 : i + 1;
    }

    /** JNI name mangling of a class name (with '/') or method name or parameter descriptors. */
    static String mangle(String s) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '/') sb.append('_');
            else if (c == '_') sb.append("_1");
            else if (c == ';') sb.append("_2");
            else if (c == '[') sb.append("_3");
            else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) sb.append(c);
            else sb.append(String.format("_0%04x", (int) c));
        }
        return sb.toString();
    }

    private static String type(byte[] dex, int strOff, int typeOff, int t) {
        return string(dex, s32(dex, strOff + 4 * s32(dex, typeOff + 4 * t)));
    }

    private static int uleb(byte[] b, int[] p) {
        int v = 0;
        for (int shift = 0; ; shift += 7) {
            int x = b[p[0]++] & 0xff;
            v |= (x & 0x7f) << shift;
            if ((x & 0x80) == 0) return v;
        }
    }

    /** A string_data_item: uleb128 UTF-16 length, then MUTF-8 bytes up to a NUL. */
    private static String string(byte[] b, int p) {
        while ((b[p++] & 0x80) != 0) { }
        StringBuilder sb = new StringBuilder();
        for (int c; (c = b[p] & 0xff) != 0; ) {
            if (c < 0x80) { sb.append((char) c); p++; }
            else if ((c & 0xe0) == 0xc0) { sb.append((char) ((c & 0x1f) << 6 | (b[p + 1] & 0x3f))); p += 2; }
            else { sb.append((char) ((c & 0x0f) << 12 | (b[p + 1] & 0x3f) << 6 | (b[p + 2] & 0x3f))); p += 3; }
        }
        return sb.toString();
    }
}
