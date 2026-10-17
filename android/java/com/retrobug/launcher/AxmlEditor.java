// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Reads and edits a compiled (binary, "AXML") AndroidManifest.xml.
//
// Why this exists: the repackager has to make a 32-bit game installable on a 64-bit-only phone, and the
// manifest is where the platform decides whether it will. Android 14+ refuses an APK whose targetSdkVersion
// is below 24, and old games ship with none at all (Gumball Lite: minSdkVersion only) or an old one (Angry
// Birds Go: 22). The phone has no aapt2, so the edit is done on the binary form directly.
//
// Why not a general AXML library: only a handful of reads and one write are needed, and the write has a
// structural catch — the resource map pairs attribute *names* with resource ids by position, so it indexes
// the first N pool strings. A new attribute name with an id therefore has to land at index N, which shifts
// every later string reference in every chunk. Doing that by hand over a minimal chunk model is smaller and
// easier to audit than carrying a dependency.
//
// Pure Java, no `android.*` import, so tools/run_java_tests.sh runs it on the PC against real aapt2 output.
// Chunk layouts follow frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h.
package com.retrobug.launcher;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

final class AxmlEditor {
    private static final int RES_XML_TYPE = 0x0003;
    private static final int RES_STRING_POOL_TYPE = 0x0001;
    private static final int RES_XML_RESOURCE_MAP_TYPE = 0x0180;
    private static final int XML_START_NAMESPACE = 0x0100;
    private static final int XML_END_NAMESPACE = 0x0101;
    private static final int XML_START_ELEMENT = 0x0102;
    private static final int XML_END_ELEMENT = 0x0103;
    private static final int XML_CDATA = 0x0104;

    private static final int POOL_UTF8_FLAG = 0x100;
    private static final int POOL_SORTED_FLAG = 0x1;
    private static final int TYPE_STRING = 0x03;
    private static final int TYPE_INT_DEC = 0x10;
    private static final int TYPE_INT_BOOLEAN = 0x12;
    private static final int ATTR_SIZE = 20;
    private static final int NODE_HEADER = 16;

    private static final int ATTR_MIN_SDK_VERSION = 0x0101020c;
    private static final int ATTR_TARGET_SDK_VERSION = 0x01010270;
    private static final String ANDROID_NS = "http://schemas.android.com/apk/res/android";

    private final List<String> strings = new ArrayList<>();
    private boolean utf8;
    private int poolFlags;
    private byte[] poolBytes;           // the original pool chunk; reused verbatim until a string is added
    private boolean poolDirty;
    private int[] resIds;               // null: the file had no resource map
    private final List<byte[]> nodes = new ArrayList<>();

    private AxmlEditor() {}

    // ---- little-endian accessors over a byte[]; ByteBuffer would be heavier than the three widths needed.
    private static int u16(byte[] b, int o) { return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8; }
    private static int s32(byte[] b, int o) {
        return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8 | (b[o + 2] & 0xff) << 16 | (b[o + 3] & 0xff) << 24;
    }
    private static void put16(byte[] b, int o, int v) { b[o] = (byte) v; b[o + 1] = (byte) (v >> 8); }
    private static void put32(byte[] b, int o, int v) {
        b[o] = (byte) v; b[o + 1] = (byte) (v >> 8); b[o + 2] = (byte) (v >> 16); b[o + 3] = (byte) (v >> 24);
    }

    static AxmlEditor parse(byte[] axml) throws IOException {
        try {
            return parseChunks(axml);
        } catch (IndexOutOfBoundsException | NegativeArraySizeException ex) {
            // Chunk fields are untrusted offsets and lengths; the checks below catch the likely lies, and this
            // is the backstop so no input ever escapes as an unnamed runtime exception.
            throw new IOException("binary XML malformed: " + ex, ex);
        }
    }

    private static AxmlEditor parseChunks(byte[] axml) throws IOException {
        if (axml.length < 8 || u16(axml, 0) != RES_XML_TYPE || u16(axml, 2) != 8 || s32(axml, 4) != axml.length) {
            throw new IOException("not a binary XML manifest (file header is not a RES_XML chunk covering the file)");
        }
        AxmlEditor e = new AxmlEditor();
        int pos = 8;
        while (pos < axml.length) {
            if (pos + 8 > axml.length) throw new IOException("binary XML truncated: chunk header at " + pos);
            int type = u16(axml, pos), size = s32(axml, pos + 4);
            if (size < 8 || size % 4 != 0 || pos + size > axml.length) {
                throw new IOException("binary XML chunk 0x" + Integer.toHexString(type) + " at " + pos
                        + " has bad size " + size);
            }
            byte[] chunk = new byte[size];
            System.arraycopy(axml, pos, chunk, 0, size);
            if (type == RES_STRING_POOL_TYPE) {
                e.readPool(chunk);
            } else if (type == RES_XML_RESOURCE_MAP_TYPE) {
                if (u16(chunk, 2) < 8 || u16(chunk, 2) > size) throw new IOException("resource map header size " + u16(chunk, 2) + " is outside its chunk");
                e.resIds = new int[(size - u16(chunk, 2)) / 4];
                for (int i = 0; i < e.resIds.length; i++) e.resIds[i] = s32(chunk, u16(chunk, 2) + i * 4);
            } else {
                checkNode(chunk, pos);
                e.nodes.add(chunk);
            }
            pos += size;
        }
        if (e.poolBytes == null) throw new IOException("binary XML has no string pool");
        return e;
    }

    /** Reject a node chunk too short for the fields the readers and shifters will touch in it. */
    private static void checkNode(byte[] c, int pos) throws IOException {
        int type = u16(c, 0), min;
        switch (type) {
            case XML_START_ELEMENT: min = NODE_HEADER + 20; break;
            case XML_START_NAMESPACE: case XML_END_NAMESPACE: case XML_END_ELEMENT: min = NODE_HEADER + 8; break;
            case XML_CDATA: min = NODE_HEADER + 12; break;
            default: min = 8;
        }
        if (c.length < min || (type >= XML_START_NAMESPACE && type <= XML_CDATA && u16(c, 2) != NODE_HEADER)) {
            throw new IOException("binary XML malformed: node 0x" + Integer.toHexString(type) + " at " + pos
                    + " is too short or has a bad header");
        }
        if (type == XML_START_ELEMENT) {
            long end = (long) NODE_HEADER + u16(c, NODE_HEADER + 8) + (long) attrCount(c) * u16(c, NODE_HEADER + 10);
            if (u16(c, NODE_HEADER + 10) < ATTR_SIZE || end > c.length) {
                throw new IOException("binary XML malformed: attributes of element at " + pos + " overrun the chunk");
            }
        }
    }

    // ---------------------------------------------------------------- string pool

    private void readPool(byte[] c) throws IOException {
        int hdr = u16(c, 2), count = s32(c, 8), styles = s32(c, 12), start = s32(c, 20);
        poolFlags = s32(c, 16);
        utf8 = (poolFlags & POOL_UTF8_FLAG) != 0;
        // Manifests carry no styled text; refusing beats silently dropping span data on the rewrite.
        if (styles != 0) throw new IOException("string pool has styles; not supported for a manifest");
        for (int i = 0; i < count; i++) {
            int p = start + s32(c, hdr + i * 4);
            if (p >= c.length) throw new IOException("string " + i + " offset " + p + " is outside the pool");
            strings.add(utf8 ? readUtf8(c, p) : readUtf16(c, p));
        }
        poolBytes = c;
    }

    private static String readUtf16(byte[] c, int p) {
        int len = u16(c, p);
        p += 2;
        if ((len & 0x8000) != 0) { len = ((len & 0x7fff) << 16) | u16(c, p); p += 2; }
        return new String(c, p, len * 2, StandardCharsets.UTF_16LE);
    }

    private static String readUtf8(byte[] c, int p) {
        int chars = c[p++] & 0xff;                       // UTF-16 length: skipped, only the byte length is needed
        if ((chars & 0x80) != 0) p++;
        int len = c[p++] & 0xff;
        if ((len & 0x80) != 0) len = ((len & 0x7f) << 8) | (c[p++] & 0xff);
        return new String(c, p, len, StandardCharsets.UTF_8);
    }

    private byte[] writePool() {
        ByteArrayOutputStream data = new ByteArrayOutputStream();
        int[] offs = new int[strings.size()];
        for (int i = 0; i < offs.length; i++) {
            offs[i] = data.size();
            String s = strings.get(i);
            if (utf8) {
                byte[] raw = s.getBytes(StandardCharsets.UTF_8);
                writeUtf8Len(data, s.length());
                writeUtf8Len(data, raw.length);
                data.write(raw, 0, raw.length);
                data.write(0);
            } else {
                int n = s.length();
                if (n > 0x7fff) { data.write((n >> 16) | 0x80); data.write(n >> 24); }
                data.write(n); data.write(n >> 8);
                byte[] raw = s.getBytes(StandardCharsets.UTF_16LE);
                data.write(raw, 0, raw.length);
                data.write(0); data.write(0);
            }
        }
        while (data.size() % 4 != 0) data.write(0);
        int hdr = 28, start = hdr + 4 * offs.length;
        byte[] out = new byte[start + data.size()];
        put16(out, 0, RES_STRING_POOL_TYPE);
        put16(out, 2, hdr);
        put32(out, 4, out.length);
        put32(out, 8, offs.length);
        put32(out, 12, 0);
        // A sorted pool promises binary-searchable order; an inserted string breaks that, so the claim is dropped.
        put32(out, 16, poolFlags & ~POOL_SORTED_FLAG);
        put32(out, 20, start);
        put32(out, 24, 0);
        for (int i = 0; i < offs.length; i++) put32(out, hdr + 4 * i, offs[i]);
        System.arraycopy(data.toByteArray(), 0, out, start, data.size());
        return out;
    }

    private static void writeUtf8Len(ByteArrayOutputStream o, int n) {
        if (n > 0x7f) { o.write((n >> 8) | 0x80); o.write(n); } else o.write(n);
    }

    // ---------------------------------------------------------------- reading

    private String str(int i) { return i < 0 || i >= strings.size() ? null : strings.get(i); }

    private int indexOf(String s) { return strings.indexOf(s); }

    /** Offset of attribute {@code i} inside a START_ELEMENT chunk. */
    private static int attrOff(byte[] c, int i) { return NODE_HEADER + u16(c, NODE_HEADER + 8) + i * u16(c, NODE_HEADER + 10); }

    private static int attrCount(byte[] c) { return u16(c, NODE_HEADER + 12); }

    /** First START_ELEMENT named {@code name}, or -1. */
    private int findElement(String name) {
        for (int i = 0; i < nodes.size(); i++) {
            byte[] c = nodes.get(i);
            if (u16(c, 0) == XML_START_ELEMENT && name.equals(str(s32(c, NODE_HEADER + 4)))) return i;
        }
        return -1;
    }

    /** Offset of the attribute named {@code attr} (any namespace is fine for a name unique per element), or -1. */
    private int findAttr(byte[] c, String attr) {
        for (int i = 0; i < attrCount(c); i++) {
            if (attr.equals(str(s32(c, attrOff(c, i) + 4)))) return attrOff(c, i);
        }
        return -1;
    }

    private Integer intAttr(String element, String attr) {
        int n = findElement(element);
        if (n < 0) return null;
        byte[] c = nodes.get(n);
        int a = findAttr(c, attr);
        return a < 0 ? null : s32(c, a + 16);
    }

    String packageName() {
        int n = findElement("manifest");
        if (n < 0) return null;
        byte[] c = nodes.get(n);
        int a = findAttr(c, "package");
        if (a < 0) return null;
        int raw = s32(c, a + 8);
        return raw >= 0 ? str(raw) : ((c[a + 15] & 0xff) == TYPE_STRING ? str(s32(c, a + 16)) : null);
    }

    Integer minSdk() { return intAttr("uses-sdk", "minSdkVersion"); }

    Integer targetSdk() { return intAttr("uses-sdk", "targetSdkVersion"); }

    /** Attribute names of the first {@code element}, in file order (tests check the resource-id ordering). */
    List<String> attrNames(String element) {
        List<String> out = new ArrayList<>();
        int n = findElement(element);
        if (n < 0) return out;
        byte[] c = nodes.get(n);
        for (int i = 0; i < attrCount(c); i++) out.add(str(s32(c, attrOff(c, i) + 4)));
        return out;
    }

    Boolean extractNativeLibs() {
        Integer v = intAttr("application", "extractNativeLibs");
        return v == null ? null : v != 0;
    }

    Boolean hasCode() {
        Integer v = intAttr("application", "hasCode");
        return v == null ? null : v != 0;
    }

    /** String value of attribute {@code attr} of the first {@code element} whose android:name is {@code name}. */
    String stringAttrOf(String element, String name, String attr) {
        for (byte[] c : nodes) {
            if (u16(c, 0) != XML_START_ELEMENT || !element.equals(str(s32(c, NODE_HEADER + 4)))) continue;
            if (!name.equals(stringValue(c, findAttr(c, "name")))) continue;
            return stringValue(c, findAttr(c, attr));
        }
        return null;
    }

    private String stringValue(byte[] c, int a) {
        if (a < 0) return null;
        int raw = s32(c, a + 8);
        return raw >= 0 ? str(raw) : ((c[a + 15] & 0xff) == TYPE_STRING ? str(s32(c, a + 16)) : null);
    }

    // ---------------------------------------------------------------- editing

    void setTargetSdk(int value) throws IOException { setUsesSdk("targetSdkVersion", ATTR_TARGET_SDK_VERSION, value); }

    void setMinSdk(int value) throws IOException { setUsesSdk("minSdkVersion", ATTR_MIN_SDK_VERSION, value); }

    /** Sets an int attribute of {@code <uses-sdk>}: rewritten in place, inserted, or with a new element. */
    private void setUsesSdk(String attr, int resId, int value) throws IOException {
        int us = findElement("uses-sdk");
        if (us >= 0) {
            int a = findAttr(nodes.get(us), attr);
            if (a >= 0) {                       // already there: rewrite the typed value, nothing else moves
                byte[] c = nodes.get(us);
                putTyped(c, a, value);
                return;
            }
        }
        int manifest = findElement("manifest");
        if (manifest < 0) throw new IOException("manifest has no <manifest> element");
        int nameIdx = ensureAttrName(attr, resId);
        int androidNs = indexOf(ANDROID_NS);
        if (androidNs < 0) throw new IOException("manifest declares no android namespace to attach " + attr + " to");
        if (us >= 0) {
            nodes.set(us, withAttribute(nodes.get(us), androidNs, nameIdx, resId, value));
            return;
        }
        // No <uses-sdk> at all: give <manifest> one as its first child.
        int tag = strings.indexOf("uses-sdk");
        if (tag < 0) { strings.add("uses-sdk"); tag = strings.size() - 1; poolDirty = true; }
        int line = s32(nodes.get(manifest), 8);
        byte[] start = new byte[NODE_HEADER + 20];
        put16(start, 0, XML_START_ELEMENT); put16(start, 2, NODE_HEADER); put32(start, 4, start.length);
        put32(start, 8, line); put32(start, 12, -1);
        put32(start, NODE_HEADER, -1); put32(start, NODE_HEADER + 4, tag);
        put16(start, NODE_HEADER + 8, 20); put16(start, NODE_HEADER + 10, ATTR_SIZE);
        byte[] end = new byte[NODE_HEADER + 8];
        put16(end, 0, XML_END_ELEMENT); put16(end, 2, NODE_HEADER); put32(end, 4, end.length);
        put32(end, 8, line); put32(end, 12, -1);
        put32(end, NODE_HEADER, -1); put32(end, NODE_HEADER + 4, tag);
        nodes.add(manifest + 1, end);
        nodes.add(manifest + 1, withAttribute(start, androidNs, nameIdx, resId, value));
    }

    private static final int ATTR_DEBUGGABLE = 0x0101000f;

    /**
     * Sets {@code <application android:debuggable="true">} — a diagnostics option (profiling with simpleperf,
     * {@code run-as} for the app's files), never a default: a debuggable app is slower and exposes its data.
     */
    void setDebuggable() throws IOException {
        int app = findElement("application");
        if (app < 0) throw new IOException("manifest has no <application> element");
        byte[] c = nodes.get(app);
        int off = findAttr(c, "debuggable");
        if (off < 0) {
            int nameIdx = ensureAttrName("debuggable", ATTR_DEBUGGABLE);  // first: it may insert into the pool
            int androidNs = indexOf(ANDROID_NS);
            if (androidNs < 0) throw new IOException("manifest declares no android namespace");
            c = withAttribute(nodes.get(findElement("application")), androidNs, nameIdx, ATTR_DEBUGGABLE, -1);
            nodes.set(app, c);
            off = findAttr(c, "debuggable");
        }
        c[off + 15] = TYPE_INT_BOOLEAN;
        put32(c, off + 16, -1);
        put32(c, off + 8, -1);  // no raw string value
    }

    private static final int ATTR_NAME = 0x01010003;
    private static final int ATTR_EXPORTED = 0x01010010;
    private static final int ATTR_AUTHORITIES = 0x01010018;
    private static final int ATTR_INIT_ORDER = 0x0101001a;

    /**
     * Adds {@code <provider android:name=cls android:exported="false" android:authorities=auth
     * android:initOrder=initOrder/>} as the last child of {@code <application>}. The targetSdk shims run from
     * that provider: the platform creates providers before Application.onCreate and before any activity or
     * service, and a high initOrder puts it ahead of the game's own providers.
     */
    void addProvider(String cls, String authorities, int initOrder) throws IOException {
        int app = findElement("application");
        if (app < 0) throw new IOException("manifest has no <application> element");
        // Names first: each insert shifts later pool indexes, so no index is taken until they are all in.
        int nName = ensureAttrName("name", ATTR_NAME);
        int nExported = ensureAttrName("exported", ATTR_EXPORTED);
        int nAuth = ensureAttrName("authorities", ATTR_AUTHORITIES);
        int nInit = ensureAttrName("initOrder", ATTR_INIT_ORDER);
        int ns = indexOf(ANDROID_NS);
        if (ns < 0) throw new IOException("manifest declares no android namespace to attach the provider to");
        int tag = poolString("provider"), vName = poolString(cls), vAuth = poolString(authorities);
        int end = -1;
        for (int i = nodes.size() - 1; i > app && end < 0; i--) {
            byte[] c = nodes.get(i);
            if (u16(c, 0) == XML_END_ELEMENT && "application".equals(str(s32(c, NODE_HEADER + 4)))) end = i;
        }
        if (end < 0) throw new IOException("<application> has no end element");
        int line = s32(nodes.get(end), 8);
        // Attributes in resource-id order, as aapt2 writes them: name < exported < authorities < initOrder.
        byte[] start = new byte[NODE_HEADER + 20 + 4 * ATTR_SIZE];
        put16(start, 0, XML_START_ELEMENT); put16(start, 2, NODE_HEADER); put32(start, 4, start.length);
        put32(start, 8, line); put32(start, 12, -1);
        put32(start, NODE_HEADER, -1); put32(start, NODE_HEADER + 4, tag);
        put16(start, NODE_HEADER + 8, 20); put16(start, NODE_HEADER + 10, ATTR_SIZE);
        put16(start, NODE_HEADER + 12, 4);
        int a = NODE_HEADER + 20;
        putStringAttr(start, a, ns, nName, vName);
        put32(start, a + ATTR_SIZE, ns); put32(start, a + ATTR_SIZE + 4, nExported);
        put32(start, a + ATTR_SIZE + 8, -1); put16(start, a + ATTR_SIZE + 12, 8);
        start[a + ATTR_SIZE + 15] = TYPE_INT_BOOLEAN; put32(start, a + ATTR_SIZE + 16, 0);
        putStringAttr(start, a + 2 * ATTR_SIZE, ns, nAuth, vAuth);
        put32(start, a + 3 * ATTR_SIZE, ns); put32(start, a + 3 * ATTR_SIZE + 4, nInit);
        putTyped(start, a + 3 * ATTR_SIZE, initOrder);
        byte[] close = new byte[NODE_HEADER + 8];
        put16(close, 0, XML_END_ELEMENT); put16(close, 2, NODE_HEADER); put32(close, 4, close.length);
        put32(close, 8, line); put32(close, 12, -1);
        put32(close, NODE_HEADER, -1); put32(close, NODE_HEADER + 4, tag);
        nodes.add(end, close);
        nodes.add(end, start);
    }

    // ---------------------------------------------------------------- package rename

    /** Attributes holding a class name, by element; a relative one resolves against the manifest package. */
    private static final String[][] CLASS_ATTRS = {
            {"application", "name", "backupAgent", "manageSpaceActivity"},
            {"activity", "name", "parentActivityName"},
            {"activity-alias", "name", "targetActivity"},
            {"service", "name"}, {"receiver", "name"}, {"provider", "name"}, {"instrumentation", "name"},
    };
    private static final String[] PERMISSION_DECLS = {"permission", "permission-tree", "permission-group"};
    private static final String[] PERMISSION_REFS = {"name", "permission", "readPermission", "writePermission", "permissionGroup"};

    private static boolean in(String s, String... set) {
        for (String x : set) if (x.equals(s)) return true;
        return false;
    }

    private static boolean isClassAttr(String element, String attr) {
        for (String[] row : CLASS_ATTRS) {
            if (!row[0].equals(element)) continue;
            for (int i = 1; i < row.length; i++) if (row[i].equals(attr)) return true;
        }
        return false;
    }

    /** {@code name} as the platform resolves it against package {@code pkg} (PackageParser.buildClassName). */
    static String fullClassName(String pkg, String name) {
        if (name.startsWith(".")) return pkg + name;
        return name.indexOf('.') < 0 ? pkg + "." + name : name;
    }

    /**
     * {@code v} (an authority or a declared permission) moved from {@code old} to {@code neu}: a name under the old
     * package keeps its tail; any other gets the new package's tail (or the whole new package) appended, because
     * the platform refuses a second app declaring the same authority or permission whatever its prefix.
     */
    static String renamedName(String old, String neu, String v) {
        if (v.equals(old) || v.startsWith(old + ".")) return neu + v.substring(old.length());
        return v + (neu.startsWith(old + ".") ? neu.substring(old.length()) : "." + neu);
    }

    private static final java.util.regex.Pattern PACKAGE_NAME =
            java.util.regex.Pattern.compile("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+");

    /**
     * Renames the manifest package to {@code neu} so the app installs beside the original. The DEX keeps its
     * classes, so every class name the manifest gives relative to the old package (".Main", "Main") is written
     * out in full against the old package first; provider authorities and declared permissions are renamed
     * (they must be unique on the device), with every reference to a renamed permission; an intent-filter
     * category equal to the old package (GCM/C2DM) and taskAffinity/process names under it follow the package.
     * Returns one report line per change.
     */
    List<String> renamePackage(String neu) throws IOException {
        String old = packageName();
        if (old == null) throw new IOException("manifest has no package to rename");
        if (!PACKAGE_NAME.matcher(neu).matches()) throw new IOException("not a valid package name: " + neu);
        if (neu.equals(old)) throw new IOException("new package is the same as the old one: " + neu);
        List<String> out = new ArrayList<>();
        java.util.Map<String, String> perms = new java.util.LinkedHashMap<>();
        for (byte[] c : nodes) {
            if (u16(c, 0) != XML_START_ELEMENT || !in(str(s32(c, NODE_HEADER + 4)), PERMISSION_DECLS)) continue;
            String v = stringValue(c, findAttr(c, "name"));
            if (v != null) perms.put(v, renamedName(old, neu, v));
        }
        boolean renamed = false;
        for (byte[] c : nodes) {
            if (u16(c, 0) != XML_START_ELEMENT) continue;
            String el = str(s32(c, NODE_HEADER + 4));
            for (int i = 0; i < attrCount(c); i++) {
                int a = attrOff(c, i);
                String attr = str(s32(c, a + 4)), v = stringValue(c, a);
                if (attr == null || v == null) continue;
                String nv = null;
                if ("manifest".equals(el) && "package".equals(attr)) {
                    nv = neu;
                    renamed = true;
                } else if (isClassAttr(el, attr) && !v.isEmpty()) {
                    nv = fullClassName(old, v);
                } else if ("meta-data".equals(el) && "value".equals(attr) && v.startsWith(".")
                        && "android.support.PARENT_ACTIVITY".equals(stringValue(c, findAttr(c, "name")))) {
                    nv = old + v;                      // NavUtils resolves it against getPackageName()
                } else if ("provider".equals(el) && "authorities".equals(attr)) {
                    StringBuilder sb = new StringBuilder();
                    for (String auth : v.split(";", -1)) {
                        if (sb.length() > 0) sb.append(';');
                        sb.append(auth.isEmpty() ? auth : renamedName(old, neu, auth));
                    }
                    nv = sb.toString();
                } else if (perms.containsKey(v) && in(attr, PERMISSION_REFS)) {
                    nv = perms.get(v);
                } else if ("category".equals(el) && "name".equals(attr) && v.equals(old)) {
                    nv = neu;
                } else if (in(attr, "taskAffinity", "process") && (v.equals(old) || v.startsWith(old + "."))) {
                    nv = neu + v.substring(old.length());
                }
                if (nv == null || nv.equals(v)) continue;
                int idx = poolString(nv);
                put32(c, a + 8, idx);
                put16(c, a + 12, 8); c[a + 14] = 0; c[a + 15] = TYPE_STRING; put32(c, a + 16, idx);
                out.add(el + " " + attr + ": " + v + " -> " + nv);
            }
        }
        if (!renamed) throw new IOException("manifest package attribute not found on <manifest>");
        return out;
    }

    private static void putStringAttr(byte[] c, int a, int ns, int name, int value) {
        put32(c, a, ns); put32(c, a + 4, name); put32(c, a + 8, value);
        put16(c, a + 12, 8); c[a + 14] = 0; c[a + 15] = TYPE_STRING; put32(c, a + 16, value);
    }

    /** Pool index of {@code s}, appended when missing (values need no resource-map slot). */
    private int poolString(String s) {
        int i = indexOf(s);
        if (i >= 0) return i;
        strings.add(s);
        poolDirty = true;
        return strings.size() - 1;
    }

    private static void putTyped(byte[] c, int a, int value) {
        put32(c, a + 8, -1);                    // no raw string: the typed value is the whole attribute
        put16(c, a + 12, 8);
        c[a + 14] = 0;
        c[a + 15] = TYPE_INT_DEC;
        put32(c, a + 16, value);
    }

    /**
     * Pool index of the attribute name {@code name} (resource id {@code resId}). Added at index ==
     * resource-map length when missing, because the map pairs ids with the first N strings by position;
     * every later reference shifts.
     */
    private int ensureAttrName(String name, int resId) {
        if (resIds == null) resIds = new int[0];
        for (int i = 0; i < resIds.length; i++) if (resIds[i] == resId) return i;
        int at = resIds.length;
        strings.add(at, name);
        poolDirty = true;
        for (byte[] c : nodes) shiftStringRefs(c, at);
        int[] grown = new int[at + 1];
        System.arraycopy(resIds, 0, grown, 0, at);
        grown[at] = resId;
        resIds = grown;
        return at;
    }

    /** A copy of START_ELEMENT {@code c} with one int attribute added in resource-id order. */
    private byte[] withAttribute(byte[] c, int ns, int name, int resId, int value) throws IOException {
        if (u16(c, NODE_HEADER + 10) != ATTR_SIZE) throw new IOException("unexpected attribute size " + u16(c, NODE_HEADER + 10));
        int n = attrCount(c), at = n;
        // aapt2 keeps attributes sorted by resource id; names without an id sort after all that have one.
        for (int i = 0; i < n; i++) {
            int nm = s32(c, attrOff(c, i) + 4);
            if (nm >= 0 && nm < resIds.length && resIds[nm] > resId || nm >= resIds.length) { at = i; break; }
        }
        int base = attrOff(c, 0), ins = base + at * ATTR_SIZE;
        byte[] out = new byte[c.length + ATTR_SIZE];
        System.arraycopy(c, 0, out, 0, ins);
        System.arraycopy(c, ins, out, ins + ATTR_SIZE, c.length - ins);
        put32(out, 4, out.length);
        put16(out, NODE_HEADER + 12, n + 1);
        // idIndex/classIndex/styleIndex are 1-based attribute positions; keep any that pointed past the insert.
        for (int f = NODE_HEADER + 14; f <= NODE_HEADER + 18; f += 2) if (u16(out, f) > at) put16(out, f, u16(out, f) + 1);
        put32(out, ins, ns);
        put32(out, ins + 4, name);
        putTyped(out, ins, value);
        return out;
    }

    /** Add one to every string-pool reference in node {@code c} that is >= {@code at}. */
    private static void shiftStringRefs(byte[] c, int at) {
        int type = u16(c, 0);
        bump(c, 12, at);                                   // comment
        switch (type) {
            case XML_START_NAMESPACE: case XML_END_NAMESPACE: case XML_END_ELEMENT:
                bump(c, NODE_HEADER, at); bump(c, NODE_HEADER + 4, at);
                break;
            case XML_START_ELEMENT:
                bump(c, NODE_HEADER, at); bump(c, NODE_HEADER + 4, at);
                for (int i = 0; i < attrCount(c); i++) {
                    int a = attrOff(c, i);
                    bump(c, a, at); bump(c, a + 4, at); bump(c, a + 8, at);
                    if ((c[a + 15] & 0xff) == TYPE_STRING) bump(c, a + 16, at);
                }
                break;
            case XML_CDATA:
                bump(c, NODE_HEADER, at);
                if ((c[NODE_HEADER + 7] & 0xff) == TYPE_STRING) bump(c, NODE_HEADER + 8, at);
                break;
            default:
                break;                                     // unknown chunk kinds are carried through untouched
        }
    }

    private static void bump(byte[] c, int off, int at) {
        int v = s32(c, off);
        if (v >= at) put32(c, off, v + 1);
    }

    byte[] toBytes() {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] pool = poolDirty ? writePool() : poolBytes;
        o.write(pool, 0, pool.length);
        if (resIds != null) {
            byte[] map = new byte[8 + 4 * resIds.length];
            put16(map, 0, RES_XML_RESOURCE_MAP_TYPE); put16(map, 2, 8); put32(map, 4, map.length);
            for (int i = 0; i < resIds.length; i++) put32(map, 8 + 4 * i, resIds[i]);
            o.write(map, 0, map.length);
        }
        for (byte[] c : nodes) o.write(c, 0, c.length);
        byte[] body = o.toByteArray();
        byte[] out = new byte[8 + body.length];
        put16(out, 0, RES_XML_TYPE); put16(out, 2, 8); put32(out, 4, out.length);
        System.arraycopy(body, 0, out, 8, body.length);
        return out;
    }
}
