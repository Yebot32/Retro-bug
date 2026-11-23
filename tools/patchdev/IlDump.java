// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A small ECMA-335 (.NET) assembly reader for writing game patches: Unity/Mono games keep their logic as IL in
// Assembly-CSharp.dll. No decompiler is needed to find a check and patch it — the method names, the string literals
// each method loads, the methods it calls, and its IL with file offsets are enough. Read-only; prints to stdout.
//
//   java IlDump.java <dll> strings [regex]          user strings (the #US heap) matching regex
//   java IlDump.java <dll> uses <regex>             methods whose ldstr literals or call targets match regex
//   java IlDump.java <dll> methods <regex>          methods whose Type::Name matches regex (with body file offset)
//   java IlDump.java <dll> il <Type::Name regex>    IL of matching methods: file offset, opcode, operand
//
// Supports what Unity 4/5-era Mono assemblies use: PE32, #~ tables up to MemberRef, tiny and fat method headers.
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;

public class IlDump {
    final byte[] b;
    final ByteBuffer bb;
    int[][] sections;           // {virtualAddress, virtualSize, rawPointer}
    int strings, us, blob;      // heap file offsets
    int usSize;
    int[] rows = new int[64];
    int[] tableOff = new int[64];
    int[] rowSize = new int[64];
    int strIdx, guidIdx, blobIdx;

    IlDump(byte[] b) throws IOException {
        this.b = b;
        bb = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN);
        int pe = bb.getInt(0x3c);
        if (bb.getInt(pe) != 0x4550) throw new IOException("not a PE file");
        int nsec = bb.getShort(pe + 6) & 0xffff;
        int optSize = bb.getShort(pe + 20) & 0xffff;
        int opt = pe + 24;
        boolean pe32plus = (bb.getShort(opt) & 0xffff) == 0x20b;
        int dirs = opt + (pe32plus ? 112 : 96);
        int cliRva = bb.getInt(dirs + 14 * 8);
        sections = new int[nsec][];
        int sec = opt + optSize;
        for (int i = 0; i < nsec; i++) {
            int s = sec + i * 40;
            sections[i] = new int[] {bb.getInt(s + 12), bb.getInt(s + 8), bb.getInt(s + 20)};
        }
        int cli = off(cliRva);
        int md = off(bb.getInt(cli + 8));
        if (bb.getInt(md) != 0x424A5342) throw new IOException("no metadata");
        int vlen = bb.getInt(md + 12);
        int p = md + 16 + vlen + 2;
        int nstreams = bb.getShort(p) & 0xffff;
        p += 2;
        int tables = -1;
        for (int i = 0; i < nstreams; i++) {
            int o = bb.getInt(p), sz = bb.getInt(p + 4);
            p += 8;
            int e = p;
            while (b[e] != 0) e++;
            String name = new String(b, p, e - p, StandardCharsets.US_ASCII);
            p = (e + 4) & ~3;
            switch (name) {
                case "#~": case "#-": tables = md + o; break;
                case "#Strings": strings = md + o; break;
                case "#US": us = md + o; usSize = sz; break;
                case "#Blob": blob = md + o; break;
                default: break;
            }
        }
        int heap = b[tables + 6];
        strIdx = (heap & 1) != 0 ? 4 : 2;
        guidIdx = (heap & 2) != 0 ? 4 : 2;
        blobIdx = (heap & 4) != 0 ? 4 : 2;
        long valid = bb.getLong(tables + 8);
        int q = tables + 24;
        for (int t = 0; t < 64; t++) if ((valid >> t & 1) != 0) { rows[t] = bb.getInt(q); q += 4; }
        int[] order = new int[64];
        for (int t = 0; t < 64; t++) order[t] = t;
        for (int t = 0; t < 0x0B; t++) rowSize[t] = schema(t);
        // Only tables 0..0x0A are needed; the later ones are skipped by size, which needs their schemas too.
        for (int t = 0x0B; t < 0x2D; t++) rowSize[t] = schema(t);
        for (int t = 0; t < 0x2D; t++) { tableOff[t] = q; q += rows[t] * rowSize[t]; }
    }

    int off(int rva) {
        for (int[] s : sections) if (rva >= s[0] && rva < s[0] + Math.max(s[1], 1)) return rva - s[0] + s[2];
        throw new IllegalArgumentException("rva " + Integer.toHexString(rva));
    }

    int idx(int table) { return rows[table] < 0x10000 ? 2 : 4; }

    int coded(int bits, int... tables) {
        int max = 0;
        for (int t : tables) max = Math.max(max, rows[t]);
        return max < (1 << (16 - bits)) ? 2 : 4;
    }

    int typeDefOrRef() { return coded(2, 0x02, 0x01, 0x1B); }
    int hasConstant() { return coded(2, 0x04, 0x08, 0x17); }
    int hasCustomAttribute() {
        return coded(5, 0x06, 0x04, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x00, 0x0E, 0x17, 0x14, 0x11, 0x1A, 0x1B, 0x20, 0x23,
                0x26, 0x27, 0x28);
    }
    int customAttributeType() { return coded(3, 0x06, 0x0A); }
    int hasFieldMarshal() { return coded(1, 0x04, 0x08); }
    int hasDeclSecurity() { return coded(2, 0x02, 0x06, 0x20); }
    int memberRefParent() { return coded(3, 0x02, 0x01, 0x1A, 0x06, 0x1B); }
    int hasSemantics() { return coded(1, 0x14, 0x17); }
    int methodDefOrRef() { return coded(1, 0x06, 0x0A); }
    int memberForwarded() { return coded(1, 0x04, 0x06); }
    int implementation() { return coded(2, 0x26, 0x23, 0x27); }
    int resolutionScope() { return coded(2, 0x00, 0x1A, 0x23, 0x01); }
    int typeOrMethodDef() { return coded(1, 0x02, 0x06); }

    int schema(int t) {
        int s = strIdx, g = guidIdx, bl = blobIdx;
        switch (t) {
            case 0x00: return 2 + s + g + g + g;
            case 0x01: return resolutionScope() + s + s;
            case 0x02: return 4 + s + s + typeDefOrRef() + idx(0x04) + idx(0x06);
            case 0x03: return idx(0x04);
            case 0x04: return 2 + s + bl;
            case 0x05: return idx(0x06);
            case 0x06: return 4 + 2 + 2 + s + bl + idx(0x08);
            case 0x07: return idx(0x08);
            case 0x08: return 2 + 2 + s;
            case 0x09: return idx(0x02) + typeDefOrRef();
            case 0x0A: return memberRefParent() + s + bl;
            case 0x0B: return 2 + hasConstant() + bl;
            case 0x0C: return hasCustomAttribute() + customAttributeType() + bl;
            case 0x0D: return hasFieldMarshal() + bl;
            case 0x0E: return 2 + hasDeclSecurity() + bl;
            case 0x0F: return 2 + 4 + idx(0x02);
            case 0x10: return 4 + idx(0x04);
            case 0x11: return bl;
            case 0x12: return idx(0x02) + idx(0x14);
            case 0x13: return idx(0x14);
            case 0x14: return 2 + s + typeDefOrRef();
            case 0x15: return idx(0x02) + idx(0x17);
            case 0x16: return idx(0x17);
            case 0x17: return 2 + s + bl;
            case 0x18: return 2 + idx(0x06) + hasSemantics();
            case 0x19: return idx(0x02) + methodDefOrRef() + methodDefOrRef();
            case 0x1A: return s;
            case 0x1B: return bl;
            case 0x1C: return 2 + memberForwarded() + s + idx(0x1A);
            case 0x1D: return 4 + idx(0x04);
            case 0x1E: return 4 + 4;
            case 0x1F: return 4;
            case 0x20: return 4 + 2 + 2 + 2 + 2 + 4 + bl + s + s;
            case 0x21: return 4;
            case 0x22: return 4 + 4 + 4;
            case 0x23: return 2 + 2 + 2 + 2 + 4 + bl + s + s + bl;
            case 0x24: return 4 + idx(0x23);
            case 0x25: return 4 + 4 + 4 + idx(0x23);
            case 0x26: return 4 + s + bl;
            case 0x27: return 4 + 4 + s + s + implementation();
            case 0x28: return 4 + 4 + s + implementation();
            case 0x29: return idx(0x02) + idx(0x02);
            case 0x2A: return 2 + 2 + typeOrMethodDef() + s;
            case 0x2B: return methodDefOrRef() + bl;
            case 0x2C: return idx(0x2A) + typeDefOrRef();
            default: return 0;
        }
    }

    int rd(int at, int size) { return size == 2 ? bb.getShort(at) & 0xffff : bb.getInt(at); }

    String str(int i) {
        int p = strings + i, e = p;
        while (b[e] != 0) e++;
        return new String(b, p, e - p, StandardCharsets.UTF_8);
    }

    String usAt(int i) {
        int p = us + i;
        int len, first = b[p] & 0xff;
        if ((first & 0x80) == 0) { len = first; p += 1; }
        else if ((first & 0xC0) == 0x80) { len = ((first & 0x3f) << 8) | (b[p + 1] & 0xff); p += 2; }
        else { len = ((first & 0x1f) << 24) | ((b[p + 1] & 0xff) << 16) | ((b[p + 2] & 0xff) << 8) | (b[p + 3] & 0xff); p += 4; }
        int chars = len / 2;
        return new String(b, p, chars * 2, StandardCharsets.UTF_16LE);
    }

    // ---- types and methods

    String typeName(int typeDefRow) {  // 1-based
        int r = tableOff[0x02] + (typeDefRow - 1) * rowSize[0x02];
        String n = str(rd(r + 4, strIdx)), ns = str(rd(r + 4 + strIdx, strIdx));
        return ns.isEmpty() ? n : ns + "." + n;
    }

    int typeMethodList(int typeDefRow) {
        if (typeDefRow > rows[0x02]) return rows[0x06] + 1;
        int r = tableOff[0x02] + (typeDefRow - 1) * rowSize[0x02];
        int o = r + 4 + strIdx * 2 + typeDefOrRef() + idx(0x04);
        return rd(o, idx(0x06));
    }

    String[] methodOwner;
    void owners() {
        methodOwner = new String[rows[0x06] + 1];
        for (int t = 1; t <= rows[0x02]; t++) {
            int from = typeMethodList(t), to = typeMethodList(t + 1);
            String n = typeName(t);
            for (int m = from; m < to && m <= rows[0x06]; m++) methodOwner[m] = n;
        }
    }

    int methodRow(int m) { return tableOff[0x06] + (m - 1) * rowSize[0x06]; }
    int methodRva(int m) { return bb.getInt(methodRow(m)); }
    String methodName(int m) { return str(rd(methodRow(m) + 8, strIdx)); }
    String methodFull(int m) { return methodOwner[m] + "::" + methodName(m); }

    String memberRefName(int row) {
        int r = tableOff[0x0A] + (row - 1) * rowSize[0x0A];
        int parentSize = memberRefParent();
        int parent = rd(r, parentSize);
        String name = str(rd(r + parentSize, strIdx));
        int tag = parent & 7, i = parent >>> 3;
        String owner = "?";
        if (tag == 1 && i > 0) {  // TypeRef
            int tr = tableOff[0x01] + (i - 1) * rowSize[0x01] + resolutionScope();
            String n = str(rd(tr, strIdx)), ns = str(rd(tr + strIdx, strIdx));
            owner = ns.isEmpty() ? n : ns + "." + n;
        } else if (tag == 0 && i > 0) {
            owner = typeName(i);
        } else if (tag == 4) {
            owner = "<typespec>";
        }
        return owner + "::" + name;
    }

    String token(int tok) {
        int table = tok >>> 24, row = tok & 0xffffff;
        switch (table) {
            case 0x06: return row <= rows[0x06] ? methodFull(row) : "method#" + row;
            case 0x0A: return memberRefName(row);
            case 0x02: return typeName(row);
            case 0x70: return "\"" + usAt(row).replace("\n", "\\n") + "\"";
            case 0x04: {
                int r = tableOff[0x04] + (row - 1) * rowSize[0x04];
                return "field " + str(rd(r + 2, strIdx));
            }
            default: return String.format("tok %08x", tok);
        }
    }

    // ---- IL

    static final int NONE = 0, I8 = 1, I32 = 2, I64 = 3, R4 = 4, R8 = 5, TOK = 6, BR8 = 7, BR32 = 8, SWITCH = 9, U16 = 10;
    static final String[] NAMES = new String[256];
    static final int[] OPS = new int[256];
    static final String[] NAMES2 = new String[64];
    static final int[] OPS2 = new int[64];
    static void op(int c, String n, int k) { NAMES[c] = n; OPS[c] = k; }
    static void op2(int c, String n, int k) { NAMES2[c] = n; OPS2[c] = k; }
    static {
        String[] simple = {"nop", "break", "ldarg.0", "ldarg.1", "ldarg.2", "ldarg.3", "ldloc.0", "ldloc.1", "ldloc.2",
                "ldloc.3", "stloc.0", "stloc.1", "stloc.2", "stloc.3"};
        for (int i = 0; i < simple.length; i++) op(i, simple[i], NONE);
        op(0x0e, "ldarg.s", I8); op(0x0f, "ldarga.s", I8); op(0x10, "starg.s", I8); op(0x11, "ldloc.s", I8);
        op(0x12, "ldloca.s", I8); op(0x13, "stloc.s", I8); op(0x14, "ldnull", NONE);
        for (int i = 0; i <= 8; i++) op(0x15 + i, "ldc.i4." + (i == 0 ? "m1" : String.valueOf(i - 1)), NONE);
        op(0x1f, "ldc.i4.s", I8); op(0x20, "ldc.i4", I32); op(0x21, "ldc.i8", I64); op(0x22, "ldc.r4", R4);
        op(0x23, "ldc.r8", R8); op(0x25, "dup", NONE); op(0x26, "pop", NONE); op(0x27, "jmp", TOK); op(0x28, "call", TOK);
        op(0x29, "calli", TOK); op(0x2a, "ret", NONE);
        String[] br = {"br", "brfalse", "brtrue", "beq", "bge", "bgt", "ble", "blt", "bne.un", "bge.un", "bgt.un",
                "ble.un", "blt.un"};
        for (int i = 0; i < br.length; i++) { op(0x2b + i, br[i] + ".s", BR8); op(0x38 + i, br[i], BR32); }
        op(0x45, "switch", SWITCH);
        String[] simple2 = {"ldind.i1", "ldind.u1", "ldind.i2", "ldind.u2", "ldind.i4", "ldind.u4", "ldind.i8", "ldind.i",
                "ldind.r4", "ldind.r8", "ldind.ref", "stind.ref", "stind.i1", "stind.i2", "stind.i4", "stind.i8",
                "stind.r4", "stind.r8", "add", "sub", "mul", "div", "div.un", "rem", "rem.un", "and", "or", "xor", "shl",
                "shr", "shr.un", "neg", "not", "conv.i1", "conv.i2", "conv.i4", "conv.i8", "conv.r4", "conv.r8",
                "conv.u4", "conv.u8"};
        for (int i = 0; i < simple2.length; i++) op(0x46 + i, simple2[i], NONE);
        op(0x6f, "callvirt", TOK); op(0x70, "cpobj", TOK); op(0x71, "ldobj", TOK); op(0x72, "ldstr", TOK);
        op(0x73, "newobj", TOK); op(0x74, "castclass", TOK); op(0x75, "isinst", TOK); op(0x76, "conv.r.un", NONE);
        op(0x79, "unbox", TOK); op(0x7a, "throw", NONE); op(0x7b, "ldfld", TOK); op(0x7c, "ldflda", TOK);
        op(0x7d, "stfld", TOK); op(0x7e, "ldsfld", TOK); op(0x7f, "ldsflda", TOK); op(0x80, "stsfld", TOK);
        op(0x81, "stobj", TOK);
        for (int i = 0x82; i <= 0x8b; i++) op(i, "conv.ovf.x.un", NONE);
        op(0x8c, "box", TOK); op(0x8d, "newarr", TOK); op(0x8e, "ldlen", NONE); op(0x8f, "ldelema", TOK);
        for (int i = 0x90; i <= 0xa2; i++) op(i, "ldelem/stelem", NONE);
        op(0xa3, "ldelem", TOK); op(0xa4, "stelem", TOK); op(0xa5, "unbox.any", TOK);
        for (int i = 0xb3; i <= 0xba; i++) op(i, "conv.ovf.x", NONE);
        op(0xc2, "refanyval", TOK); op(0xc3, "ckfinite", NONE); op(0xc6, "mkrefany", TOK); op(0xd0, "ldtoken", TOK);
        for (int i = 0xd1; i <= 0xdc; i++) op(i, "conv/arith.ovf", NONE);
        op(0xdd, "leave", BR32); op(0xde, "leave.s", BR8); op(0xdf, "stind.i", NONE); op(0xe0, "conv.u", NONE);
        op2(0x00, "arglist", NONE); op2(0x01, "ceq", NONE); op2(0x02, "cgt", NONE); op2(0x03, "cgt.un", NONE);
        op2(0x04, "clt", NONE); op2(0x05, "clt.un", NONE); op2(0x06, "ldftn", TOK); op2(0x07, "ldvirtftn", TOK);
        op2(0x09, "ldarg", U16); op2(0x0a, "ldarga", U16); op2(0x0b, "starg", U16); op2(0x0c, "ldloc", U16);
        op2(0x0d, "ldloca", U16); op2(0x0e, "stloc", U16); op2(0x0f, "localloc", NONE); op2(0x11, "endfilter", NONE);
        op2(0x12, "unaligned.", I8); op2(0x13, "volatile.", NONE); op2(0x14, "tail.", NONE); op2(0x15, "initobj", TOK);
        op2(0x16, "constrained.", TOK); op2(0x17, "cpblk", NONE); op2(0x18, "initblk", NONE); op2(0x19, "no.", I8);
        op2(0x1a, "rethrow", NONE); op2(0x1c, "sizeof", TOK); op2(0x1d, "refanytype", NONE); op2(0x1e, "readonly.", NONE);
    }

    /** {codeStart, codeSize} file offsets of method m's IL, or null (abstract/extern). */
    int[] body(int m) {
        int rva = methodRva(m);
        if (rva == 0) return null;
        int p = off(rva);
        int h = b[p] & 0xff;
        if ((h & 3) == 2) return new int[] {p + 1, h >>> 2};
        int size = (bb.getShort(p) & 0xffff) >>> 12;
        return new int[] {p + size * 4, bb.getInt(p + 4)};
    }

    interface Visit { void at(int fileOff, String op, int kind, long val, int next); }

    void walk(int m, Visit v) {
        int[] bd = body(m);
        if (bd == null) return;
        int p = bd[0], end = bd[0] + bd[1];
        while (p < end) {
            int at = p, c = b[p++] & 0xff;
            String n; int k;
            if (c == 0xfe) { int c2 = b[p++] & 0xff; n = c2 < 64 ? NAMES2[c2] : null; k = c2 < 64 ? OPS2[c2] : NONE; }
            else { n = NAMES[c]; k = OPS[c]; }
            if (n == null) n = String.format("op_%02x", c);
            long val = 0;
            switch (k) {
                case I8: val = b[p]; p += 1; break;
                case U16: val = bb.getShort(p) & 0xffff; p += 2; break;
                case I32: case TOK: val = bb.getInt(p); p += 4; break;
                case R4: val = bb.getInt(p); p += 4; break;
                case I64: case R8: val = bb.getLong(p); p += 8; break;
                case BR8: val = b[p]; p += 1; break;
                case BR32: val = bb.getInt(p); p += 4; break;
                case SWITCH: { int cnt = bb.getInt(p); p += 4 + cnt * 4; val = cnt; break; }
                default: break;
            }
            v.at(at, n, k, val, p);
        }
    }

    public static void main(String[] a) throws Exception {
        IlDump d = new IlDump(Files.readAllBytes(Paths.get(a[0])));
        d.owners();
        String mode = a[1];
        Pattern re = Pattern.compile(a.length > 2 ? a[2] : ".", Pattern.CASE_INSENSITIVE);
        if (mode.equals("strings")) {
            int p = 1;
            while (p < d.usSize) {
                String s = d.usAt(p);
                if (re.matcher(s).find()) System.out.printf("us %06x  %s%n", p, s.replace("\n", "\\n"));
                int first = d.b[d.us + p] & 0xff;
                int hdr = (first & 0x80) == 0 ? 1 : (first & 0xC0) == 0x80 ? 2 : 4;
                int len = hdr == 1 ? first : hdr == 2 ? ((first & 0x3f) << 8) | (d.b[d.us + p + 1] & 0xff) : 0;
                if (hdr == 4) break;
                p += hdr + len;
            }
            return;
        }
        for (int m = 1; m <= d.rows[0x06]; m++) {
            final int mm = m;
            String full = d.methodFull(m);
            if (mode.equals("methods")) {
                if (re.matcher(full).find()) {
                    int[] bd = d.body(m);
                    System.out.printf("%s  il@%s size %d%n", full, bd == null ? "-" : Integer.toHexString(bd[0]),
                            bd == null ? 0 : bd[1]);
                }
            } else if (mode.equals("uses")) {
                List<String> hits = new ArrayList<>();
                d.walk(m, (at, op, kind, val, next) -> {
                    if (kind == TOK && (op.equals("ldstr") || op.startsWith("call") || op.equals("newobj") || op.endsWith("fld"))) {
                        String t = d.token((int) val);
                        if (re.matcher(t).find()) hits.add(String.format("%x %s %s", at, op, t));
                    }
                });
                if (!hits.isEmpty()) {
                    System.out.println(full);
                    for (String h : hits) System.out.println("    " + h);
                }
            } else if (mode.equals("il")) {
                if (!re.matcher(full).find()) continue;
                int[] bd = d.body(m);
                System.out.printf("== %s  (code @%x, %d bytes)%n", full, bd == null ? 0 : bd[0], bd == null ? 0 : bd[1]);
                d.walk(mm, (at, op, kind, val, next) -> {
                    String arg = "";
                    if (kind == TOK) arg = d.token((int) val);
                    else if (kind == BR8 || kind == BR32) arg = "-> " + Integer.toHexString((int) (next + val));
                    else if (kind != NONE) arg = String.valueOf(val);
                    System.out.printf("  %06x  %-12s %s%n", at, op, arg);
                });
            }
        }
    }
}
