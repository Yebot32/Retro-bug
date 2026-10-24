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

    /** True when some string in the pool contains {@code part} (an authority a game names by literal). */
    static boolean containsStringPart(byte[] dex, String part) throws IOException {
        if (dex.length < 0x70 || dex[0] != 'd' || dex[1] != 'e' || dex[2] != 'x' || dex[3] != '\n')
            throw new IOException("not a DEX file (no \"dex\\n\" magic)");
        try {
            int nStr = s32(dex, 0x38), strOff = s32(dex, 0x3c);
            for (int i = 0; i < nStr; i++) {
                int data = s32(dex, strOff + 4 * i);
                if (data < 0 || data >= dex.length) throw new IOException("string_id " + i + " points outside the file");
                if (string(dex, data).contains(part)) return true;
            }
            return false;
        } catch (IndexOutOfBoundsException ex) {
            throw new IOException("DEX malformed: " + ex, ex);
        }
    }

    /**
     * Rewrites every string in the pool that contains {@code find}, substituting {@code replace}, and returns how
     * many were rewritten. The string table is length-prefixed, so this works in place only: a rewritten string
     * that would not fit the record it lives in is left alone (the count then does not include it). ASCII text
     * throughout — an authority is a URI host, never anything else.
     */
    static int replaceStringPart(byte[] dex, String find, String replace) throws IOException {
        if (!canReplaceInPlace(dex, find, replace)) return 0;
        int replaced = 0;
        int nStr = s32(dex, 0x38), strOff = s32(dex, 0x3c);
        for (int i = 0; i < nStr; i++) {
            int data = s32(dex, strOff + 4 * i);
            String s = string(dex, data);
            if (!s.contains(find)) continue;
            int lenBytes = ulebSize(dex, data);
            byte[] text = mutf8(s.replace(find, replace));
            System.arraycopy(text, 0, dex, data + lenBytes, text.length);   // same length: the record is reused
            replaced++;
        }
        return replaced;
    }

    /**
     * True when every string containing {@code find} can take {@code replace} in place: the result must be the same
     * byte length (the string table is length-prefixed and its records are packed; a shorter one would leave bytes
     * the verifier rejects, a longer one would run into the next record) and must not disturb the table's ordering —
     * {@code string_ids} are sorted by content and ART verifies that.
     */
    static boolean canReplaceInPlace(byte[] dex, String find, String replace) throws IOException {
        if (dex.length < 0x70 || dex[0] != 'd' || dex[1] != 'e' || dex[2] != 'x' || dex[3] != '\n')
            throw new IOException("not a DEX file (no \"dex\\n\" magic)");
        if (find.isEmpty()) return false;
        try {
            int nStr = s32(dex, 0x38), strOff = s32(dex, 0x3c);
            boolean any = false;
            for (int i = 0; i < nStr; i++) {
                int data = s32(dex, strOff + 4 * i);
                if (data < 0 || data >= dex.length) throw new IOException("string_id " + i + " points outside the file");
                String s = string(dex, data);
                if (!s.contains(find)) continue;
                String neu = s.replace(find, replace);
                byte[] text = mutf8(neu);
                if (text == null || text.length != s.length()) return false;
                if (i > 0 && string(dex, s32(dex, strOff + 4 * (i - 1))).compareTo(neu) >= 0) return false;
                if (i + 1 < nStr && string(dex, s32(dex, strOff + 4 * (i + 1))).compareTo(neu) <= 0) return false;
                any = true;
            }
            return any;
        } catch (IndexOutOfBoundsException ex) {
            throw new IOException("DEX malformed: " + ex, ex);
        }
    }

    /** How many bytes the uleb128 prefix at {@code p} takes. */
    private static int ulebSize(byte[] b, int p) {
        int n = 0;
        do { n++; } while ((b[p + n - 1] & 0x80) != 0);
        return n;
    }

    private static int ulebSizeFor(int units) {
        int n = 1;
        while ((units >>>= 7) != 0) n++;
        return n;
    }

    /** MUTF-8 (the DEX encoding); null for a string this writer does not handle (a lone surrogate). */
    private static byte[] mutf8(String s) {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream(s.length() + 8);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c != 0 && c < 0x80) out.write(c);
            else if (c < 0x800) { out.write(0xc0 | (c >> 6)); out.write(0x80 | (c & 0x3f)); }
            else if (Character.isSurrogate(c)) return null;
            else { out.write(0xe0 | (c >> 12)); out.write(0x80 | ((c >> 6) & 0x3f)); out.write(0x80 | (c & 0x3f)); }
        }
        return out.toByteArray();
    }

    /**
     * Recomputes the header's SHA-1 signature (bytes 12..31, over the rest of the file from 32) and Adler-32
     * checksum (bytes 8..11, over everything from the signature on) after an in-place edit. ART refuses a DEX
     * whose checksum no longer matches ("Bad checksum"), so every {@link #replaceStringPart} caller needs this.
     */
    static void refreshDigests(byte[] dex) throws IOException {
        if (dex.length < 32 || dex[0] != 'd' || dex[1] != 'e' || dex[2] != 'x' || dex[3] != '\n')
            throw new IOException("not a DEX file (no \"dex\\n\" magic)");
        try {
            java.security.MessageDigest sha = java.security.MessageDigest.getInstance("SHA-1");
            sha.update(dex, 32, dex.length - 32);
            byte[] digest = sha.digest();
            System.arraycopy(digest, 0, dex, 12, 20);
        } catch (java.security.NoSuchAlgorithmException ex) {
            throw new IOException("SHA-1 is unavailable: " + ex, ex);
        }
        java.util.zip.Adler32 adler = new java.util.zip.Adler32();
        adler.update(dex, 12, dex.length - 12);
        int sum = (int) adler.getValue();
        dex[8] = (byte) sum;
        dex[9] = (byte) (sum >> 8);
        dex[10] = (byte) (sum >> 16);
        dex[11] = (byte) (sum >> 24);
    }
}
