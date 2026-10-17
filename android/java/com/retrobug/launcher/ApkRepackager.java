// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
package com.retrobug.launcher;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.GregorianCalendar;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * Rewrites a 32-bit game's APK so a 64-bit-only device will install it: the 32-bit libs move to
 * {@code assets/rb32/} (where the runtime reads them), each is replaced by a 64-bit stub under
 * {@code lib/arm64-v8a/} (the platform picks the app's ABI from that directory, and the stub's job is to boot
 * the guest engine), and targetSdk is raised to the install floor. Because that raise switches on behaviour
 * changes the old Java was not written for, the targetSdk compatibility shims (ShimPlan) are added: a provider in
 * the manifest, its classes as the next classesN.dex, its config under {@code assets/rb32/}; the report names
 * the risky calls found in the game's DEX. The result is unsigned: signing is a
 * separate step, and the old signature files are dropped because they would no longer verify.
 *
 * Pure java.util.zip and no android.* imports, so it is host-testable.
 */
final class ApkRepackager {
    private ApkRepackager() {}

    /** Lowest targetSdk the platform will install without a "built for an older Android" block. */
    static final int INSTALL_FLOOR_SDK = 24;
    /** minSdk the output declares at least (never lowered); RepackSigner signs for the same level. */
    static final int MIN_SDK_FLOOR = 24;
    static final String STUB_KEY = "stub";
    /** Diagnostics: the next transform marks the output debuggable (`-e debuggable 1`). Never set by the UI. */
    static volatile boolean debuggable = false;
    /** Diagnostics: the next transform is a native control (`-e native 1`): renamed and re-signed like a repack, but the
     *  32-bit libraries stay in lib/ and no runtime is added, so a 32-bit-capable device runs the same APK natively. */
    static volatile boolean nativeControl = false;
    /** Diagnostics: the next transform targets at least this SDK (`-e target N`; 0 = the install floor). Play Protect
     *  now blocks "built for an older Android" installs at the floor on some devices, and a modern target installs. */
    static volatile int targetFloor = 0;

    private static final String V7A = "lib/armeabi-v7a/";
    private static final String ARM = "lib/armeabi/";
    private static final String ARM64 = "lib/arm64-v8a/";
    private static final String GUEST_DIR = "assets/rb32/";
    private static final String MANIFEST = "AndroidManifest.xml";
    private static final String ARSC = "resources.arsc";

    static final class Result {
        String packageName;      // the output's package (the new one when renamed)
        String renamedFrom;      // the original package when renamed, else null
        int oldTargetSdk = -1;   // -1: the manifest declared none
        int newTargetSdk;
        int oldMinSdk = -1;      // -1: the manifest declared none (the platform reads that as 1)
        int newMinSdk;
        List<String> guestLibs = new ArrayList<>();
        List<String> stubs = new ArrayList<>();
        Map<String, List<String>> stubExports = new TreeMap<>();  // generated stubs: lib -> exported symbols
        List<String> report = new ArrayList<>();
        List<String> shims = new ArrayList<>();   // targetSdk compatibility shims added (ShimPlan ids)
        String shimDex;                           // classesN.dex entry carrying them, or null
        List<String> risks = new ArrayList<>();   // ShimPlan.riskReport lines (also in report)
    }

    private static IOException refused(String why) { return new IOException("refused: " + why); }

    static Result transform(File inApk, Map<String, byte[]> runtimeFiles, File outUnsigned) throws IOException {
        return transform(inApk, runtimeFiles, outUnsigned, null);
    }

    /** {@code renameTo} value that appends this to the original package: the launcher's "install beside" name. */
    static final String RENAME_SUFFIX = ".rb";

    /**
     * {@code renameTo}: null keeps the package; a value starting with '.' is appended to the original package;
     * anything else is the new package. A renamed app installs beside the original instead of replacing it
     * (AxmlEditor.renamePackage). resources.arsc's package name follows, so getIdentifier(..., getPackageName())
     * still finds the app's own resources.
     */
    static Result transform(File inApk, Map<String, byte[]> runtimeFiles, File outUnsigned, String renameTo)
            throws IOException {
        byte[] stub = runtimeFiles.get(STUB_KEY);
        if (stub == null) throw new IOException("runtimeFiles has no \"" + STUB_KEY + "\" entry");
        try {
            return run(inApk, stub, runtimeFiles, outUnsigned, renameTo);
        } catch (IOException | RuntimeException e) {
            // A half-written zip must never be mistaken for a finished one by the signing step.
            Files.deleteIfExists(outUnsigned.toPath());
            throw e;
        }
    }

    private static Result run(File inApk, byte[] stub, Map<String, byte[]> runtimeFiles, File out, String renameTo)
            throws IOException {
        Result r = new Result();
        try (ZipFile zip = new ZipFile(inApk)) {
            ZipEntry me = zip.getEntry(MANIFEST);
            if (me == null) throw new IOException("no " + MANIFEST + " in " + inApk);
            byte[] manifest;
            try (InputStream in = zip.getInputStream(me)) { manifest = readFully(in); }
            AxmlEditor ed = AxmlEditor.parse(manifest);
            r.packageName = ed.packageName();

            // With extraction off the platform maps libs straight out of the APK, so the stubs would have to
            // be the real libs; there is nothing for the guest engine to load.
            if (Boolean.FALSE.equals(ed.extractNativeLibs()))
                throw refused("extractNativeLibs=\"false\" (libs are mapped from the APK, stubs would not load)");

            boolean hasV7a = false;
            for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                if (e.isDirectory()) continue;
                if (e.getName().startsWith(ARM64))
                    throw refused("already has " + e.getName() + " (app is not 32-bit only)");
                if (e.getName().startsWith(V7A)) hasV7a = true;
            }
            // armeabi libs run on v7a hardware, so an armeabi-only app is a valid input; they are only
            // redundant (and dropped) when a v7a directory supplies the real set.
            String guestDir = hasV7a ? V7A : ARM;

            // minSdk is raised to the same floor: the output only ever runs on a 64-bit device (the stubs are
            // arm64), and a declared minSdk below 18 makes the v1 signature's SHA-256 digest "unsupported" to
            // `apksigner verify`, which checks v1 down to the manifest's minSdk.
            boolean edited = false;
            if (renameTo != null) {
                if (r.packageName == null) throw refused("manifest names no package to rename");
                String neu = renameTo.startsWith(".") ? r.packageName + renameTo : renameTo;
                List<String> changes = ed.renamePackage(neu);
                r.renamedFrom = r.packageName;
                r.packageName = neu;
                edited = true;
                r.report.add("package " + r.renamedFrom + " -> " + neu + " (installs beside the original)");
                for (String c : changes) r.report.add("rename: " + c);
            }
            Integer oldMin = ed.minSdk();
            r.oldMinSdk = oldMin == null ? -1 : oldMin;
            if (oldMin != null && oldMin >= MIN_SDK_FLOOR) {
                r.newMinSdk = oldMin;    // never lowered
                r.report.add("minSdk " + oldMin + " already >= " + MIN_SDK_FLOOR + ", left unchanged");
            } else {
                ed.setMinSdk(MIN_SDK_FLOOR);
                edited = true;
                r.newMinSdk = MIN_SDK_FLOOR;
                r.report.add("minSdk " + (oldMin == null ? "absent" : oldMin) + " -> " + MIN_SDK_FLOOR);
            }

            Integer oldT = ed.targetSdk();
            r.oldTargetSdk = oldT == null ? -1 : oldT;
            // Never below the install floor nor below the (new) minSdk; never lowered.
            int floorT = Math.max(INSTALL_FLOOR_SDK, r.newMinSdk);
            // A native control is a diagnostic install on a device whose Play Protect blocks old targets: current.
            if (nativeControl) floorT = Math.max(floorT, 33);  // 34 makes registerReceiver without RECEIVER_* fatal
            floorT = Math.max(floorT, targetFloor);
            if (oldT == null && oldMin != null && oldMin >= floorT) {
                // An absent targetSdk means "= minSdk"; writing the floor here would lower it.
                r.newTargetSdk = oldMin;
                r.report.add("targetSdk absent (defaults to minSdk " + oldMin + "), left unchanged");
            } else if (oldT != null && oldT >= floorT) {
                r.newTargetSdk = oldT;
                r.report.add("targetSdk " + oldT + " already >= " + floorT + ", left unchanged");
            } else {
                ed.setTargetSdk(floorT);
                edited = true;
                r.newTargetSdk = floorT;
                r.report.add("targetSdk " + (oldT == null ? "absent" : oldT) + " -> " + floorT);
            }
            // targetSdk compatibility shims (ShimPlan): decided from the target the original ran on.
            ShimPlan plan = ShimPlan.of(ShimPlan.effectiveTarget(oldT, oldMin), r.newTargetSdk);
            byte[] shimDex = runtimeFiles.get(ShimPlan.SHIM_DEX_KEY);
            boolean addShims = false;
            if (plan.shims.isEmpty()) {
                r.report.add("shims: none needed (original target " + plan.originalTarget + ")");
            } else if (Boolean.FALSE.equals(ed.hasCode())) {
                r.report.add("shims: NOT added — application hasCode=\"false\" (no Java to run them); needed: " + plan.shims);
            } else if (shimDex == null) {
                r.report.add("shims: NOT added — this launcher build has no shim dex; needed: " + plan.shims);
            } else {
                ed.addProvider(ShimPlan.PROVIDER_CLASS, r.packageName + ShimPlan.AUTHORITY_SUFFIX, Integer.MAX_VALUE);
                edited = true;
                addShims = true;
            }
            if (debuggable) {  // diagnostics only (`-e debuggable 1`): simpleperf and run-as on the repack
                ed.setDebuggable();
                edited = true;
                r.report.add("debuggable: android:debuggable=\"true\" set for diagnostics (profiling, run-as) — not for players");
            }
            if (r.newTargetSdk >= 31) {  // API 31+ refuses an intent-filtered component with no explicit exported
                int n = ed.setExportedDefaults();
                if (n > 0) {
                    edited = true;
                    r.report.add("exported: " + n + " component(s) with intent filters given android:exported=\"true\" (their default before API 31)");
                }
            }
            // API 28 took the Apache HTTP client off the default classpath; an app built before it got it for free.
            if (ShimPlan.effectiveTarget(oldT, oldMin) < 28 && r.newTargetSdk >= 28
                    && ed.addUsesLibrary("org.apache.http.legacy", false)) {
                edited = true;
                r.report.add("uses-library: org.apache.http.legacy (required=false), on the classpath before API 28");
            }
            if (edited) manifest = ed.toBytes();

            List<ZipEntry> guest = new ArrayList<>();
            List<String> sigDropped = new ArrayList<>();
            Set<String> names = new HashSet<>();
            Set<String> refs = new TreeSet<>();
            Map<String, NativeMethods.Native> natives = new TreeMap<>();  // every classesN.dex's, by JNI name
            Map<String, byte[]> libBytes = new HashMap<>();
            try (ZipOutputStream zo = new ZipOutputStream(Files.newOutputStream(out.toPath()))) {
                putBytes(zo, MANIFEST, manifest, ZipEntry.DEFLATED);
                names.add(MANIFEST);
                for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
                    ZipEntry e = en.nextElement();
                    String n = e.getName();
                    if (e.isDirectory() || n.equals(MANIFEST)) continue;
                    if (n.startsWith("META-INF/")) { sigDropped.add(n); continue; }
                    if (nativeControl && n.startsWith("lib/")) {  // the control run keeps the 32-bit libraries as they are
                        claim(names, n);
                        copy(zip, e, zo);
                        continue;
                    }
                    if (n.startsWith("lib/")) {
                        if (n.startsWith(guestDir)) { guest.add(e); continue; }
                        r.report.add("dropped " + n + (n.startsWith(ARM) ? " (lib/armeabi-v7a/ supplies the 32-bit set)"
                                : " (not a 32-bit ARM lib)"));
                        continue;
                    }
                    claim(names, n);
                    if (n.matches("classes\\d*\\.dex")) {
                        byte[] dex;
                        try (InputStream in = zip.getInputStream(e)) { dex = readFully(in); }
                        try {
                            refs.addAll(DexScanner.methodRefs(dex));
                        } catch (IOException ex) {
                            r.report.add("risk scan: " + n + " unreadable — " + ex.getMessage());
                        }
                        try {
                            natives.putAll(NativeMethods.natives(dex));
                        } catch (IOException ex) {
                            r.report.add("natives: " + n + " unreadable (its natives get no generated stub export) — "
                                    + ex.getMessage());
                        }
                        putBytes(zo, n, dex, e.getMethod() == ZipEntry.STORED ? ZipEntry.STORED : ZipEntry.DEFLATED);
                        continue;
                    }
                    if (r.renamedFrom != null && n.equals(ARSC)) {
                        byte[] arsc;
                        try (InputStream in = zip.getInputStream(e)) { arsc = readFully(in); }
                        int k = renameArscPackage(arsc, r.renamedFrom, r.packageName);
                        r.report.add(ARSC + ": " + (k == 0 ? "no package named " + r.renamedFrom + ", left unchanged"
                                : "package name -> " + r.packageName));
                        putBytes(zo, n, arsc, ZipEntry.STORED);  // API 30+ installs only a STORED, aligned one
                        continue;
                    }
                    copy(zip, e, zo);
                }
                r.risks = plan.riskReport(refs, addShims ? plan.shims : Collections.<String>emptyList());
                r.report.addAll(r.risks);
                if (nativeControl) {
                    r.report.add("native control: 32-bit libraries kept in lib/, no RetroBug runtime — runs natively on a 32-bit-capable device only");
                } else if (guest.isEmpty()) throw refused("no 32-bit ARM native libs (lib/armeabi-v7a or lib/armeabi)");
                if (!sigDropped.isEmpty())
                    r.report.add("dropped " + sigDropped.size() + " META-INF entries (old signature no longer valid): "
                            + sigDropped);

                List<String> stubs = new ArrayList<>();
                for (ZipEntry e : guest) {
                    String name = e.getName().substring(guestDir.length());
                    r.guestLibs.add(name);
                    String dest = GUEST_DIR + name;
                    claim(names, dest);
                    byte[] lib;
                    try (InputStream in = zip.getInputStream(e)) { lib = readFully(in); }
                    putBytes(zo, dest, lib, ZipEntry.DEFLATED);
                    libBytes.put(name, lib);
                    // Only .so files are loaded by name through the linker; anything else (gdbserver, data
                    // blobs) is carried along for the guest but gets no stub the platform would try to load.
                    if (name.endsWith(".so") && name.indexOf('/') < 0) stubs.add(name);
                    else r.report.add(name + ": no stub (not a top-level .so); kept only under " + GUEST_DIR);
                }
                Collections.sort(stubs);
                for (String s : stubs) {
                    claim(names, ARM64 + s);
                    putBytes(zo, ARM64 + s, stubFor(s, libBytes.get(s), natives, stub, r), ZipEntry.DEFLATED);
                }
                r.stubs = stubs;

                if (addShims) {
                    // The next free classesN.dex: the platform loads classes.dex, classes2.dex, ... up to the
                    // first gap, so the shim's must continue the run.
                    String dexName = "classes.dex";
                    for (int i = 2; names.contains(dexName); i++) dexName = "classes" + i + ".dex";
                    claim(names, dexName);
                    putBytes(zo, dexName, shimDex, ZipEntry.DEFLATED);
                    claim(names, ShimPlan.CONFIG_ENTRY);
                    putBytes(zo, ShimPlan.CONFIG_ENTRY, plan.config().getBytes(StandardCharsets.UTF_8), ZipEntry.DEFLATED);
                    r.shims = new ArrayList<>(plan.shims);
                    r.shimDex = dexName;
                    r.report.add("shims: " + plan.shims + " (in-process targetSdk " + plan.originalTarget + ") — provider "
                            + ShimPlan.PROVIDER_CLASS + " in " + dexName);
                }

                for (Map.Entry<String, byte[]> f : runtimeFiles.entrySet()) {
                    if (nativeControl && (f.getKey().startsWith("lib/") || f.getKey().startsWith("assets/rb32"))) continue;
                    if (f.getKey().equals(STUB_KEY) || f.getKey().equals(ShimPlan.SHIM_DEX_KEY)) continue;
                    claim(names, f.getKey());
                    putBytes(zo, f.getKey(), f.getValue(), ZipEntry.DEFLATED);
                }
            }
        }
        return r;
    }

    /**
     * The 64-bit stand-in for {@code lib}: a generated stub (StubElfWriter) exporting every {@code Java_*} name the
     * library defines that a DEX native carries, so ART binds them by its own lookup; or the prebuilt stub (which
     * only boots the library) when the library exports none and no {@code JNI_OnLoad}, or cannot be read.
     */
    static byte[] stubFor(String libName, byte[] lib, Map<String, NativeMethods.Native> natives, byte[] prebuilt,
                          Result r) {
        List<String> report = r.report;
        Set<String> defined;
        try {
            defined = StubElfWriter.jniExportsOf(lib);
        } catch (IOException ex) {
            report.add(libName + ": prebuilt stub (its exports are unreadable: " + ex.getMessage() + ")");
            return prebuilt;
        }
        List<StubElfWriter.Export> exports = new ArrayList<>();
        int unbound = 0;
        for (String sym : defined) {
            if (sym.equals(StubElfWriter.ONLOAD)) continue;
            NativeMethods.Native n = natives.get(sym);
            if (n == null) { unbound++; continue; }  // no Java declaration names it: ART never looks it up
            exports.add(new StubElfWriter.Export(sym, NativeMethods.shortyOf(n.descriptor), n.isStatic));
        }
        if (exports.isEmpty() && !defined.contains(StubElfWriter.ONLOAD)) {
            report.add(libName + ": prebuilt stub (no Java_* the DEX declares, no JNI_OnLoad)");
            return prebuilt;
        }
        report.add(libName + ": generated stub, " + exports.size() + " Java_* export(s) + JNI_OnLoad"
                + (unbound == 0 ? "" : "; " + unbound + " Java_* with no native declaration in the DEX left out"));
        List<String> names = new ArrayList<>();
        names.add(StubElfWriter.ONLOAD);
        for (StubElfWriter.Export e : exports) names.add(e.symbol);
        r.stubExports.put(libName, names);
        return StubElfWriter.write(libName, exports);
    }

    /**
     * Renames, in place, every top-level ResTable_package chunk of {@code arsc} named {@code old} to {@code neu}
     * (the name is a fixed char16[128] field, so nothing moves). Returns how many were renamed; a table it cannot
     * walk is left as it is (0). Layout: frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h.
     */
    static int renameArscPackage(byte[] arsc, String old, String neu) throws IOException {
        if (neu.length() > 127) throw refused("package name too long for resources.arsc: " + neu);
        if (arsc.length < 12 || le16(arsc, 0) != 0x0002) return 0;
        int n = 0;
        for (int pos = le16(arsc, 2); pos + 8 <= arsc.length; ) {
            int type = le16(arsc, pos), size = le32(arsc, pos + 4);
            if (size < 8 || size > arsc.length - pos) break;
            if (type == 0x0200 && size >= 12 + 256) {
                StringBuilder name = new StringBuilder();
                for (int i = 0; i < 128 && le16(arsc, pos + 12 + 2 * i) != 0; i++) name.append((char) le16(arsc, pos + 12 + 2 * i));
                if (name.toString().equals(old)) {
                    for (int i = 0; i < 128; i++) {
                        int ch = i < neu.length() ? neu.charAt(i) : 0;
                        arsc[pos + 12 + 2 * i] = (byte) ch;
                        arsc[pos + 13 + 2 * i] = (byte) (ch >> 8);
                    }
                    n++;
                }
            }
            pos += size;
        }
        return n;
    }

    private static int le16(byte[] b, int o) { return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8; }
    private static int le32(byte[] b, int o) { return le16(b, o) | le16(b, o + 2) << 16; }

    private static void claim(Set<String> names, String n) throws IOException {
        if (!names.add(n)) throw refused("duplicate output entry " + n);
    }

    // Fixed timestamp: the same input and runtime must give byte-identical output.
    private static void stamp(ZipEntry e) {
        // Calendar fields in the default zone: ZipEntry stores local DOS time, so this is 2008-01-01 00:00 in every zone.
        e.setTime(new GregorianCalendar(2008, 0, 1, 0, 0, 0).getTimeInMillis());
    }

    // InputStream.readAllBytes is API 33; this class runs on-device from API 26.
    private static byte[] readFully(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        for (int n; (n = in.read(buf)) > 0; ) bo.write(buf, 0, n);
        return bo.toByteArray();
    }

    private static void putBytes(ZipOutputStream zo, String name, byte[] data, int method) throws IOException {
        ZipEntry e = new ZipEntry(name);
        e.setMethod(method);
        if (method == ZipEntry.STORED) {
            CRC32 c = new CRC32(); c.update(data);
            e.setSize(data.length); e.setCompressedSize(data.length); e.setCrc(c.getValue());
        }
        stamp(e);
        zo.putNextEntry(e);
        zo.write(data);
        zo.closeEntry();
    }

    /**
     * Copies an entry keeping its compression method, except resources.arsc, which is always STORED: a package
     * targeting API 30+ is refused unless its resources.arsc is uncompressed and 4-byte aligned (RepackSigner aligns
     * STORED entries), and some games ship it compressed.
     */
    private static void copy(ZipFile zip, ZipEntry src, ZipOutputStream zo) throws IOException {
        byte[] data;
        try (InputStream in = zip.getInputStream(src)) { data = readFully(in); }
        final boolean stored = src.getMethod() == ZipEntry.STORED || src.getName().equals(ARSC);
        putBytes(zo, src.getName(), data, stored ? ZipEntry.STORED : ZipEntry.DEFLATED);
    }
}
